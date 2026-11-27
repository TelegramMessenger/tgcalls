#include "platform/darwin/Av1VideoToolboxDecoder.h"

#include "av1/Av1Obu.h"
#include "rtc_base/logging.h"

namespace tgcalls {

namespace {

// kCMVideoCodecType_AV1, spelled out for SDKs that predate the constant.
constexpr CMVideoCodecType kAv1CodecType = 'av01';

void DecompressionOutput(void *decompressionOutputRefCon, void *sourceFrameRefCon, OSStatus status,
                         VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer, CMTime presentationTimestamp,
                         CMTime presentationDuration) {
    auto *context = static_cast<Av1VideoToolboxDecoder::CallbackContext *>(decompressionOutputRefCon);
    if (status != noErr || !imageBuffer) {
        RTC_LOG(LS_ERROR) << "Av1VideoToolboxDecoder: decode failed, status " << status;
        context->status = status != noErr ? status : -1;
        return;
    }
    context->output(imageBuffer, uint32_t(reinterpret_cast<uintptr_t>(sourceFrameRefCon)));
}

CFDictionaryRef CreateDestinationAttributes() {
    CFMutableDictionaryRef attributes =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef ioSurface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pixelFormatNumber = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pixelFormat);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurface);
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, pixelFormatNumber);
    CFRelease(ioSurface);
    CFRelease(pixelFormatNumber);
    return attributes;
}

} // namespace

bool Av1VideoToolboxDecoder::IsSupported() {
    static const bool supported = VTIsHardwareDecodeSupported(kAv1CodecType);
    return supported;
}

Av1VideoToolboxDecoder::Av1VideoToolboxDecoder(Output output) : _context(std::make_unique<CallbackContext>()) {
    _context->output = std::move(output);
}

Av1VideoToolboxDecoder::~Av1VideoToolboxDecoder() {
    destroySession();
}

bool Av1VideoToolboxDecoder::Decode(const uint8_t *temporalUnit, size_t size, uint32_t rtpTimestamp) {
    const std::vector<Av1Obu> obus = ParseAv1Obus(temporalUnit, size);
    if (obus.empty()) {
        return false;
    }
    // A sample is the temporal unit without its temporal delimiter, as in an mp4 track.
    std::vector<uint8_t> sample;
    sample.reserve(size);
    for (const auto &obu : obus) {
        if (obu.type == kAv1ObuTemporalDelimiter) {
            continue;
        }
        if (obu.type == kAv1ObuSequenceHeader) {
            std::vector<uint8_t> record = MakeAv1CodecConfigurationRecord(obu);
            auto header = ParseAv1SequenceHeader(obu);
            if (!record.empty() && header && (record != _record || !_session)) {
                if (!recreateSession(record, header->maxWidth, header->maxHeight, header->fullRange)) {
                    return false;
                }
            }
        }
        sample.insert(sample.end(), obu.data, obu.data + obu.size);
    }
    if (!_session || sample.empty()) {
        return false;
    }

    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, sample.size(), nullptr, nullptr, 0,
                                                         sample.size(), kCMBlockBufferAssureMemoryNowFlag, &block);
    if (status != noErr || !block) {
        return false;
    }
    status = CMBlockBufferReplaceDataBytes(sample.data(), block, 0, sample.size());
    CMSampleBufferRef sampleBuffer = nullptr;
    const size_t sampleSize = sample.size();
    if (status == noErr) {
        status = CMSampleBufferCreateReady(nullptr, block, _format, 1, 0, nullptr, 1, &sampleSize, &sampleBuffer);
    }
    CFRelease(block);
    if (status != noErr || !sampleBuffer) {
        return false;
    }

    _context->status = noErr;
    status = VTDecompressionSessionDecodeFrame(_session, sampleBuffer, 0,
                                               reinterpret_cast<void *>(uintptr_t(rtpTimestamp)), nullptr);
    if (status == kVTInvalidSessionErr) {
        // The system invalidates sessions while the app is in the background: rebuild from the current record and
        // retry once, as the H.264/H.265 decoders do.
        const std::vector<uint8_t> record = _record;
        if (recreateSession(record, _width, _height, _fullRange)) {
            _context->status = noErr;
            status = VTDecompressionSessionDecodeFrame(_session, sampleBuffer, 0,
                                                       reinterpret_cast<void *>(uintptr_t(rtpTimestamp)), nullptr);
        }
    }
    CFRelease(sampleBuffer);
    if (status != noErr) {
        RTC_LOG(LS_ERROR) << "Av1VideoToolboxDecoder: VTDecompressionSessionDecodeFrame failed, status " << status;
        return false;
    }
    return _context->status == noErr;
}

bool Av1VideoToolboxDecoder::recreateSession(const std::vector<uint8_t> &record, uint32_t width, uint32_t height,
                                             bool fullRange) {
    destroySession();

    CFMutableDictionaryRef atoms =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef recordData = CFDataCreate(nullptr, record.data(), CFIndex(record.size()));
    CFDictionarySetValue(atoms, CFSTR("av1C"), recordData);
    CFRelease(recordData);
    CFMutableDictionaryRef extensions =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FullRangeVideo,
                         fullRange ? kCFBooleanTrue : kCFBooleanFalse);
    CFRelease(atoms);

    OSStatus status = CMVideoFormatDescriptionCreate(nullptr, kAv1CodecType, int32_t(width), int32_t(height),
                                                     extensions, &_format);
    CFRelease(extensions);
    if (status != noErr || !_format) {
        RTC_LOG(LS_ERROR) << "Av1VideoToolboxDecoder: format description failed, status " << status;
        _format = nullptr;
        return false;
    }

    CFDictionaryRef attributes = CreateDestinationAttributes();
    VTDecompressionOutputCallbackRecord callback = {DecompressionOutput, _context.get()};
    status = VTDecompressionSessionCreate(nullptr, _format, nullptr, attributes, &callback, &_session);
    CFRelease(attributes);
    if (status != noErr || !_session) {
        RTC_LOG(LS_ERROR) << "Av1VideoToolboxDecoder: session creation failed, status " << status;
        _session = nullptr;
        destroySession();
        return false;
    }
    _record = record;
    _width = width;
    _height = height;
    _fullRange = fullRange;
    RTC_LOG(LS_INFO) << "Av1VideoToolboxDecoder: session for " << width << "x" << height;
    return true;
}

void Av1VideoToolboxDecoder::destroySession() {
    if (_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(_session);
        VTDecompressionSessionInvalidate(_session);
        CFRelease(_session);
        _session = nullptr;
    }
    if (_format) {
        CFRelease(_format);
        _format = nullptr;
    }
    _record.clear();
}

} // namespace tgcalls
