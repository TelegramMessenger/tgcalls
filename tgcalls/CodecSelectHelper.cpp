#include "CodecSelectHelper.h"

#include "platform/PlatformInterface.h"

#include "media/base/media_constants.h"
#include "absl/strings/match.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/field_trial.h"

namespace tgcalls {
namespace {

using VideoFormat = webrtc::SdpVideoFormat;

bool CompareFormats(const VideoFormat &a, const VideoFormat &b) {
	if (a.name < b.name) {
		return true;
	} else if (b.name < a.name) {
		return false;
	} else {
		return a.parameters < b.parameters;
	}
}

int FormatPriority(const VideoFormat &format) {
	static const auto kCodecs = {
		std::string(cricket::kAv1CodecName),
		std::string(cricket::kH265CodecName),
		std::string(cricket::kVp9CodecName),
		std::string(cricket::kH264CodecName),
		std::string(cricket::kVp8CodecName),
	};
	const auto platform = PlatformInterface::SharedInstance();

	auto result = 0;
	for (const auto &name : kCodecs) {
		if (format.name == name && platform->supportsEncoding(name)) {
			return result;
		}
		++result;
	}
	return -1;
}

bool ComparePriorities(const VideoFormat &a, const VideoFormat &b) {
	return FormatPriority(a) < FormatPriority(b);
}

std::vector<VideoFormat> FilterAndSortEncoders(std::vector<VideoFormat> list) {
	const auto listBegin = begin(list);
	const auto listEnd = end(list);
	std::sort(listBegin, listEnd, ComparePriorities);
	auto eraseFrom = listBegin;
	auto eraseTill = eraseFrom;
	while (eraseTill != listEnd && FormatPriority(*eraseTill) == -1) {
		++eraseTill;
	}
	if (eraseTill != eraseFrom) {
		list.erase(eraseFrom, eraseTill);
	}
	return list;
}

std::vector<VideoFormat> AppendUnique(
		std::vector<VideoFormat> list,
		std::vector<VideoFormat> other) {
	if (list.empty()) {
		return other;
	}
	list.reserve(list.size() + other.size());
	const auto oldBegin = &list[0];
	const auto oldEnd = oldBegin + list.size();
	for (auto &format : other) {
		if (std::find(oldBegin, oldEnd, format) == oldEnd) {
			list.push_back(std::move(format));
		}
	}
	return list;
}

void AddDefaultFeedbackParams(cricket::VideoCodec *codec) {
	// Don't add any feedback params for RED and ULPFEC.
	if (codec->name == cricket::kRedCodecName || codec->name == cricket::kUlpfecCodecName)
		return;
	codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamRemb, cricket::kParamValueEmpty));
	codec->AddFeedbackParam(
		cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc, cricket::kParamValueEmpty));
	// Don't add any more feedback params for FLEXFEC.
	if (codec->name == cricket::kFlexfecCodecName)
		return;
	codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamCcm, cricket::kRtcpFbCcmParamFir));
	codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kParamValueEmpty));
	codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kRtcpFbNackParamPli));
	if (codec->name == cricket::kVp8CodecName &&
		webrtc::field_trial::IsEnabled("WebRTC-RtcpLossNotification")) {
		codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamLntf, cricket::kParamValueEmpty));
	}
}

} // namespace

VideoFormatsMessage ComposeSupportedFormats(
		std::vector<VideoFormat> encoders,
		std::vector<VideoFormat> decoders) {
	encoders = FilterAndSortEncoders(std::move(encoders));

	auto result = VideoFormatsMessage();
	result.encodersCount = encoders.size();
	result.formats = AppendUnique(std::move(encoders), std::move(decoders));
	for (const auto &format : result.formats) {
		RTC_LOG(LS_INFO) << "Format: " << format.ToString();
	}
	RTC_LOG(LS_INFO) << "First " << result.encodersCount << " formats are supported encoders.";
	return result;
}

CommonFormats ComputeCommonFormats(
		const VideoFormatsMessage &my,
		VideoFormatsMessage their) {
	assert(my.encodersCount <= my.formats.size());
	assert(their.encodersCount <= their.formats.size());

	for (const auto &format : their.formats) {
		RTC_LOG(LS_INFO) << "Their format: " << format.ToString();
	}
	RTC_LOG(LS_INFO) << "Their first " << their.encodersCount << " formats are supported encoders.";

	const auto myBegin = begin(my.formats);
	const auto myEnd = end(my.formats);
	const auto myEncodersBegin = myBegin;
	const auto myEncodersEnd = myBegin + my.encodersCount;
	const auto theirBegin = begin(their.formats);
	const auto theirEnd = end(their.formats);
	const auto theirEncodersBegin = theirBegin;
	const auto theirEncodersEnd = theirBegin + their.encodersCount;

	auto result = CommonFormats();
	auto myEncoderFormat = VideoFormat(std::string());
	auto theirEncoderIndex = their.encodersCount;
	result.list.reserve(my.formats.size() + their.formats.size());
	for (auto i = myEncodersBegin; i != myEncodersEnd; ++i) {
		const auto j = std::find(theirBegin, theirEnd, *i);
		if (j != theirEnd) {
			if (myEncoderFormat.name.empty()) {
				myEncoderFormat = *i;
			}
			result.list.push_back(*i);

			const auto theirIndex = (j - theirBegin);
			if (theirIndex < theirEncoderIndex) {
				theirEncoderIndex = theirIndex;
			}
		}
	}
	auto theirEncoderFormat = (theirEncoderIndex < their.encodersCount)
		? their.formats[theirEncoderIndex]
		: VideoFormat(std::string());
	for (auto i = theirEncodersBegin; i != theirEncodersEnd; ++i) {
		if (std::find(myEncodersEnd, myEnd, *i) != myEnd) {
			if (theirEncoderFormat.name.empty()) {
				theirEncoderFormat = *i;
			}
			result.list.push_back(std::move(*i));
		}
	}
	std::sort(begin(result.list), end(result.list), CompareFormats);
	if (!myEncoderFormat.name.empty()) {
		const auto i = std::find(begin(result.list), end(result.list), myEncoderFormat);
		assert(i != end(result.list));
		result.myEncoderIndex = (i - begin(result.list));
	}

	for (const auto &format : result.list) {
		RTC_LOG(LS_INFO) << "Common format: " << format.ToString();
	}
	RTC_LOG(LS_INFO) << "My encoder: " << (result.myEncoderIndex >= 0 ? result.list[result.myEncoderIndex].ToString() : "(null)");
	RTC_LOG(LS_INFO) << "Their encoder: " << (!theirEncoderFormat.name.empty() ? theirEncoderFormat.ToString() : "(null)");

	return result;
}

CommonCodecs AssignPayloadTypesAndDefaultCodecs(CommonFormats &&formats) {
	if (formats.list.empty()) {
		return CommonCodecs();
	}

	constexpr int kFirstDynamicPayloadType = 96;
	constexpr int kLastDynamicPayloadType = 127;

	int payload_type = kFirstDynamicPayloadType;

	formats.list.push_back(webrtc::SdpVideoFormat(cricket::kRedCodecName));
	formats.list.push_back(webrtc::SdpVideoFormat(cricket::kUlpfecCodecName));

	if (true) {
		webrtc::SdpVideoFormat flexfec_format(cricket::kFlexfecCodecName);
		// This value is currently arbitrarily set to 10 seconds. (The unit
		// is microseconds.) This parameter MUST be present in the SDP, but
		// we never use the actual value anywhere in our code however.
		// TODO(brandtr): Consider honouring this value in the sender and receiver.
		flexfec_format.parameters = { {cricket::kFlexfecFmtpRepairWindow, "10000000"} };
		formats.list.push_back(flexfec_format);
	}

	auto inputIndex = 0;
	auto result = CommonCodecs();
	result.list.reserve(2 * formats.list.size() - 2);
	for (const auto &format : formats.list) {
		cricket::VideoCodec codec(format);
		codec.id = payload_type;
		AddDefaultFeedbackParams(&codec);

		if (inputIndex++ == formats.myEncoderIndex) {
			result.myEncoderIndex = result.list.size();
		}
		result.list.push_back(codec);

		// Increment payload type.
		++payload_type;
		if (payload_type > kLastDynamicPayloadType) {
			RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
			break;
		}

		// Add associated RTX codec for non-FEC codecs.
		if (!absl::EqualsIgnoreCase(codec.name, cricket::kUlpfecCodecName) &&
			!absl::EqualsIgnoreCase(codec.name, cricket::kFlexfecCodecName)) {
			result.list.push_back(cricket::VideoCodec::CreateRtxCodec(payload_type, codec.id));

			// Increment payload type.
			++payload_type;
			if (payload_type > kLastDynamicPayloadType) {
				RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
				break;
			}
		}
	}
	return result;
}

} // namespace tgcalls
