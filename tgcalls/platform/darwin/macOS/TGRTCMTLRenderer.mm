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

#include "api/video/video_rotation.h"
#include "rtc_base/checks.h"

static NSString *const vertexFunctionName = @"vertexPassthrough";
static NSString *const fragmentFunctionName = @"fragmentColorConversion";
static NSString *const fragmentDoTransformFilter = @"doTransformFilter";
static NSString *const twoInputVertexName = @"twoInputVertex";
static NSString *const normalBlendFragmentName = @"normalBlendFragment";
static NSString *const gaussianBlurFragmentName = @"gaussianBlurFragment";


static NSString *const pipelineDescriptorLabel = @"RTCPipeline";
static NSString *const commandBufferLabel = @"RTCCommandBuffer";
static NSString *const renderEncoderLabel = @"RTCEncoder";
static NSString *const renderEncoderDebugGroup = @"RTCDrawFrame";

static CGFloat const kBlurTextureSizeFactor = 4.;


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

// The max number of command buffers in flight (submitted to GPU).
// For now setting it up to 1.
// In future we might use triple buffering method if it improves performance.
static const NSInteger kMaxInflightBuffers = 1;

@implementation TGRTCMTLRenderer {
  __kindof CAMetalLayer *_view;

  // Controller.

  // Renderer.
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLLibrary> _defaultLibrary;
  id<MTLRenderPipelineState> _pipelineStateYUVtoRGB;
  id<MTLRenderPipelineState> _pipelineStateTransform;
  id<MTLRenderPipelineState> _pipelineStateNormalBlend;

  // Buffers.
  id<MTLBuffer> _vertexBuffer;

  // Values affecting the vertex buffer. Stored for comparison to avoid unnecessary recreation.
  int _frameWidth;
  int _frameHeight;
  int _oldCropWidth;
  int _oldCropHeight;
  int _oldCropX;
  int _oldCropY;
  RTCVideoRotation _oldRotation;
    

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
    view.device = _device;
      
    [self loadAssets];

    float vertexBufferArray[16] = {0};
    _vertexBuffer = [_device newBufferWithBytes:vertexBufferArray
                                         length:sizeof(vertexBufferArray)
                                        options:MTLResourceCPUCacheModeWriteCombined];
    success = YES;
  }
  return success;
}
#pragma mark - Inheritance

- (id<MTLDevice>)currentMetalDevice {
  return _device;
}


- (void)uploadTexturesToRenderEncoder:(id<MTLRenderCommandEncoder>)renderEncoder {
  RTC_NOTREACHED() << "Virtual method not implemented in subclass.";
}
-(NSArray<id<MTLTexture>> *)textures {
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

  if (frameWidth != _frameWidth ||
      frameHeight != _frameHeight) {
    getCubeVertexData(frameWidth,
                      frameHeight,
                      rotation,
                      (float *)_vertexBuffer.contents);
    _frameWidth = frameWidth;
    _frameHeight = frameHeight;
  }

  return YES;
}

#pragma mark - GPU methods

- (BOOL)setupMetal {
  // Set the view to use the default device.
  _device = CGDirectDisplayCopyCurrentMetalDevice(CGMainDisplayID());
  if (!_device) {
    return NO;
  }

  // Create a new command queue.
  _commandQueue = [_device newCommandQueue];
  _defaultLibrary = [_device newDefaultLibrary];

  return YES;
}

- (void)loadAssets {
 
    {
        id<MTLFunction> vertexFunction = [_defaultLibrary newFunctionWithName:vertexFunctionName];
        id<MTLFunction> fragmentFunction = [_defaultLibrary newFunctionWithName:fragmentFunctionName];

        MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.label = pipelineDescriptorLabel;
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        pipelineDescriptor.colorAttachments[0].pixelFormat = _view.pixelFormat;
        pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
        NSError *error = nil;
        _pipelineStateYUVtoRGB = [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    }

    {
        id<MTLFunction> vertexFunction = [_defaultLibrary newFunctionWithName:vertexFunctionName];
        id<MTLFunction> fragmentFunction = [_defaultLibrary newFunctionWithName:fragmentDoTransformFilter];
        
        MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.label = @"transformInputLabel";
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        pipelineDescriptor.colorAttachments[0].pixelFormat = _view.pixelFormat;
        pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
        _pipelineStateTransform = [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:nil];
    }
    
    {
        id<MTLFunction> vertexFunction = [_defaultLibrary newFunctionWithName:twoInputVertexName];
        id<MTLFunction> fragmentFunction = [_defaultLibrary newFunctionWithName:normalBlendFragmentName];
        
        MTLRenderPipelineDescriptor *pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.label = @"twoInputLabel";
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        pipelineDescriptor.colorAttachments[0].pixelFormat = _view.pixelFormat;
        pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
        _pipelineStateNormalBlend = [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:nil];
    }
}

- (id<MTLTexture>)createTextureWithUsage:(MTLTextureUsage) usage {
    MTLTextureDescriptor *rgbTextureDescriptor = [MTLTextureDescriptor
                                                  texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                  width:_frameWidth
                                                  height:_frameHeight
                                                  mipmapped:YES];
    rgbTextureDescriptor.usage = usage;
    return [_device newTextureWithDescriptor:rgbTextureDescriptor];
}

- (id<MTLRenderCommandEncoder>)createRenderEncoderForTarget: (id<MTLTexture>)texture with: (id<MTLCommandBuffer>)commandBuffer {
    MTLRenderPassDescriptor *renderPassDescriptor = [[MTLRenderPassDescriptor alloc] init];
    renderPassDescriptor.colorAttachments[0].texture = texture;
    renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    
    id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = renderEncoderLabel;
    
    return renderEncoder;
}

- (id<MTLSamplerState>)defaultSamplerState {
    MTLSamplerDescriptor *samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToZero;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToZero;

    return [_device newSamplerStateWithDescriptor:samplerDescriptor];
}

- (id<MTLTexture>)convertYUVtoRGV {
    id<MTLTexture> rgbTexture = [self createTextureWithUsage: MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget];

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget: rgbTexture with: commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_pipelineStateYUVtoRGB];
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [self uploadTexturesToRenderEncoder:renderEncoder];
    [renderEncoder setFragmentSamplerState:self.defaultSamplerState atIndex:0];
    
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

- (id<MTLTexture>)blurInputTexture: (id<MTLTexture>)inputTexture {
    id<MTLTexture> blurTexture = [self createTextureWithUsage: MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget];
    
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    
    if (@available(macOS 10.13, *)) {
        MPSImageGaussianBlur *blur = [[MPSImageGaussianBlur alloc] initWithDevice:_device sigma: 60.0];
        
        [blur encodeToCommandBuffer:commandBuffer sourceTexture:inputTexture destinationTexture:blurTexture];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    } else {
        // Fallback on earlier versions
    }
    
    return blurTexture;
}

- (id<MTLTexture>)transformTextureWithInputTexture: (id<MTLTexture>)inputTexture scaleX: (CGFloat) inputScaleX scaleY: (CGFloat) inputScaleY {
    id<MTLTexture> texture = [self createTextureWithUsage: MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];;
    
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget:texture with:commandBuffer];
    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_pipelineStateTransform];
    [renderEncoder setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
    [renderEncoder setFragmentTexture:inputTexture atIndex:0];

    simd_float2 scale = simd_make_float2(inputScaleX, inputScaleY);
    [renderEncoder setFragmentBytes:&scale length:sizeof(scale) atIndex:0];

    simd_float2 translate = simd_make_float2(0, 0);
    [renderEncoder setFragmentBytes:&translate length:sizeof(translate) atIndex:1];

    [renderEncoder setFragmentSamplerState:self.defaultSamplerState atIndex:0];

    [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                    instanceCount:1];
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    
    return texture;
}

- (void)mergeTexturesWithInputTextures: (NSArray<id<MTLTexture>>*)inputTextures target:(id<MTLTexture>)target {
    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];

    id<MTLRenderCommandEncoder> renderEncoder = [self createRenderEncoderForTarget:target with:commandBuffer];

    [renderEncoder pushDebugGroup:renderEncoderDebugGroup];
    [renderEncoder setRenderPipelineState:_pipelineStateNormalBlend];

    float verts[8] = {-1.0, 1.0, 1.0, 1.0, -1.0, -1.0, 1.0, -1.0}; //Metal uses a top-left origin, rotate 0

    id<MTLBuffer> vertexBuffer = [_device newBufferWithBytes:verts length:sizeof(verts) options:0];
    [renderEncoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    
    for (int i = 0; i < inputTextures.count; i++) {
        id<MTLTexture> currentTexture = inputTextures[i];

        float values[8] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0}; //rotate 0 quad

        id<MTLBuffer> vertexBuffer = [_device newBufferWithBytes:values
                                                          length:sizeof(values)
                                                         options:0];
        vertexBuffer.label = @"Texture Coordinates";

        [renderEncoder setVertexBuffer:vertexBuffer offset:0 atIndex:1 + i];
        [renderEncoder setFragmentTexture:currentTexture atIndex:i];
     }

    [renderEncoder setFragmentSamplerState:self.defaultSamplerState atIndex:0];

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
    
    CGSize viewPortSize = NSZeroSize;
    viewPortSize = _view.bounds.size;

    id<MTLTexture> rgbTexture = [self convertYUVtoRGV];
    
    float ratio = (float)_frameHeight / (float)_frameWidth;
    CGFloat heightAspectScale = viewPortSize.height / (viewPortSize.width * ratio);
    CGFloat widthAspectScale = viewPortSize.width / (viewPortSize.height * (1.0/ratio));
    
    id<MTLTexture> frontTexture = [self transformTextureWithInputTexture:rgbTexture scaleX:MAX(1.0, widthAspectScale) scaleY:MAX(1.0, heightAspectScale)];
    
    id<MTLTexture> backgroundTexture = [self transformTextureWithInputTexture:rgbTexture scaleX:MIN(1.0, widthAspectScale) scaleY:MIN(1.0, heightAspectScale)];

    id<MTLTexture> blurredBackgroundTexture = [self blurInputTexture: backgroundTexture];
    
    [self mergeTexturesWithInputTextures:@[frontTexture, blurredBackgroundTexture] target:drawable.texture];
    
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
    // Wait until the inflight (curently sent to GPU) command buffer
    // has completed the GPU work.

    if ([self setupTexturesForFrame:frame]) {
      [self render];
    } else {
    }
  }
}

@end
