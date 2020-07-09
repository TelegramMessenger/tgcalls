#include "VideoCaptureInterface.h"

#include "VideoCaptureInterfaceImpl.h"

namespace tgcalls {

std::shared_ptr<VideoCaptureInterface> VideoCaptureInterface::makeInstance() {
	return std::shared_ptr<VideoCaptureInterface>(new VideoCaptureInterfaceImpl());
}

VideoCaptureInterface::~VideoCaptureInterface() = default;

} // namespace tgcalls
