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

    int _frameWidth;
    int _frameHeight;
    
    id<MTLTexture> _rgbTexture;
    id<MTLTexture> _rgbScaledAndBlurredTexture;

}

@synthesize rotationOverride = _rotationOverride;

- (instancetype)init {
  if (self = [super init]) {
  }

  return self;
}

- (BOOL)addRenderingDestination:(__kindof CAMetalLayer *)view {
  return [self setupWithView:view];
}

#pragma mark - Private

- (BOOL)setupWithView:(__kindof CAMetalLayer *)view {
    BOOL success = NO;
    if ([self setupMetal]) {
        _view = view;
          
        view.device = metalContext.device;

        
        _context = metalContext;

        float vertexBufferArray[16] = {0};
        _vertexBuffer = [metalContext.device newBufferWithBytes:vertexBufferArray
                                             length:sizeof(vertexBufferArray)
                                            options:MTLResourceCPUCacheModeWriteCombined];
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

  if (frameWidth != _frameWidth || frameHeight != _frameHeight) {
    getCubeVertexData(frameWidth,
                      frameHeight,
                      rotation,
                      (float *)_vertexBuffer.contents);
      
      _frameWidth = frameWidth;
      _frameHeight = frameHeight;
      
      
      _rgbTexture = [self createTextureWithUsage: MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget size:CGSizeMake(_frameWidth, _frameHeight)];
      
      _rgbScaledAndBlurredTexture = [self createTextureWithUsage:MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget size:CGSizeMake(_frameWidth, _frameHeight)];
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
  _commandQueue = [_context.device newCommandQueue];

  return YES;
}

- (id<MTLTexture>)createTextureWithUsage:(MTLTextureUsage) usage size:(CGSize)size {
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
    [commandBuffer waitUntilCompleted];
    
    return rgbTexture;
}

- (id<MTLTexture>)scaleAndBlur:(id<MTLTexture>)inputTexture scale:(simd_float2)scale {
    id<MTLTexture> rgbTexture = _rgbTexture;

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    
    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: _rgbScaledAndBlurredTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_context.pipelineScaleAndBlur];

    [renderEncoder setFragmentTexture:inputTexture atIndex:0];
    
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];

    [renderEncoder setFragmentBytes:&scale length:sizeof(scale) atIndex:0];
    [renderEncoder setFragmentSamplerState:_context.sampler atIndex:0];

    
    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                    instanceCount:1];
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    
    return _rgbScaledAndBlurredTexture;
}

- (void)mergeYUVTexturesInTarget:(id<MTLTexture>)targetTexture foregroundTexture: (id<MTLTexture>)foregroundTexture backgroundTexture:(id<MTLTexture>)backgroundTexture scale1:(simd_float2)scale1 scale2:(simd_float2)scale2 {
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: targetTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_context.pipelineTransformAndBlend];
    
    float verts[8] = {-1.0, 1.0, 1.0, 1.0, -1.0, -1.0, 1.0, -1.0}; //Metal uses a top-left origin, rotate 0
    
    id<MTLBuffer> vertexBuffer = [_context.device newBufferWithBytes:verts length:sizeof(verts) options:0];
    [renderEncoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    
    float values[8] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0}; //rotate 0 quad
    
    id<MTLBuffer> vertexBuffer1 = [_context.device newBufferWithBytes:values
                                                      length:sizeof(values)
                                                     options:0];
    vertexBuffer1.label = @"Texture Coordinates 1";
    
    id<MTLBuffer> vertexBuffer2 = [_context.device newBufferWithBytes:values
                                                       length:sizeof(values)
                                                      options:0];
    vertexBuffer2.label = @"Texture Coordinates 2";
    
    [renderEncoder setVertexBuffer:vertexBuffer1 offset:0 atIndex:1];
    [renderEncoder setVertexBuffer:vertexBuffer2 offset:0 atIndex:2];
    
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
    [commandBuffer waitUntilCompleted];
}

- (void)render {
    id<CAMetalDrawable> drawable = _view.nextDrawable;
    
    CGSize viewPortSize = _view.bounds.size;

    id<MTLTexture> targetTexture = drawable.texture;
    
    float ratio = (float)_frameHeight / (float)_frameWidth;
    CGFloat heightAspectScale = viewPortSize.height / (viewPortSize.width * ratio);
    CGFloat widthAspectScale = viewPortSize.width / (viewPortSize.height * (1.0/ratio));
    
    _rgbTexture = [self convertYUVtoRGV];

    _rgbScaledAndBlurredTexture = [self scaleAndBlur:_rgbTexture scale:simd_make_float2(MIN(1.0, widthAspectScale), MIN(1.0, heightAspectScale))];
    
    [self mergeYUVTexturesInTarget: targetTexture
                    foregroundTexture: _rgbTexture
                    backgroundTexture: _rgbScaledAndBlurredTexture
                    scale1:simd_make_float2(MAX(1.0, widthAspectScale), MAX(1.0, heightAspectScale))
                    scale2:simd_make_float2(MIN(1.0, widthAspectScale), MIN(1.0, heightAspectScale))];
    
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
}

-(void)dealloc {
    
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
