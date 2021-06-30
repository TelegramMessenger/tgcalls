/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#import "TGRTCMTLRenderer+Private.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import "base/RTCLogging.h"
#import "base/RTCVideoFrame.h"
#import "base/RTCVideoFrameBuffer.h"
#import "TGRTCMetalContextHolder.h"

#include "api/video/video_rotation.h"
#include "rtc_base/checks.h"


MTLFrameSize MTLAspectFitted(MTLFrameSize from, MTLFrameSize to) {
    double scale = std::min(
        from.width / std::max(1., double(to.width)),
        from.height / std::max(1., double(to.height)));
    return {
        float(std::ceil(to.width * scale)),
        float(std::ceil(to.height * scale))
    };
}

MTLFrameSize MTLAspectFilled(MTLFrameSize from, MTLFrameSize to) {
    //        let scale = max(size.width / max(1.0, self.width), size.height / max(1.0, self.height))

    double scale = std::max(
        to.width / std::max(1., double(from.width)),
        to.height / std::max(1., double(from.height)));
    return {
        float(std::ceil(from.width * scale)),
        float(std::ceil(from.height * scale))
    };
}


static NSString *const pipelineDescriptorLabel = @"RTCPipeline";
static NSString *const commandBufferLabel = @"RTCCommandBuffer";
static NSString *const renderEncoderLabel = @"RTCEncoder";
static NSString *const renderEncoderDebugGroup = @"RTCDrawFrame";


static TGRTCMetalContextHolder *metalContext = nil;

bool initMetal() {
    if (metalContext == nil) {
        metalContext = [[TGRTCMetalContextHolder alloc] init];
    } else if(metalContext.displayId != CGMainDisplayID()) {
        metalContext = [[TGRTCMetalContextHolder alloc] init];
    }
    return metalContext != nil;
}

static inline void getCubeVertexData(size_t frameWidth,
                                     size_t frameHeight,
                                     RTCVideoRotation rotation,
                                     float *buffer) {
  // The computed values are the adjusted texture coordinates, in [0..1].
  // For the left and top, 0.0 means no cropping and e.g. 0.2 means we're skipping 20% of the
  // left/top edge.
  // For the right and bottom, 1.0 means no cropping and e.g. 0.8 means we're skipping 20% of the
  // right/bottom edge (i.e. render up to 80% of the width/height).
  float cropLeft = 0;
  float cropRight = 1;
  float cropTop = 0;
  float cropBottom = 1;

  float values[16] = {-1.0, -1.0, cropLeft, cropBottom,
                         1.0, -1.0, cropRight, cropBottom,
                        -1.0,  1.0, cropLeft, cropTop,
                         1.0,  1.0, cropRight, cropTop};
    memcpy(buffer, &values, sizeof(values));
}

@implementation TGRTCMTLRenderer {
    __kindof CAMetalLayer *_view;

    TGRTCMetalContextHolder* _context;
    
    id<MTLCommandQueue> _commandQueue;
    id<MTLBuffer> _vertexBuffer;

    MTLFrameSize _frameSize;
    MTLFrameSize _scaledSize;

    
    id<MTLTexture> _rgbTexture;
    id<MTLTexture> _rgbScaledAndBlurredTexture;
    
    id<MTLBuffer> _vertexBuffer0;
    id<MTLBuffer> _vertexBuffer1;
    id<MTLBuffer> _vertexBuffer2;
    
    dispatch_semaphore_t _inflight;

}

@synthesize rotationOverride = _rotationOverride;

- (instancetype)init {
  if (self = [super init]) {
      _inflight = dispatch_semaphore_create(0);
      
      float vertexBufferArray[16] = {0};
      _vertexBuffer = [metalContext.device newBufferWithBytes:vertexBufferArray
                                           length:sizeof(vertexBufferArray)
                                          options:MTLResourceCPUCacheModeWriteCombined];

  }

  return self;
}

- (BOOL)setRenderingDestination:(__kindof CAMetalLayer *)view {
  return [self setupWithView:view];
}

#pragma mark - Private

- (BOOL)setupWithView:(__kindof CAMetalLayer *)view {
    BOOL success = NO;
    if ([self setupMetal]) {
        _view = view;
        view.device = metalContext.device;
        _context = metalContext;
        success = YES;
    }
    return success;
}
#pragma mark - Inheritance

- (id<MTLDevice>)currentMetalDevice {
  return metalContext.device;
}


- (void)uploadTexturesToRenderEncoder:(id<MTLRenderCommandEncoder>)renderEncoder {
  RTC_NOTREACHED() << "Virtual method not implemented in subclass.";
}

- (void)getWidth:(int *)width
          height:(int *)height
         ofFrame:(nonnull RTC_OBJC_TYPE(RTCVideoFrame) *)frame {
  RTC_NOTREACHED() << "Virtual method not implemented in subclass.";
}

- (BOOL)setupTexturesForFrame:(nonnull RTC_OBJC_TYPE(RTCVideoFrame) *)frame {
  RTCVideoRotation rotation;
  NSValue *rotationOverride = self.rotationOverride;
  if (rotationOverride) {
      [rotationOverride getValue:&rotation];
  } else {
      rotation = frame.rotation;
  }

    
  int frameWidth, frameHeight;
  [self getWidth:&frameWidth
          height:&frameHeight
         ofFrame:frame];

  if (frameWidth != _frameSize.width || frameHeight != _frameSize.height) {
    getCubeVertexData(frameWidth,
                      frameHeight,
                      rotation,
                      (float *)_vertexBuffer.contents);
      
      _frameSize.width = frameWidth;
      _frameSize.height = frameHeight;
      
      MTLFrameSize small;
      small.width = _frameSize.width / 2;
      small.height = _frameSize.height / 2;

      _scaledSize = MTLAspectFitted(small, _frameSize);
      _rgbTexture = [self createTextureWithUsage: MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget size:_frameSize];
      
      _rgbScaledAndBlurredTexture = [self createTextureWithUsage:MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget size:_scaledSize];
      
      float verts[8] = {-1.0, 1.0, 1.0, 1.0, -1.0, -1.0, 1.0, -1.0}; //Metal uses a top-left origin, rotate 0
      
      _vertexBuffer0 = [_context.device newBufferWithBytes:verts length:sizeof(verts) options:0];
      
      float values[8] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0}; //rotate 0 quad
      
      _vertexBuffer1 = [_context.device newBufferWithBytes:values
                                                        length:sizeof(values)
                                                       options:0];
      
      _vertexBuffer2 = [_context.device newBufferWithBytes:values
                                                         length:sizeof(values)
                                                        options:0];
  }

  return YES;
}

#pragma mark - GPU methods

- (BOOL)setupMetal {
    _context = metalContext;
  // Set the view to use the default device.
  if (!_context.device) {
    return NO;
  }
  _commandQueue = [_context.device newCommandQueueWithMaxCommandBufferCount:3];

  return YES;
}

- (id<MTLTexture>)createTextureWithUsage:(MTLTextureUsage) usage size:(MTLFrameSize)size {
    MTLTextureDescriptor *rgbTextureDescriptor = [MTLTextureDescriptor
                                                  texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                  width:size.width
                                                  height:size.height
                                                  mipmapped:YES];
    rgbTextureDescriptor.usage = usage;
    return [metalContext.device newTextureWithDescriptor:rgbTextureDescriptor];
}

- (id<MTLRenderCommandEncoder>)createRenderEncoderForTarget: (id<MTLTexture>)texture with: (id<MTLCommandBuffer>)commandBuffer {
    MTLRenderPassDescriptor *renderPassDescriptor = [[MTLRenderPassDescriptor alloc] init];
    renderPassDescriptor.colorAttachments[0].texture = texture;
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    
    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = renderEncoderLabel;
    
    return renderEncoder;
}


- (id<MTLTexture>)convertYUVtoRGV {
    id<MTLTexture> rgbTexture = _rgbTexture;

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    
    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: rgbTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_context.pipelineYuvRgb];
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [self uploadTexturesToRenderEncoder:renderEncoder];
    [renderEncoder setFragmentSamplerState:_context.sampler atIndex:0];
    
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                    instanceCount:1];
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer commit];
//    [commandBuffer waitUntilCompleted];
    
    return rgbTexture;
}

- (id<MTLTexture>)scaleAndBlur:(id<MTLTexture>)inputTexture scale:(simd_float2)scale {

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    
    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: _rgbScaledAndBlurredTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_context.pipelineScaleAndBlur];

    [renderEncoder setFragmentTexture:inputTexture atIndex:0];
    
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];

    [renderEncoder setFragmentBytes:&scale length:sizeof(scale) atIndex:0];
    [renderEncoder setFragmentSamplerState:_context.sampler atIndex:0];

    bool vertical = true;
    [renderEncoder setFragmentBytes:&vertical length:sizeof(vertical) atIndex:1];

    
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                    instanceCount:1];
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer commit];
//    [commandBuffer waitUntilCompleted];
    
    return _rgbScaledAndBlurredTexture;
}

- (void)mergeYUVTexturesInTarget:(id<MTLTexture>)targetTexture foregroundTexture: (id<MTLTexture>)foregroundTexture backgroundTexture:(id<MTLTexture>)backgroundTexture scale1:(simd_float2)scale1 scale2:(simd_float2)scale2 {
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: targetTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_context.pipelineTransformAndBlend];
    
    
    [renderEncoder setVertexBuffer:_vertexBuffer0 offset:0 atIndex:0];
    [renderEncoder setVertexBuffer:_vertexBuffer1 offset:0 atIndex:1];
    [renderEncoder setVertexBuffer:_vertexBuffer2 offset:0 atIndex:2];
    
    [renderEncoder setFragmentTexture:foregroundTexture atIndex:0];
    [renderEncoder setFragmentTexture:backgroundTexture atIndex:1];

    [renderEncoder setFragmentBytes:&scale1 length:sizeof(scale1) atIndex:0];
    [renderEncoder setFragmentBytes:&scale2 length:sizeof(scale2) atIndex:1];
    
    [renderEncoder setFragmentSamplerState:_context.sampler atIndex:0];
    [renderEncoder setFragmentSamplerState:_context.sampler atIndex:1];
    
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                    instanceCount:1];
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer commit];
//    [commandBuffer waitUntilCompleted];
}

- (void)render {
    id<CAMetalDrawable> drawable = _view.nextDrawable;
    
    CGSize drawableSize = _view.drawableSize;

    MTLFrameSize from;
    MTLFrameSize to;
    
    from.width = _view.bounds.size.width;
    from.height = _view.bounds.size.height;

    to.width = drawableSize.width;
    to.height = drawableSize.height;
    
    MTLFrameSize viewSize = MTLAspectFilled(to, from);
    
    MTLFrameSize fitted = MTLAspectFitted(from, to);

    
    CGSize viewPortSize = CGSizeMake(viewSize.width, viewSize.height);
    
    
    id<MTLTexture> targetTexture = drawable.texture;
    
    float ratio = (float)_frameSize.height / (float)_frameSize.width;
    CGFloat heightAspectScale = viewPortSize.height / (fitted.width * ratio);
    CGFloat widthAspectScale = viewPortSize.width / (fitted.height * (1.0/ratio));

    _rgbTexture = [self convertYUVtoRGV];

    simd_float2 smallScale = simd_make_float2(_frameSize.width / _scaledSize.width, _frameSize.height / _scaledSize.height);
    
    _rgbScaledAndBlurredTexture = [self scaleAndBlur:_rgbTexture scale:smallScale];
    
    simd_float2 scale1 = simd_make_float2(MAX(1.0, widthAspectScale), MAX(1.0, heightAspectScale));
    
    float bgRatio_w = _scaledSize.width / _frameSize.width;
    float bgRatio_h = _scaledSize.height / _frameSize.height;

    simd_float2 scale2 = simd_make_float2(MIN(bgRatio_w, widthAspectScale * bgRatio_w), MIN(bgRatio_h, heightAspectScale * bgRatio_h));

    [self mergeYUVTexturesInTarget: targetTexture
                    foregroundTexture: _rgbTexture
                    backgroundTexture: _rgbScaledAndBlurredTexture
                    scale1:scale1
                    scale2:scale2];
    
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    [commandBuffer presentDrawable:drawable];
    
    
    dispatch_semaphore_t inflight = _inflight;

    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
        dispatch_semaphore_signal(inflight);
    }];

    dispatch_async(dispatch_get_main_queue(), ^{
        [commandBuffer commit];
    });
    dispatch_semaphore_wait(_inflight, DISPATCH_TIME_FOREVER);

//    [commandBuffer commit];
    
}

-(void)dealloc {
    dispatch_semaphore_signal(_inflight);
}

#pragma mark - RTCMTLRenderer

- (void)drawFrame:(RTC_OBJC_TYPE(RTCVideoFrame) *)frame {
  @autoreleasepool {
      if ([self setupTexturesForFrame:frame]) {
          [self render];
      }
  }
}

@end


//
//- (id<MTLTexture>)blurInputTexture: (id<MTLTexture>)inputTexture {
//    id<MTLTexture> blurTexture = [self createTextureWithUsage: MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget];
//
//    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
//
//    if (@available(macOS 10.13, *)) {
//        MPSImageGaussianBlur *blur = [[MPSImageGaussianBlur alloc] initWithDevice:metalContext.device sigma: 60.0];
//
//        [blur encodeToCommandBuffer:commandBuffer sourceTexture:inputTexture destinationTexture:blurTexture];
//        [commandBuffer commit];
//        [commandBuffer waitUntilCompleted];
//    } else {
//        // Fallback on earlier versions
//    }
//
//    return blurTexture;
//}
