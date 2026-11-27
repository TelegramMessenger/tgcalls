// The rotation VideoCameraCapturer tags its frames with (platform/darwin/CameraFrameRotation.h): from the device
// orientation, and at capture start from the interface orientation when the device faces no screen-facing way.

#include "platform/darwin/CameraFrameRotation.h"

#include <cstdio>
#include <initializer_list>

static int failures = 0;

#define CHECK_EQ(actual, expected)                                                                          \
    do {                                                                                                    \
        const int actual_ = static_cast<int>(actual);                                                       \
        const int expected_ = static_cast<int>(expected);                                                   \
        if (actual_ != expected_) {                                                                         \
            std::fprintf(stderr, "%s:%d: %s is %d, expected %d\n", __FILE__, __LINE__, #actual, actual_, expected_); \
            failures++;                                                                                     \
        }                                                                                                   \
    } while (0)

using tgcalls::CameraDeviceOrientation;
using tgcalls::CameraInterfaceOrientation;
using tgcalls::cameraDeviceOrientationMatching;
using tgcalls::cameraFrameRotation;
using tgcalls::initialCameraDeviceOrientation;

namespace {

const CameraDeviceOrientation kScreenFacing[] = {
    CameraDeviceOrientation::Portrait,
    CameraDeviceOrientation::PortraitUpsideDown,
    CameraDeviceOrientation::LandscapeLeft,
    CameraDeviceOrientation::LandscapeRight,
};

const CameraDeviceOrientation kNotScreenFacing[] = {
    CameraDeviceOrientation::Unknown,
    CameraDeviceOrientation::FaceUp,
    CameraDeviceOrientation::FaceDown,
};

const CameraInterfaceOrientation kInterfaceOrientations[] = {
    CameraInterfaceOrientation::Unknown,
    CameraInterfaceOrientation::Portrait,
    CameraInterfaceOrientation::PortraitUpsideDown,
    CameraInterfaceOrientation::LandscapeLeft,
    CameraInterfaceOrientation::LandscapeRight,
};

// The table the capturer has always used, for both cameras.
void testFrameRotationTable() {
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::Portrait, true, -1), 90);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::Portrait, false, -1), 90);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::PortraitUpsideDown, true, -1), 270);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::PortraitUpsideDown, false, -1), 270);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::LandscapeLeft, true, -1), 180);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::LandscapeLeft, false, -1), 0);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::LandscapeRight, true, -1), 0);
    CHECK_EQ(cameraFrameRotation(CameraDeviceOrientation::LandscapeRight, false, -1), 180);
}

// Lying flat, or an orientation not known yet, says nothing: the rotation stays what it was.
void testNotScreenFacingKeepsTheRotation() {
    for (CameraDeviceOrientation orientation : kNotScreenFacing) {
        for (int current : {0, 90, 180, 270}) {
            CHECK_EQ(cameraFrameRotation(orientation, true, current), current);
            CHECK_EQ(cameraFrameRotation(orientation, false, current), current);
        }
    }
}

// Turning the device half a turn turns the frame half a turn, whichever camera.
void testHalfTurnOfTheDeviceIsHalfTurnOfTheFrame() {
    for (bool isFront : {true, false}) {
        CHECK_EQ((cameraFrameRotation(CameraDeviceOrientation::Portrait, isFront, -1) + 180) % 360,
                 cameraFrameRotation(CameraDeviceOrientation::PortraitUpsideDown, isFront, -1));
        CHECK_EQ((cameraFrameRotation(CameraDeviceOrientation::LandscapeLeft, isFront, -1) + 180) % 360,
                 cameraFrameRotation(CameraDeviceOrientation::LandscapeRight, isFront, -1));
    }
}

// UIKit's landscape names are swapped between the two enums.
void testInterfaceToDevice() {
    CHECK_EQ(cameraDeviceOrientationMatching(CameraInterfaceOrientation::Portrait), CameraDeviceOrientation::Portrait);
    CHECK_EQ(cameraDeviceOrientationMatching(CameraInterfaceOrientation::PortraitUpsideDown), CameraDeviceOrientation::PortraitUpsideDown);
    CHECK_EQ(cameraDeviceOrientationMatching(CameraInterfaceOrientation::LandscapeLeft), CameraDeviceOrientation::LandscapeRight);
    CHECK_EQ(cameraDeviceOrientationMatching(CameraInterfaceOrientation::LandscapeRight), CameraDeviceOrientation::LandscapeLeft);
    CHECK_EQ(cameraDeviceOrientationMatching(CameraInterfaceOrientation::Unknown), CameraDeviceOrientation::Portrait);
}

// A screen-facing device orientation wins over the interface, whatever the interface is (an iPhone screen locked
// to portrait while the phone is held in landscape).
void testDeviceOrientationWinsWhenScreenFacing() {
    for (CameraDeviceOrientation device : kScreenFacing) {
        for (CameraInterfaceOrientation interface : kInterfaceOrientations) {
            CHECK_EQ(initialCameraDeviceOrientation(device, interface), device);
        }
    }
}

// Issue #255: an iPad held upside down, lying back on a stand (face up) when the camera starts, with the interface
// upside down. The front camera's frames, and the preview drawn from them, must be tagged 270 (upside down), not 90
// (the portrait stand-in), which showed and sent the picture rotated 180 degrees.
void testIpadUpsideDownLyingBack() {
    for (CameraDeviceOrientation device : kNotScreenFacing) {
        const CameraDeviceOrientation initial = initialCameraDeviceOrientation(device, CameraInterfaceOrientation::PortraitUpsideDown);
        CHECK_EQ(initial, CameraDeviceOrientation::PortraitUpsideDown);
        CHECK_EQ(cameraFrameRotation(initial, true, 90), 270);
        CHECK_EQ(cameraFrameRotation(initial, false, 90), 270);
    }
}

// The same with the interface in either landscape: the frame follows the interface, each camera with its own sign.
void testIpadLandscapeLyingFlat() {
    for (CameraDeviceOrientation device : kNotScreenFacing) {
        // Interface landscape-left: the device's landscape-right (Home button / USB-C on the left).
        const CameraDeviceOrientation left = initialCameraDeviceOrientation(device, CameraInterfaceOrientation::LandscapeLeft);
        CHECK_EQ(left, CameraDeviceOrientation::LandscapeRight);
        CHECK_EQ(cameraFrameRotation(left, true, 90), 0);
        CHECK_EQ(cameraFrameRotation(left, false, 90), 180);

        const CameraDeviceOrientation right = initialCameraDeviceOrientation(device, CameraInterfaceOrientation::LandscapeRight);
        CHECK_EQ(right, CameraDeviceOrientation::LandscapeLeft);
        CHECK_EQ(cameraFrameRotation(right, true, 90), 180);
        CHECK_EQ(cameraFrameRotation(right, false, 90), 0);

        const CameraDeviceOrientation portrait = initialCameraDeviceOrientation(device, CameraInterfaceOrientation::Portrait);
        CHECK_EQ(portrait, CameraDeviceOrientation::Portrait);
        CHECK_EQ(cameraFrameRotation(portrait, true, 0), 90);
    }
}

// Nothing known at all (no screen-facing device orientation, no interface orientation) is portrait, as before. The
// capturer passes an unknown interface orientation for a scene on another screen than the main one (iPhone Duo's inner
// screen, whose portrait is not the body's), which keeps the portrait stand-in that screen had.
void testNothingKnownIsPortrait() {
    for (CameraDeviceOrientation device : kNotScreenFacing) {
        CHECK_EQ(initialCameraDeviceOrientation(device, CameraInterfaceOrientation::Unknown), CameraDeviceOrientation::Portrait);
    }
}

} // namespace

int main() {
    testFrameRotationTable();
    testNotScreenFacingKeepsTheRotation();
    testHalfTurnOfTheDeviceIsHalfTurnOfTheFrame();
    testInterfaceToDevice();
    testDeviceOrientationWinsWhenScreenFacing();
    testIpadUpsideDownLyingBack();
    testIpadLandscapeLyingFlat();
    testNothingKnownIsPortrait();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("camera_frame_rotation_test: all checks passed\n");
    return 0;
}
