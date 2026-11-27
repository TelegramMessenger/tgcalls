#include "group/ScreencastSendParameters.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace tgcalls {

namespace {

// H264 level_idc of level 5.2 (0x34).
constexpr long kScreencastH264Level = 52;
constexpr int kScreencastMaxBitrateBps = 2000000;
constexpr int kScreencastAudioHeadroomBps = 32000;
constexpr int kScreencastMaxFramerate = 60;
// See iosScreencastLayerParameters.
constexpr double kScreencastTopLayerMaxPixels = 1000000.0;
constexpr double kScreencastLowLayerShortSide = 240.0;
constexpr double kScreencastMiddleLayerShortSide = 480.0;

ScreencastLayerParameters layer(int minBitrateBps, int maxBitrateBps, double scaleResolutionDownBy) {
    ScreencastLayerParameters result;
    result.minBitrateBps = minBitrateBps;
    result.maxBitrateBps = maxBitrateBps;
    result.scaleResolutionDownBy = scaleResolutionDownBy;
    result.maxFramerate = kScreencastMaxFramerate;
    return result;
}

} // namespace

std::vector<ScreencastLayerParameters> iosScreencastLayerParameters(int layerCount, int frameWidth, int frameHeight) {
    const bool isSizeKnown = frameWidth > 0 && frameHeight > 0;
    const double pixels = isSizeKnown ? double(frameWidth) * double(frameHeight) : 0.0;
    const double shortSide = isSizeKnown ? double(std::min(frameWidth, frameHeight)) : 0.0;

    const double topScale = isSizeKnown ? std::max(1.0, std::sqrt(pixels / kScreencastTopLayerMaxPixels)) : 1.0;
    const double middleScale = isSizeKnown ? std::max(1.0, shortSide / kScreencastMiddleLayerShortSide) : 2.0;
    const double lowScale = isSizeKnown ? std::max(1.0, shortSide / kScreencastLowLayerShortSide) : (layerCount == 3 ? 4.0 : 2.0);

    switch (layerCount) {
        case 3:
            return {
                layer(50000, 100000, lowScale),
                layer(150000, 200000, middleScale),
                layer(300000, kScreencastMaxBitrateBps, topScale)
            };
        case 2:
            return {
                layer(50000, 100000, lowScale),
                layer(200000, kScreencastMaxBitrateBps, topScale)
            };
        default:
            return {
                layer(0, kScreencastMaxBitrateBps, topScale)
            };
    }
}

int iosScreencastTotalMaxBitrateBps() {
    return kScreencastMaxBitrateBps + kScreencastAudioHeadroomBps;
}

std::string iosScreencastFieldTrial(std::string const &key) {
    if (key == "WebRTC-Bwe-ProbingConfiguration") {
        return "skip_if_est_larger_than_fraction_of_max:0.9";
    }
    return std::string();
}

std::string iosScreencastH264ProfileLevelId(std::string const &profileLevelId) {
    if (profileLevelId.size() != 6) {
        return profileLevelId;
    }
    for (const char c : profileLevelId) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return profileLevelId;
        }
    }
    const long level = std::strtol(profileLevelId.substr(4, 2).c_str(), nullptr, 16);
    if (level >= kScreencastH264Level) {
        return profileLevelId;
    }
    return profileLevelId.substr(0, 4) + "34";
}

} // namespace tgcalls
