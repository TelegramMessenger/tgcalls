#ifndef TGCALLS_GROUP_INSTANCE_IMPL_H
#define TGCALLS_GROUP_INSTANCE_IMPL_H

#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>

namespace tgcalls {

class LogSinkImpl;
class GroupInstanceManager;

struct GroupInstanceDescriptor {
    std::function<void(std::string const &)> sdpAnswerEmitted;
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

private:
	std::unique_ptr<ThreadLocalObject<GroupInstanceManager>> _manager;
	std::unique_ptr<LogSinkImpl> _logSink;

};

} // namespace tgcalls

#endif
