#include "StreamingMediaContext.h"

#include "AudioStreamingPart.h"
#include "VideoStreamingPart.h"

#include "absl/types/optional.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "absl/types/variant.h"
#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"
#include "common_audio/ring_buffer.h"

namespace tgcalls {

namespace {

struct VideoSegment {
    std::shared_ptr<VideoStreamingPart> part;
    double lastFramePts = -1.0;
};

struct MediaSegment {
    int64_t timestamp = 0;
    int64_t duration = 0;
    std::shared_ptr<AudioStreamingPart> audio;
    std::vector<std::shared_ptr<VideoSegment>> video;
};

struct PendingAudioSegmentData {
};

struct PendingVideoSegmentData {
    int32_t channelId = 0;
    VideoChannelDescription::Quality quality = VideoChannelDescription::Quality::Thumbnail;

    PendingVideoSegmentData(int32_t channelId_, VideoChannelDescription::Quality quality_) :
    channelId(channelId_),
    quality(quality_) {
    }
};

struct PendingMediaSegmentPartResult {
    std::vector<uint8_t> data;

    explicit PendingMediaSegmentPartResult(std::vector<uint8_t> &&data_) :
    data(std::move(data_)) {
    }
};

struct PendingMediaSegmentPart {
    absl::variant<PendingAudioSegmentData, PendingVideoSegmentData> typeData;

    int64_t minRequestTimestamp = 0;

    std::shared_ptr<BroadcastPartTask> task;
    std::shared_ptr<PendingMediaSegmentPartResult> result;
};

struct PendingMediaSegment {
    int64_t timestamp = 0;
    std::vector<std::shared_ptr<PendingMediaSegmentPart>> parts;
};

#if 0

class SampleRingBuffer {
public:
    SampleRingBuffer(size_t size) {
        _buffer = WebRtc_CreateBuffer(size, sizeof(int16_t));
    }

    ~SampleRingBuffer() {
        if (_buffer) {
            WebRtc_FreeBuffer(_buffer);
        }
    }

    size_t availableForWriting() {
        return WebRtc_available_write(_buffer);
    }

    size_t write(int16_t const *samples, size_t count) {
        return WebRtc_WriteBuffer(_buffer, samples, count);
    }

    size_t read(int16_t *samples, size_t count) {
        void *data_ptr = nullptr;
        return WebRtc_ReadBuffer(_buffer, &data_ptr, samples, count);
    }

private:
    RingBuffer *_buffer = nullptr;
};

#else

class SampleRingBuffer {
public:
    SampleRingBuffer(size_t size) {
        _buffer.resize(size);
    }

    ~SampleRingBuffer() {
    }

    size_t availableForWriting() {
        return _buffer.size() - _currentSize;
    }

    size_t write(int16_t const *samples, size_t count) {
        size_t actualSamples = std::min(availableForWriting(), count);
        if (actualSamples != 0) {
            memcpy(((uint8_t *)_buffer.data()) + _currentSize * 2, samples, actualSamples * 2);
            _currentSize += actualSamples;
        }
        return actualSamples;
    }

    size_t read(int16_t *samples, size_t count) {
        size_t actualSamples = std::min(_currentSize, count);
        memcpy(samples, _buffer.data(), actualSamples * 2);
        size_t bufferSize = _buffer.size();
        _buffer.erase(_buffer.begin(), _buffer.begin() + actualSamples);
        _buffer.resize(bufferSize);
        _currentSize -= actualSamples;
        return actualSamples;
    }

private:
    std::vector<int16_t> _buffer;
    size_t _currentSize = 0;
};

#endif

}

class StreamingMediaContextPrivate : public std::enable_shared_from_this<StreamingMediaContextPrivate> {
public:
    StreamingMediaContextPrivate(StreamingMediaContext::StreamingMediaContextArguments &&arguments) :
    _threads(arguments.threads),
    _requestAudioBroadcastPart(arguments.requestAudioBroadcastPart),
    _requestVideoBroadcastPart(arguments.requestVideoBroadcastPart),
    _displayVideoFrame(arguments.displayVideoFrame),
    _audioRingBuffer(_audioDataRingBufferMaxSize) {
    }

    ~StreamingMediaContextPrivate() {
    }

    void start() {
        beginRenderTimer(0);
    }

    void beginRenderTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<StreamingMediaContextPrivate>(shared_from_this());
        _threads->getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }

            strong->render();

            strong->beginRenderTimer(16);
        }, timeoutMs);
    }

    void render() {
        int64_t absoluteTimestamp = rtc::TimeMillis();

        while (getAvailableBufferDuration() > 1000) {
            if (_availableSegments.empty()) {
                _playbackReferenceTimestamp = 0;
                break;
            }

            if (_playbackReferenceTimestamp == 0) {
                _playbackReferenceTimestamp = absoluteTimestamp;
            }

            double relativeTimestamp = ((double)(absoluteTimestamp - _playbackReferenceTimestamp)) / 1000.0;

            auto segment = _availableSegments[0];
            double segmentDuration = ((double)segment->duration) / 1000.0;

            for (auto &videoSegment : segment->video) {
                auto frame = videoSegment->part->getFrameAtRelativeTimestamp(relativeTimestamp);
                if (frame) {
                    if (videoSegment->lastFramePts != frame->pts) {
                        videoSegment->lastFramePts = frame->pts;
                        _displayVideoFrame(frame->frame);
                    }
                }
            }

            _audioDataMutex.Lock();
            if (segment->audio) {
                while (_audioRingBuffer.availableForWriting() >= 480) {
                    auto audioChannels = segment->audio->get10msPerChannel();
                    if (audioChannels.empty()) {
                        break;
                    }

                    if (_audioMixBuffer.size() < 480) {
                        _audioMixBuffer.resize(480);
                    }
                    memset(_audioMixBuffer.data(), 0, _audioMixBuffer.size() * sizeof(int16_t));

                    for (const auto &audioChannel : audioChannels) {
                        memcpy(_audioMixBuffer.data(), audioChannel.pcmData.data(), audioChannel.pcmData.size() * sizeof(int16_t));

                        break;
                    }

                    /*const float wave_frequency_hz_ = 180.0;
                    const int16_t amplitude_ = INT16_MAX / 2;

                    int16_t* frame_data = _audioMixBuffer.data();
                    for (size_t i = 0; i < _audioMixBuffer.size(); ++i) {
                        for (size_t ch = 0; ch < 1; ++ch) {
                            frame_data[1 * i + ch] = (int16_t)(amplitude_ * sinf(phase_));
                        }
                        phase_ += wave_frequency_hz_ * 2 * kPi / 48000;
                    }*/

                    _audioRingBuffer.write(_audioMixBuffer.data(), _audioMixBuffer.size());
                }
            }
            _audioDataMutex.Unlock();

            if (relativeTimestamp >= segmentDuration) {
                _playbackReferenceTimestamp += segment->duration;

                if (segment->audio && segment->audio->getRemainingMilliseconds() > 0) {
                    RTC_LOG(LS_INFO) << "render: discarding " << segment->audio->getRemainingMilliseconds() << " ms of audio at the end of a segment";
                }

                _availableSegments.erase(_availableSegments.begin());
            }

            break;
        }

        requestSegmentIfNeeded();
    }

    void getAudio(int16_t *audio_samples, const size_t num_samples, const uint32_t samples_per_sec) {
        _audioDataMutex.Lock();

        size_t readSamples = _audioRingBuffer.read(audio_samples, num_samples);
        if (readSamples < num_samples) {
            memset(audio_samples + readSamples, 0, num_samples - readSamples);
        }

        _audioDataMutex.Unlock();
    }

    int64_t getAvailableBufferDuration() {
        int64_t result = 0;

        for (const auto &segment : _availableSegments) {
            result += segment->duration;
        }

        return result;
    }

    void discardPendingSegment() {
        if (!_pendingSegment) {
            return;
        }

        for (const auto &it : _pendingSegment->parts) {
            if (it->task) {
                it->task->cancel();
            }
        }
        _pendingSegment.reset();
    }

    void requestSegmentIfNeeded() {
        if (_pendingSegment) {
            return;
        }

        if (getAvailableBufferDuration() > 2000) {
            return;
        }

        _pendingSegment = std::make_shared<PendingMediaSegment>();
        _pendingSegment->timestamp = _nextSegmentTimestamp;

        //RTC_LOG(LS_INFO) << "initialize pending segment at " << _pendingSegment->timestamp;

        auto audio = std::make_shared<PendingMediaSegmentPart>();
        audio->typeData = PendingAudioSegmentData();
        audio->minRequestTimestamp = 0;
        _pendingSegment->parts.push_back(audio);

        for (size_t i = 0; i < _activeVideoChannels.size(); i++) {
            auto video = std::make_shared<PendingMediaSegmentPart>();
            auto quality = _activeVideoChannels[i].quality;
            if (quality == VideoChannelDescription::Quality::Medium) {
                quality = VideoChannelDescription::Quality::Thumbnail;
            }
            video->typeData = PendingVideoSegmentData((int32_t)(i + 1), quality);
            video->minRequestTimestamp = 0;
            _pendingSegment->parts.push_back(video);
        }

        checkPendingSegment();
    }

    void checkPendingSegment() {
        if (!_pendingSegment) {
            //RTC_LOG(LS_INFO) << "checkPendingSegment: _pendingSegment == nullptr";
            return;
        }

        const auto weak = std::weak_ptr<StreamingMediaContextPrivate>(shared_from_this());

        auto segmentTimestamp = _pendingSegment->timestamp;

        //RTC_LOG(LS_INFO) << "checkPendingSegment timestamp: " << segmentTimestamp;

        int64_t absoluteTimestamp = rtc::TimeMillis();
        int64_t minDelayedRequestTimeout = INT_MAX;

        bool allPartsDone = true;

        for (auto &part : _pendingSegment->parts) {
            if (!part->result) {
                allPartsDone = false;
            }

            if (!part->result && !part->task) {
                if (part->minRequestTimestamp < absoluteTimestamp) {
                    const auto weakPart = std::weak_ptr<PendingMediaSegmentPart>(part);

                    std::function<void(BroadcastPart &&)> handleResult = [weak, weakPart, threads = _threads, segmentTimestamp](BroadcastPart &&part) {
                        threads->getMediaThread()->PostTask(RTC_FROM_HERE, [weak, weakPart, part = std::move(part), segmentTimestamp]() mutable {
                            auto strong = weak.lock();
                            if (!strong) {
                                return;
                            }

                            if (!strong->_pendingSegment) {
                                return;
                            }
                            if (strong->_pendingSegment->timestamp != segmentTimestamp) {
                                return;
                            }
                            auto pendingPart = weakPart.lock();
                            if (!pendingPart) {
                                return;
                            }

                            pendingPart->task.reset();

                            switch (part.status) {
                                case BroadcastPart::Status::Success: {
                                    pendingPart->result = std::make_shared<PendingMediaSegmentPartResult>(std::move(part.data));
                                    strong->checkPendingSegment();
                                    break;
                                }
                                case BroadcastPart::Status::NotReady: {
                                    if (segmentTimestamp == 0) {
                                        int64_t responseTimestampMilliseconds = (int64_t)(part.responseTimestamp * 1000.0);
                                        int64_t responseTimestampBoundary = (responseTimestampMilliseconds / strong->_segmentDuration) * strong->_segmentDuration;

                                        strong->_nextSegmentTimestamp = responseTimestampBoundary;
                                        strong->discardPendingSegment();
                                        strong->requestSegmentIfNeeded();
                                    } else {
                                        pendingPart->minRequestTimestamp = rtc::TimeMillis() + 100;
                                        strong->checkPendingSegment();
                                    }
                                    break;
                                }
                                case BroadcastPart::Status::ResyncNeeded: {
                                    int64_t responseTimestampMilliseconds = (int64_t)(part.responseTimestamp * 1000.0);
                                    int64_t responseTimestampBoundary = (responseTimestampMilliseconds / strong->_segmentDuration) * strong->_segmentDuration;

                                    strong->_nextSegmentTimestamp = responseTimestampBoundary;
                                    strong->discardPendingSegment();
                                    strong->requestSegmentIfNeeded();

                                    break;
                                }
                                default: {
                                    RTC_FATAL() << "Unknown part.status";
                                    break;
                                }
                            }
                        });
                    };

                    const auto typeData = &part->typeData;
                    if (const auto audioData = absl::get_if<PendingAudioSegmentData>(typeData)) {
                        part->task = _requestAudioBroadcastPart(segmentTimestamp, _segmentDuration, handleResult);
                    } else if (const auto videoData = absl::get_if<PendingVideoSegmentData>(typeData)) {
                        part->task = _requestVideoBroadcastPart(segmentTimestamp, _segmentDuration, videoData->channelId, videoData->quality, handleResult);
                    }
                } else {
                    minDelayedRequestTimeout = std::min(minDelayedRequestTimeout, part->minRequestTimestamp - absoluteTimestamp);
                }
            }
        }

        if (minDelayedRequestTimeout < INT32_MAX) {
            //RTC_LOG(LS_INFO) << "delay next checkPendingSegment for " << minDelayedRequestTimeout << " ms";

            const auto weak = std::weak_ptr<StreamingMediaContextPrivate>(shared_from_this());
            _threads->getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->checkPendingSegment();
            }, (int32_t)minDelayedRequestTimeout);
        }

        if (allPartsDone) {
            std::shared_ptr<MediaSegment> segment = std::make_shared<MediaSegment>();
            segment->timestamp = _pendingSegment->timestamp;
            segment->duration = _segmentDuration;
            for (auto &part : _pendingSegment->parts) {
                const auto typeData = &part->typeData;
                if (const auto audioData = absl::get_if<PendingAudioSegmentData>(typeData)) {
                    segment->audio = std::make_shared<AudioStreamingPart>(std::move(part->result->data));
                } else if (const auto videoData = absl::get_if<PendingVideoSegmentData>(typeData)) {
                    auto videoSegment = std::make_shared<VideoSegment>();
                    videoSegment->part = std::make_shared<VideoStreamingPart>(std::move(part->result->data));
                    segment->video.push_back(videoSegment);
                }
            }
            _availableSegments.push_back(segment);

            //RTC_LOG(LS_INFO) << "commit segment " << segment->timestamp;

            _nextSegmentTimestamp = _pendingSegment->timestamp + _segmentDuration;
            discardPendingSegment();
            requestSegmentIfNeeded();
        }
    }

    void setActiveVideoChannels(std::vector<StreamingMediaContext::VideoChannel> const &videoChannels) {
        _activeVideoChannels = videoChannels;
    }

private:
    std::shared_ptr<Threads> _threads;
    std::function<std::shared_ptr<BroadcastPartTask>(int64_t, int64_t, std::function<void(BroadcastPart &&)>)> _requestAudioBroadcastPart;
    std::function<std::shared_ptr<BroadcastPartTask>(int64_t, int64_t, int32_t, VideoChannelDescription::Quality, std::function<void(BroadcastPart &&)>)> _requestVideoBroadcastPart;
    std::function<void(webrtc::VideoFrame const &)> _displayVideoFrame;

    const int64_t _segmentDuration = 500;
    int64_t _nextSegmentTimestamp = 0;

    std::vector<std::shared_ptr<MediaSegment>> _availableSegments;
    std::shared_ptr<PendingMediaSegment> _pendingSegment;

    int64_t _playbackReferenceTimestamp = 0;

    const size_t _audioDataRingBufferMaxSize = 4800;
    webrtc::Mutex _audioDataMutex;
    SampleRingBuffer _audioRingBuffer;
    std::vector<int16_t> _audioMixBuffer;

    const float kPi = 3.14159265f;
    float phase_ = 0.f;

    std::vector<StreamingMediaContext::VideoChannel> _activeVideoChannels;
};

StreamingMediaContext::StreamingMediaContext(StreamingMediaContextArguments &&arguments) {
    _private = std::make_shared<StreamingMediaContextPrivate>(std::move(arguments));
    _private->start();
}

StreamingMediaContext::~StreamingMediaContext() {
}

void StreamingMediaContext::setActiveVideoChannels(std::vector<VideoChannel> const &videoChannels) {
    _private->setActiveVideoChannels(videoChannels);
}

void StreamingMediaContext::getAudio(int16_t *audio_samples, const size_t num_samples, const uint32_t samples_per_sec) {
    _private->getAudio(audio_samples, num_samples, samples_per_sec);
}

}
