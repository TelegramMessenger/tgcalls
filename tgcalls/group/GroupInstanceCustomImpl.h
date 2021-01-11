#ifndef TGCALLS_GROUP_INSTANCE_CUSTOM_IMPL_H
#define TGCALLS_GROUP_INSTANCE_CUSTOM_IMPL_H

#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>

#include "../Instance.h"

namespace webrtc {
class AudioDeviceModule;
class TaskQueueFactory;
}

namespace tgcalls {

class LogSinkImpl;
class GroupInstanceCustomManager;

struct GroupInstanceCustomDescriptor {
    std::function<void(std::vector<uint8_t> const &)> sendPacket;
};

template <typename T>
class ThreadLocalObject;

class GroupInstanceCustomImpl final {
public:
    explicit GroupInstanceCustomImpl(GroupInstanceCustomDescriptor &&descriptor);
    ~GroupInstanceCustomImpl();

    void stop();

    void receivePacket(std::vector<uint8_t> &&data);

private:
    std::unique_ptr<ThreadLocalObject<GroupInstanceCustomManager>> _manager;

};

} // namespace tgcalls

#endif
