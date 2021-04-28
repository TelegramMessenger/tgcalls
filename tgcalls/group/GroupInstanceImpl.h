#ifndef TGCALLS_GROUP_INSTANCE_IMPL_H
#define TGCALLS_GROUP_INSTANCE_IMPL_H

#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>

#include "../Instance.h"

#include "../StaticThreads.h"
#include "group/GroupJoinPayload.h"

namespace webrtc {
class AudioDeviceModule;
class TaskQueueFactory;
class VideoTrackSourceInterface;
}

namespace rtc {
template <class T>
class scoped_refptr;
}

namespace tgcalls {

class LogSinkImpl;
class GroupInstanceManager;
struct AudioFrame;

struct GroupConfig {
    bool need_log{true};
    FilePath logPath;
};

struct GroupLevelValue {
    float level = 0.;
    bool voice = false;
};

struct GroupLevelUpdate {
    uint32_t ssrc = 0;
    GroupLevelValue value;
};

struct GroupLevelsUpdate {
    std::vector<GroupLevelUpdate> updates;
};

class BroadcastPartTask {
public:
    virtual ~BroadcastPartTask() = default;

    virtual void cancel() = 0;
};

struct BroadcastPart {
    enum class Status {
        Success,
        NotReady,
        ResyncNeeded
    };

    int64_t timestampMilliseconds = 0;
    double responseTimestamp = 0;
    Status status = Status::NotReady;
    std::vector<uint8_t> oggData;
};

enum class GroupConnectionMode {
    GroupConnectionModeNone,
    GroupConnectionModeRtc,
    GroupConnectionModeBroadcast
};

struct GroupNetworkState {
    bool isConnected = false;
    bool isTransitioningFromBroadcastToRtc = false;
};

enum VideoContentType {
    None,
    Screencast,
    Generic
};

enum VideoCodecName {
    VP8,
    VP9
};

struct GroupInstanceDescriptor {
    std::shared_ptr<Threads> threads;
    GroupConfig config;
    std::function<void(GroupNetworkState)> networkStateUpdated;
    std::function<void(GroupLevelsUpdate const &)> audioLevelsUpdated;
    std::function<void(uint32_t, const AudioFrame &)> onAudioFrame;
    std::string initialInputDeviceId;
    std::string initialOutputDeviceId;
    bool useDummyChannel{true};
    bool disableIncomingChannels{false};
    std::function<rtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory*)> createAudioDeviceModule;
    std::shared_ptr<VideoCaptureInterface> videoCapture; // deprecated
    std::function<webrtc::VideoTrackSourceInterface*()> getVideoSource;
    std::function<void(std::vector<uint32_t> const &)> incomingVideoSourcesUpdated;
    std::function<void(std::vector<uint32_t> const &)> participantDescriptionsRequired;
    std::function<std::shared_ptr<BroadcastPartTask>(int64_t, int64_t, std::function<void(BroadcastPart &&)>)> requestBroadcastPart;
    int outgoingAudioBitrateKbit{32};
    bool disableOutgoingAudioProcessing{false};
    VideoContentType videoContentType{VideoContentType::None};
    bool initialEnableNoiseSuppression{false};
    std::vector<VideoCodecName> videoCodecPreferences;
};

struct GroupParticipantDescription {
    uint32_t audioSsrc = 0;

    absl::optional<std::string> videoInformation;
    absl::optional<std::string> screencastInformation;
};

template <typename T>
class ThreadLocalObject;

class GroupInstanceInterface {
protected:
    GroupInstanceInterface() = default;

public:
    virtual ~GroupInstanceInterface() = default;

    virtual void stop() = 0;

    virtual void setConnectionMode(GroupConnectionMode connectionMode, bool keepBroadcastIfWasEnabled) = 0;

    virtual void emitJoinPayload(std::function<void(GroupJoinPayload const &)> completion) = 0;
    virtual void setJoinResponsePayload(std::string const &payload) = 0;
    virtual void addParticipants(std::vector<GroupParticipantDescription> &&participants) = 0;
    virtual void removeSsrcs(std::vector<uint32_t> ssrcs) = 0;
    virtual void removeIncomingVideoSource(uint32_t ssrc) = 0;

    virtual void setIsMuted(bool isMuted) = 0;
    virtual void setIsNoiseSuppressionEnabled(bool isNoiseSuppressionEnabled) = 0;
    virtual void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) = 0;
    //virtual void setVideoSource(std::function<webrtc::VideoTrackSourceInterface*()> getVideoSource) = 0;
    virtual void setAudioOutputDevice(std::string id) = 0;
    virtual void setAudioInputDevice(std::string id) = 0;

    virtual void addIncomingVideoOutput(uint32_t ssrc, std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) = 0;

    virtual void setVolume(uint32_t ssrc, double volume) = 0;
    virtual void setFullSizeVideoSsrc(uint32_t ssrc) = 0;

    struct AudioDevice {
      enum class Type {Input, Output};
      std::string name;
      std::string guid;
    };
    static std::vector<GroupInstanceInterface::AudioDevice> getAudioDevices(AudioDevice::Type type);
};

} // namespace tgcalls

#endif
