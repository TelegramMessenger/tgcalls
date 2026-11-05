#include "group/ScreencastSendParameters.h"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

using tgcalls::ScreencastLayerParameters;
using tgcalls::iosScreencastLayerParameters;
using tgcalls::iosScreencastTotalMaxBitrateBps;

bool near(double a, double b) {
    return a - b < 0.001 && b - a < 0.001;
}

bool layerEquals(ScreencastLayerParameters const &layer, int minBitrateBps, int maxBitrateBps, double scaleResolutionDownBy, int maxFramerate) {
    return layer.minBitrateBps == minBitrateBps
        && layer.maxBitrateBps == maxBitrateBps
        && near(layer.scaleResolutionDownBy, scaleResolutionDownBy)
        && layer.maxFramerate == maxFramerate;
}

void TestSingleLayerIsCappedAtThreeMegabitsAndOneMegapixel() {
    // Frame size not known yet: no scaling.
    auto layers = iosScreencastLayerParameters(1);
    CHECK_TRUE(layers.size() == 1);
    if (layers.size() == 1) {
        CHECK_TRUE(layerEquals(layers[0], 0, 2000000, 1.0, 60));
    }
    // An unfolded iPhone Duo screen (2004x2850, 5.7 Mpx) is brought down to 1 Mpx.
    layers = iosScreencastLayerParameters(1, 2004, 2850);
    CHECK_TRUE(layers.size() == 1);
    if (layers.size() == 1) {
        CHECK_TRUE(layerEquals(layers[0], 0, 2000000, 2.389854, 60));
    }
    // A frame already at most 1 Mpx is not scaled.
    layers = iosScreencastLayerParameters(1, 836, 1192);
    if (layers.size() == 1) {
        CHECK_TRUE(near(layers[0].scaleResolutionDownBy, 1.0));
    }
}

void TestTwoLayersAreTwoFortyPAndOneMegapixel() {
    auto layers = iosScreencastLayerParameters(2, 2004, 2850);
    CHECK_TRUE(layers.size() == 2);
    if (layers.size() == 2) {
        // 240 lines on the short side.
        CHECK_TRUE(layerEquals(layers[0], 50000, 100000, 2004.0 / 240.0, 60));
        CHECK_TRUE(layerEquals(layers[1], 200000, 2000000, 2.389854, 60));
    }
    layers = iosScreencastLayerParameters(2, 836, 1192);
    if (layers.size() == 2) {
        CHECK_TRUE(near(layers[0].scaleResolutionDownBy, 836.0 / 240.0));
        CHECK_TRUE(near(layers[1].scaleResolutionDownBy, 1.0));
    }
    // Landscape: the short side is the height.
    layers = iosScreencastLayerParameters(2, 2850, 2004);
    if (layers.size() == 2) {
        CHECK_TRUE(near(layers[0].scaleResolutionDownBy, 2004.0 / 240.0));
    }
    // Never upscaled.
    layers = iosScreencastLayerParameters(2, 200, 300);
    if (layers.size() == 2) {
        CHECK_TRUE(near(layers[0].scaleResolutionDownBy, 1.0));
        CHECK_TRUE(near(layers[1].scaleResolutionDownBy, 1.0));
    }
    // Frame size not known yet: the lower layer at half size, as before.
    layers = iosScreencastLayerParameters(2);
    if (layers.size() == 2) {
        CHECK_TRUE(layerEquals(layers[0], 50000, 100000, 2.0, 60));
        CHECK_TRUE(layerEquals(layers[1], 200000, 2000000, 1.0, 60));
    }
}

void TestThreeLayersAreTwoFortyFourEightyAndOneMegapixel() {
    auto layers = iosScreencastLayerParameters(3, 2004, 2850);
    CHECK_TRUE(layers.size() == 3);
    if (layers.size() == 3) {
        CHECK_TRUE(layerEquals(layers[0], 50000, 100000, 2004.0 / 240.0, 60));
        CHECK_TRUE(layerEquals(layers[1], 150000, 200000, 2004.0 / 480.0, 60));
        CHECK_TRUE(layerEquals(layers[2], 300000, 2000000, 2.389854, 60));
    }
    layers = iosScreencastLayerParameters(3);
    if (layers.size() == 3) {
        CHECK_TRUE(layerEquals(layers[0], 50000, 100000, 4.0, 60));
        CHECK_TRUE(layerEquals(layers[1], 150000, 200000, 2.0, 60));
        CHECK_TRUE(layerEquals(layers[2], 300000, 2000000, 1.0, 60));
    }
}

void TestTotalLeavesAudioHeadroom() {
    CHECK_TRUE(iosScreencastTotalMaxBitrateBps() == 2032000);
}

void TestH264ProfileLevelIdIsRaisedToLevel52() {
    // The profile (and its constraint flags) stays; only the level byte is raised, to 5.2 (0x34).
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("42e01f") == "42e034");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("640c1f") == "640c34");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("640C29") == "640C34");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("640c34") == "640c34");
    // A higher level is never lowered; anything that is not six hex digits is left alone.
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("640c3d") == "640c3d");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("") == "");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("42e0") == "42e0");
    CHECK_TRUE(tgcalls::iosScreencastH264ProfileLevelId("42e0zz") == "42e0zz");
}

} // namespace

int main() {
    TestSingleLayerIsCappedAtThreeMegabitsAndOneMegapixel();
    TestTwoLayersAreTwoFortyPAndOneMegapixel();
    TestThreeLayersAreTwoFortyFourEightyAndOneMegapixel();
    TestTotalLeavesAudioHeadroom();
    TestH264ProfileLevelIdIsRaisedToLevel52();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
