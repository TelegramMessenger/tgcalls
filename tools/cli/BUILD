load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# Time-coded speaker tones and their detector (the broadcast testbench).
cc_library(
    name = "broadcast_signals",
    srcs = ["broadcast_signals.cpp"],
    hdrs = ["broadcast_signals.h"],
    copts = ["-std=c++17"],
    # Used by the new player's runner (telegram-ios Tests/BroadcastRunner), as are the libraries below marked public.
    visibility = ["//visibility:public"],
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

# What the broadcast testbench measures from a listener's playout, frames and levels.
cc_library(
    name = "broadcast_metrics",
    srcs = ["broadcast_metrics.cpp"],
    hdrs = ["broadcast_metrics.h"],
    copts = TGCALLS_CLI_COPTS,
    deps = [
        ":broadcast_signals",
        "//submodules/TgVoipWebrtc:tgcalls_core",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "broadcast_metrics_test",
    srcs = ["broadcast_metrics_test.cpp"],
    copts = TGCALLS_CLI_COPTS,
    linkopts = TGCALLS_CLI_LINKOPTS,
    deps = [
        ":broadcast_metrics",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
    ],
)

# The broadcast report (schema 1): broadcast mode's, and the new player's runner's (telegram-ios Tests/BroadcastRunner).
cc_library(
    name = "broadcast_report",
    srcs = ["broadcast_report.cpp"],
    hdrs = ["broadcast_report.h"],
    copts = TGCALLS_CLI_COPTS,
    deps = [":broadcast_metrics"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "broadcast_report_writer_test",
    srcs = ["broadcast_report_test.cpp"],
    copts = TGCALLS_CLI_COPTS,
    linkopts = TGCALLS_CLI_LINKOPTS,
    deps = [
        ":broadcast_report",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
    ],
)

# Group participants on the Go SFU (CustomImpl and ReferenceImpl), also the broadcast speakers.
cc_library(
    name = "group_participant",
    srcs = [
        "group_participant.cpp",
        "fake_video_source.cpp",
    ],
    hdrs = [
        "group_participant.h",
        "fake_video_source.h",
        "fake_video_sink.h",
    ],
    copts = TGCALLS_CLI_COPTS,
    deps = [
        ":broadcast_signals",
        "//submodules/TgVoipWebrtc:tgcalls_core",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
    ],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "tgcalls_cli",
    srcs = [
        "main.cpp",
        "group_mode.cpp",
        "group_mode.h",
        "group_churn_mode.cpp",
        "group_churn_mode.h",
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
        ":broadcast_signals",
        ":group_participant",
        "//submodules/TgVoipWebrtc:tgcalls_core",
        "//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu",
    ],
    data = [
        "//submodules/TgVoipWebrtc:reference_core_wasm",
        "//submodules/TgVoipWebrtc:variant_core_wasm",
    ],
)
