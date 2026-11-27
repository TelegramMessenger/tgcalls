#pragma once

#include "api/video/i420_buffer.h"
#include "api/video/adapted_video_track_source.h"
#include "rtc_base/ref_counted_object.h"

#include <atomic>
#include <thread>

// Generates I420 frames at `fps` (30 by default; a screen-sharing participant uses 60, the in-app share's rate) with
// per-participant color tint and an incrementing frame counter rendered as block digits.
class FakeVideoTrackSource : public webrtc::AdaptedVideoTrackSource {
public:
    static webrtc::scoped_refptr<FakeVideoTrackSource> Create(int participantId, int fps = 30);

    ~FakeVideoTrackSource() override;

    void Stop();

    // VideoTrackSourceInterface
    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return false; }
    std::optional<bool> needs_denoising() const override { return false; }

protected:
    FakeVideoTrackSource(int participantId, int fps);

private:
    void GenerateThread();
    void RenderDigits(uint8_t* yPlane, int strideY, int frameNumber);

    int participantId_;
    int fps_;
    uint8_t uTint_;
    uint8_t vTint_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};
