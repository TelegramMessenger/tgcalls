#ifndef TGCALLS_VIDEO_STREAMING_PART_H
#define TGCALLS_VIDEO_STREAMING_PART_H

#include "absl/types/optional.h"
#include <vector>
#include <stdint.h>

#include "api/video/video_frame.h"

namespace tgcalls {

class VideoStreamingPartState;

class VideoStreamingPart {
public:
    explicit VideoStreamingPart(std::vector<uint8_t> &&data);
    ~VideoStreamingPart();
    
    VideoStreamingPart(const VideoStreamingPart&) = delete;
    VideoStreamingPart(VideoStreamingPart&& other) {
        _state = other._state;
        other._state = nullptr;
    }
    VideoStreamingPart& operator=(const VideoStreamingPart&) = delete;
    VideoStreamingPart& operator=(VideoStreamingPart&&) = delete;

    std::vector<webrtc::VideoFrame> getNextFrame();
    
private:
    VideoStreamingPartState *_state = nullptr;
};

}

#endif
