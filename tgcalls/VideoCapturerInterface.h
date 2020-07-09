#ifndef TGCALLS_VIDEO_CAPTURER_INTERFACE_H
#define TGCALLS_VIDEO_CAPTURER_INTERFACE_H

namespace tgcalls {

class VideoCapturerInterface {
public:
	virtual ~VideoCapturerInterface() = default;

	virtual void setIsEnabled(bool isEnabled) = 0;
};

} // namespace tgcalls

#endif
