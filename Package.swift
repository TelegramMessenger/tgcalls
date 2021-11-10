// swift-tools-version:5.5
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "tgcalls",
    platforms: [.macOS(.v10_11)],
    products: [
        .library(
            name: "tgcalls",
            targets: ["tgcalls"]),
    ],
    dependencies: [
    ],
    targets: [
        .target(
            name: "tgcalls",
            dependencies: [],
            path: "tgcalls",
            exclude: ["platform/android",
                      "platform/tdesktop",
                      "platform/uwp",
                      "platform/fake",
                      "platform/darwin/VideoCaptureView.mm",
                      "platform/darwin/VideoCaptureView.h",
                      "platform/darwin/GLVideoView.mm",
                      "platform/darwin/GLVideoView.h",
                      "platform/darwin/VideoMetalView.mm",
                      "platform/darwin/VideoMetalView.h",
                      "platform/darwin/VideoSampleBufferView.mm",
                      "platform/darwin/VideoSampleBufferView.h",
                      "platform/darwin/AudioDeviceModuleIOS.h",
                      "platform/darwin/VideoCameraCapturer.mm",
                      "platform/darwin/VideoCameraCapturer.h",
                      "platform/darwin/CustomExternalCapturer.mm",
                      "platform/darwin/CustomExternalCapturer.h",
                     ],
            publicHeadersPath: ".",
            cxxSettings: [
                .headerSearchPath(".."),
                .headerSearchPath("."),
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
                .define("RTC_ENABLE_VP9", to: "1", nil),
                .define("TGVOIP_NAMESPACE", to: "tgvoip_webrtc", nil),
            ]),
    ],
    cxxLanguageStandard: .cxx17
)
