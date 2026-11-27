#ifndef TGCALLS_CAMERA_FRAME_ROTATION_H
#define TGCALLS_CAMERA_FRAME_ROTATION_H

// The rotation VideoCameraCapturer tags its frames with, kept free of UIKit so a host test can pin it
// (Tests/CameraFrameRotationTest.cpp). The enums carry the raw values of UIDeviceOrientation and
// UIInterfaceOrientation; VideoCameraCapturer.mm static_asserts that they match.

namespace tgcalls {

enum class CameraDeviceOrientation : int {
    Unknown = 0,
    Portrait = 1,
    PortraitUpsideDown = 2,
    LandscapeLeft = 3,
    LandscapeRight = 4,
    FaceUp = 5,
    FaceDown = 6
};

// UIKit names the landscape interface orientations after the side the Home button ends up on, the device ones from
// the opposite point of view: UIInterfaceOrientationLandscapeLeft == UIDeviceOrientationLandscapeRight.
enum class CameraInterfaceOrientation : int {
    Unknown = 0,
    Portrait = 1,
    PortraitUpsideDown = 2,
    LandscapeRight = 3,
    LandscapeLeft = 4
};

inline bool cameraDeviceOrientationIsScreenFacing(CameraDeviceOrientation orientation) {
    switch (orientation) {
        case CameraDeviceOrientation::Portrait:
        case CameraDeviceOrientation::PortraitUpsideDown:
        case CameraDeviceOrientation::LandscapeLeft:
        case CameraDeviceOrientation::LandscapeRight:
            return true;
        default:
            return false;
    }
}

// The device orientation that UIKit defines as the same physical position as an interface orientation. Unknown maps
// to portrait.
inline CameraDeviceOrientation cameraDeviceOrientationMatching(CameraInterfaceOrientation orientation) {
    switch (orientation) {
        case CameraInterfaceOrientation::PortraitUpsideDown:
            return CameraDeviceOrientation::PortraitUpsideDown;
        case CameraInterfaceOrientation::LandscapeLeft:
            return CameraDeviceOrientation::LandscapeRight;
        case CameraInterfaceOrientation::LandscapeRight:
            return CameraDeviceOrientation::LandscapeLeft;
        default:
            return CameraDeviceOrientation::Portrait;
    }
}

// The orientation the capturer starts from. The device's own, when it faces a screen-facing way; otherwise (lying
// flat or tilted far back, which an iPad on a stand or a table reports as face up, or not known yet: UIDevice can
// learn it only after orientation notifications begin) the app's interface orientation. On iPad the
// interface follows the device into all four orientations, so it is upside down whenever the iPad was last held
// upside down; a portrait stand-in sent such an iPad's camera rotated 180 degrees for as long as it lay that way.
inline CameraDeviceOrientation initialCameraDeviceOrientation(CameraDeviceOrientation deviceOrientation, CameraInterfaceOrientation interfaceOrientation) {
    if (cameraDeviceOrientationIsScreenFacing(deviceOrientation)) {
        return deviceOrientation;
    }
    return cameraDeviceOrientationMatching(interfaceOrientation);
}

// Degrees (0, 90, 180, 270) a frame is to be turned to read upright against gravity, for a camera whose buffers come
// in landscape (the video data output's default). A device orientation that faces no screen-facing way says nothing,
// and keeps `currentRotation`. The front camera's buffer is mirrored, so its landscape rotations are swapped.
inline int cameraFrameRotation(CameraDeviceOrientation deviceOrientation, bool isFrontCamera, int currentRotation) {
    switch (deviceOrientation) {
        case CameraDeviceOrientation::Portrait:
            return 90;
        case CameraDeviceOrientation::PortraitUpsideDown:
            return 270;
        case CameraDeviceOrientation::LandscapeLeft:
            return isFrontCamera ? 180 : 0;
        case CameraDeviceOrientation::LandscapeRight:
            return isFrontCamera ? 0 : 180;
        default:
            return currentRotation;
    }
}

} // namespace tgcalls

#endif
