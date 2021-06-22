//
//  Capturer.m
//  CoreMediaMacCapture
//
//  Created by Mikhail Filimonov on 21.06.2021.
//

#import "TGCMIOCapturer.h"
#import "TGCMIODevice.h"

@interface TGCMIOCapturer ()

@end

@implementation TGCMIOCapturer
{
    NSString * _deviceId;
    TGCMIODevice * _device;
}
-(id)initWithDeviceId:(NSString *)deviceId {
    if (self = [super init]) {
        _deviceId = deviceId;
        
    }
    return self;
}

-(void)start:(renderBlock)renderBlock {
    _device = [TGCMIODevice FindDeviceByUniqueId:_deviceId];
   
    [_device run:^(CMSampleBufferRef sampleBuffer) {
        renderBlock(sampleBuffer);
    }];
    
    
}
-(void)stop {
    [_device stop];
}

-(void)dealloc {
    
}

@end
