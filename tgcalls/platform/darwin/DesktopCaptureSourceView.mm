//
//  DesktopCaptureSourceView.m
//  TgVoipWebrtc
//
//  Created by Mikhail Filimonov on 28.12.2020.
//  Copyright © 2020 Mikhail Filimonov. All rights reserved.
//

#import "DesktopCaptureSourceView.h"
#import "platform/darwin/VideoMetalViewMac.h"

@interface DesktopCaptureSourceView ()
@property (nonatomic, strong) ;
@end

@implementation DesktopCaptureSourceView
{
    DesktopCaptureSourceHelper _helper;
}

-(id)initWithHelper:(DesktopCaptureSourceHelper)helper {
    if (self = [super initWithFrame:CGRectZero]) {
        _helper = helper;
        std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink = [self getSink];
        helper.setOutput(sink);
        [self setVideoContentMode:kCAGravityResizeAspectFill];
    }
    return self;
}

@end

@implementation DesktopCaptureSourceScope

-(id)initWithSource:(DesktopCaptureSource)source data:(DesktopCaptureSourceData)data {
    if (self = [super init]) {
        _data = data;
        _source = source;
    }
    return self;
}

-(NSString *)cachedKey {
    return [[NSString alloc] initWithFormat:@"%@:%@", [NSString stringWithUTF8String:self.source.uniqueKey().c_str()], [NSString stringWithUTF8String:self.data.cachedKey().c_str()]];
}

@end

@implementation DesktopCaptureSourceViewManager
{
    std::map<NSString*, DesktopCaptureSourceHelper> _cached;
}

-(NSView *)createForScope:(DesktopCaptureSourceScope*)scope {
    auto i = _cached.find(scope.cachedKey);
    if (i == end(_cached)) {
        i = _cached.emplace(
            scope.cachedKey,
            DesktopCaptureSourceHelper(scope.source, scope.data)).first;
    }
    return [[DesktopCaptureSourceView alloc] initWithHelper:i->second];
}

-(void)start:(DesktopCaptureSourceScope *)scope {
    const auto i = _cached.find(scope.cachedKey)
    if (i != end(_cached)) {
        i->second.start();
    }
}

-(void)stop:(DesktopCaptureSourceScope *)scope {
    const auto i = _cached.find(scope.cachedKey)
    if (i != end(_cached)) {
        i->second.stop();
    }
}

-(void)dealloc {
    for (auto &[key, helper] : _cached) {
        helper.stop();
    }
}

@end
