load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Time-coded speaker tones and their detector (the broadcast testbench).
cc_library(
    name = "broadcast_signals",
    srcs = ["broadcast_signals.cpp"],
    hdrs = ["broadcast_signals.h"],
    copts = ["-std=c++17"],
)

cc_test(
    name = "broadcast_signals_test",
    srcs = ["broadcast_signals_test.cpp"],
    copts = ["-std=c++17"],
    deps = [
        ":broadcast_signals",
        "//third-party/opus:opus",
    ],
)

TGCALLS_CLI_COPTS = [
    "-I{}/tgcalls/tgcalls".format("submodules/TgVoipWebrtc"),
    "-Ithird-party/webrtc/webrtc",
    "-Ithird-party/webrtc/dependencies",
    "-Ithird-party/webrtc/absl",
    "-Ithird-party/libyuv",
    "-DRTC_ENABLE_VP9",
    "-DNDEBUG",
    "-std=c++17",
    "-w",
] + select({
    "@platforms//os:linux": ["-DWEBRTC_LINUX", "-DWEBRTC_POSIX"],
    "//conditions:default": ["-DWEBRTC_MAC", "-DWEBRTC_POSIX"],
})

TGCALLS_CLI_LINKOPTS = select({
    "@platforms//os:linux": ["-lpthread", "-lm", "-ldl"],
    "//conditions:default": ["-framework", "CoreFoundation", "-framework", "Security"],
})

# The emulated broadcast server's parts, read by the decoders today's engine uses.
cc_test(
    name = "broadcast_part_format_test",
    srcs = ["broadcast_part_format_test.cpp"],
    copts = TGCALLS_CLI_COPTS,
    linkopts = TGCALLS_CLI_LINKOPTS,
    data = ["//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu:broadcast_fixtures"],
    deps = [
        ":broadcast_signals",
        "//submodules/TgVoipWebrtc:tgcalls_core",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
        "//third-party/opus:opus",
    ],
)

cc_binary(
    name = "tgcalls_cli",
    srcs = [
        "main.cpp",
        "group_mode.cpp",
        "group_mode.h",
        "group_participant.cpp",
        "group_participant.h",
        "group_churn_mode.cpp",
        "group_churn_mode.h",
        "fake_video_source.h",
        "fake_video_source.cpp",
        "fake_video_sink.h",
    ],
    copts = [
        "-I{}/tgcalls/tgcalls".format("submodules/TgVoipWebrtc"),
        "-Ithird-party/webrtc/webrtc",
        "-Ithird-party/webrtc/dependencies",
        "-Ithird-party/webrtc/absl",
        "-Ithird-party/libyuv",
        "-DRTC_ENABLE_VP9",
        "-DNDEBUG",
        "-std=c++17",
        "-w",
    ] + select({
        "@platforms//os:linux": ["-DWEBRTC_LINUX", "-DWEBRTC_POSIX"],
        "//conditions:default": ["-DWEBRTC_MAC", "-DWEBRTC_POSIX"],
    }),
    linkopts = select({
        "@platforms//os:linux": ["-lpthread", "-lm", "-ldl"],
        "//conditions:default": ["-framework", "CoreFoundation", "-framework", "Security"],
    }),
    deps = [
        "//submodules/TgVoipWebrtc:tgcalls_core",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
    ],
    data = [
        "//submodules/TgVoipWebrtc:reference_core_wasm",
        "//submodules/TgVoipWebrtc:variant_core_wasm",
    ],
)
