#ifndef TGCALLS_GROUP_INSTANCE_IMPL_H
#define TGCALLS_GROUP_INSTANCE_IMPL_H

#include "rtc_base/copy_on_write_buffer.h"


#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>

#include "Instance.h"

namespace webrtc {
class AudioDeviceModule;
class TaskQueueFactory;
}

namespace tgcalls {

class LogSinkImpl;
class GroupInstanceManager;

struct GroupInstanceDescriptor {
    std::function<void(bool)> networkStateUpdated;
    std::function<void(std::vector<std::pair<uint32_t, float>> const &)> audioLevelsUpdated;
};

struct GroupJoinPayloadFingerprint {
    std::string hash;
    std::string setup;
    std::string fingerprint;
};

struct GroupJoinPayload {
    std::string ufrag;
    std::string pwd;
    std::vector<GroupJoinPayloadFingerprint> fingerprints;
    
    uint32_t ssrc = 0;
};

struct GroupJoinResponseCandidate {
    std::string port;
    std::string protocol;
    std::string network;
    std::string generation;
    std::string id;
    std::string component;
    std::string foundation;
    std::string priority;
    std::string ip;
    std::string type;
    
    std::string tcpType;
    std::string relAddr;
    std::string relPort;
};

struct GroupJoinResponsePayload {
    std::string ufrag;
    std::string pwd;
    std::vector<GroupJoinPayloadFingerprint> fingerprints;
    std::vector<GroupJoinResponseCandidate> candidates;
};

template <typename T>
class ThreadLocalObject;

class GroupInstanceImpl final {
public:
	explicit GroupInstanceImpl(GroupInstanceDescriptor &&descriptor);
	~GroupInstanceImpl();
    
    
    void stop();
    
    void emitJoinPayload(std::function<void(GroupJoinPayload)> completion);
    void setJoinResponsePayload(GroupJoinResponsePayload payload);
    void setSsrcs(std::vector<uint32_t> ssrcs);
    
    void setIsMuted(bool isMuted);

private:
	std::unique_ptr<ThreadLocalObject<GroupInstanceManager>> _manager;
	std::unique_ptr<LogSinkImpl> _logSink;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule();
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;

};

} // namespace tgcalls

#endif
