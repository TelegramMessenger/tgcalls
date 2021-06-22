//
//  CoreMediaVideoHAL.h
//  CoreMediaMacCapture
//
//  Created by Mikhail Filimonov on 21.06.2021.
//

#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardware.h>

NS_ASSUME_NONNULL_BEGIN

typedef void(^RenderBlock)(CMSampleBufferRef);

@interface TGCMIODevice : NSObject

+(TGCMIODevice * __nullable)FindDeviceByUniqueId:(NSString *)pUID;

-(void)run:(RenderBlock)render;
-(void)stop;

-(CMIODeviceID)cmioDevice;
@end




NS_ASSUME_NONNULL_END
