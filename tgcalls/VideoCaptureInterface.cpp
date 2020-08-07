#include "VideoCaptureInterface.h"

#include "VideoCaptureInterfaceImpl.h"

namespace tgcalls {

std::unique_ptr<VideoCaptureInterface> VideoCaptureInterface::Create() {
	return std::make_unique<VideoCaptureInterfaceImpl>();
}

VideoCaptureInterface::~VideoCaptureInterface() = default;

} // namespace tgcalls
