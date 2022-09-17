// swift-tools-version:5.5
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "TgVoipWebrtc",
    platforms: [.macOS(.v10_12)],
    products: [
        .library(
            name: "TgVoipWebrtc",
            targets: ["TgVoipWebrtc"]),
    ],
    dependencies: [
    ],
    targets: [
        .target(
            name: "TgVoipWebrtc",
            dependencies: [],
            path: ".",
            exclude: ["LICENSE",
                      "README.md",
                      "tgcalls/platform/android",
                      "tgcalls/platform/tdesktop",
                      "tgcalls/platform/uwp",
                      "tgcalls/platform/fake",
                      "tgcalls/platform/darwin/iOS",
                      "tgcalls/platform/darwin/VideoCaptureView.mm",
                      "tgcalls/platform/darwin/VideoCaptureView.h",
                      "tgcalls/platform/darwin/GLVideoView.mm",
                      "tgcalls/platform/darwin/GLVideoView.h",
                      "tgcalls/platform/darwin/VideoMetalView.mm",
                      "tgcalls/platform/darwin/VideoMetalView.h",
                      "tgcalls/platform/darwin/VideoSampleBufferView.mm",
                      "tgcalls/platform/darwin/VideoSampleBufferView.h",
                      "tgcalls/platform/darwin/AudioDeviceModuleIOS.h",
                      "tgcalls/platform/darwin/VideoCameraCapturer.mm",
                      "tgcalls/platform/darwin/VideoCameraCapturer.h",
                      "tgcalls/platform/darwin/CustomExternalCapturer.mm",
                      "tgcalls/platform/darwin/CustomExternalCapturer.h",
                     ],
            publicHeadersPath: "macos/PublicHeaders",
            cxxSettings: [
                .headerSearchPath("."),
                .headerSearchPath("tgcalls"),
                .headerSearchPath("macos/PublicHeaders"),
                .unsafeFlags(["-I../../core-xprojects/webrtc/build/src",
                              "-I../../core-xprojects/webrtc/build/src/third_party/abseil-cpp",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc/components/renderer/metal",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc/components/video_codec",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc/base",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc/api/video_codec",
                              "-I../../core-xprojects/webrtc/build/src/third_party/libyuv/include",
                              "-I../../core-xprojects/webrtc/build/src/sdk/objc/components/renderer/opengl",
                              "-I../../core-xprojects/openssl/build/openssl/include",
                              "-I../../core-xprojects/libopus/build/libopus/include",
                              "-I../../core-xprojects/ffmpeg/build/ffmpeg/include",
                              "-I../telegram-ios/third-party/rnnoise/PublicHeaders",
                              "-I../libtgvoip"]),
                
                .define("WEBRTC_POSIX", to: "1", nil),
                .define("WEBRTC_MAC", to: "1", nil),
                .define("NDEBUG", to: "1", nil),
                .define("RTC_ENABLE_VP9", to: "1", nil),
                .define("TGVOIP_NAMESPACE", to: "tgvoip_webrtc", nil),
            ]),
    ],
    cxxLanguageStandard: .cxx20
)
