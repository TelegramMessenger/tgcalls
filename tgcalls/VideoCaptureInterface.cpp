#include "VideoCaptureInterface.h"

#include "VideoCaptureInterfaceImpl.h"

namespace tgcalls {

std::shared_ptr<VideoCaptureInterface> VideoCaptureInterface::Create() {
	return std::make_shared<VideoCaptureInterfaceImpl>();
}

VideoCaptureInterface::~VideoCaptureInterface() = default;

} // namespace tgcalls
