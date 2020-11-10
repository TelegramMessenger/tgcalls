#ifndef TGCALLS_GROUP_INSTANCE_IMPL_H
#define TGCALLS_GROUP_INSTANCE_IMPL_H

#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>

#include "Instance.h"

namespace tgcalls {

class LogSinkImpl;
class GroupInstanceManager;

struct GroupInstanceDescriptor {
    std::function<void(std::string const &)> sdpAnswerEmitted;
    std::function<void(std::vector<std::string> const &)> incomingVideoStreamListUpdated;
    std::shared_ptr<VideoCaptureInterface> videoCapture;
};

template <typename T>
class ThreadLocalObject;

class GroupInstanceImpl final {
public:
	explicit GroupInstanceImpl(GroupInstanceDescriptor &&descriptor);
	~GroupInstanceImpl();
    
    void emitOffer();
    void setOfferSdp(std::string const &offerSdp, bool isPartial);
    void setIsMuted(bool isMuted);
    void setIncomingVideoOutput(std::string const &streamId, std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);

private:
	std::unique_ptr<ThreadLocalObject<GroupInstanceManager>> _manager;
	std::unique_ptr<LogSinkImpl> _logSink;

};

} // namespace tgcalls

#endif
