#include "VideoCaptureInterface.h"

#include "VideoCaptureInterfaceImpl.h"

namespace tgcalls {

std::unique_ptr<VideoCaptureInterface> VideoCaptureInterface::Create(std::shared_ptr<PlatformContext> platformContext, bool screenCast) {
	return std::make_unique<VideoCaptureInterfaceImpl>(platformContext, screenCast);
}

VideoCaptureInterface::~VideoCaptureInterface() = default;

} // namespace tgcalls
