#ifndef TGCALLS_DARWIN_FFMPEG_H
#define TGCALLS_DARWIN_FFMPEG_H

#include "platform/PlatformInterface.h"

namespace tgcalls {

void setupDarwinVideoDecoding(AVCodecContext *codecContext);
rtc::scoped_refptr<webrtc::VideoFrameBuffer> createDarwinPlatformFrameFromData(AVFrame const *frame);

} // namespace tgcalls

#endif
