#ifndef TGCALLS_LOG_SINK_IMPL_H
#define TGCALLS_LOG_SINK_IMPL_H

#include "rtc_base/logging.h"
#include <fstream>
#include <sstream>

namespace tgcalls {

struct FilePath;

class LogSinkImpl final : public webrtc::LogSink {
public:
	LogSinkImpl(const FilePath &logPath);

	void OnLogMessage(const std::string &msg, webrtc::LoggingSeverity severity, const char *tag) override;
	void OnLogMessage(const std::string &message, webrtc::LoggingSeverity severity) override;
	void OnLogMessage(const std::string &message) override;

	std::string result() const {
		return _data.str();
	}

private:
	std::ofstream _file;
	std::ostringstream _data;

};

} // namespace tgcalls

#endif
