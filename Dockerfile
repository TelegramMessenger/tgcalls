# syntax=docker/dockerfile:1
# Multi-stage build for tgcalls_cli Linux container
# Build: from the superproject's root, docker build -t tgcalls-test -f submodules/TgVoipWebrtc/tgcalls/Dockerfile .
# Run:   docker run tgcalls-test --mode reflector --reflector 91.108.13.2:598 --duration 10

# ============================================================
# Stage 1: Build
# ============================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# clang with libc++, as upstream WebRTC builds: WebRTC compiles against libc++ only (an unqualified isfinite from
# <cmath>, a pair<string, bool> map looked up with a pair<string_view, bool>). BAZEL_CXXOPTS and BAZEL_LINKLIBS set
# Bazel's own C++ toolchain on libc++; CC/CXX also reach the genrules' cmake/meson builds (.bazelrc passes both to
# actions), which build C only. gcc stays for clang's startup files and scripts that call it by name.
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc g++ clang-18 libc++-18-dev libc++abi-18-dev cmake meson ninja-build nasm make \
    autoconf automake libtool pkg-config python3 \
    unzip curl ca-certificates patch \
    zlib1g-dev libbz2-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CC=clang-18 CXX=clang++-18 BAZEL_CXXOPTS=-std=c++17:-stdlib=libc++ BAZEL_LINKLIBS=-lc++:-lc++abi:-lm

WORKDIR /src

# Copy source tree
COPY . .

# Download Bazel for the container (the host's copy is a macOS binary): the version versions.json pins for the repo (its
# .bazelrc uses that version's options), checked against the release's sha256. arm64 only: MODULE.bazel pins wasi-sdk,
# the compiler of the 19.0.0 wasm core the CLI embeds, for arm64 Linux alone.
ARG BAZEL_VERSION=9.2.0
ARG BAZEL_SHA256_ARM64=049dd21f40ad979db11c3ee68c96a42ce75f1185e69ac61ab20de1501427a410
RUN ARCH=$(uname -m) && \
    if [ "$ARCH" != "aarch64" ]; then \
      echo "Unsupported arch: $ARCH (arm64 only: MODULE.bazel pins wasi-sdk for arm64 Linux alone)" && exit 1; fi && \
    mkdir -p build-input/configuration-repository && \
    curl -fL "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-arm64" \
      -o build-input/bazel-linux && \
    echo "${BAZEL_SHA256_ARM64}  build-input/bazel-linux" | sha256sum -c - && \
    chmod +x build-input/bazel-linux

# MODULE.bazel names the app's build configuration module (build_configuration, written by Make.py, kept out of the
# context by Dockerfile.dockerignore); the CLI's build loads nothing from it, so an empty module stands in.
RUN printf 'module(\n    name = "build_configuration",\n)\n' > build-input/configuration-repository/MODULE.bazel && \
    touch build-input/configuration-repository/BUILD build-input/configuration-repository/WORKSPACE

# Build with persistent Bazel cache, one compile per 1.5 GB of the VM's memory: a compile of WebRTC's media files
# takes about a gigabyte, and a job per CPU thrashed an 8 GB VM (files of a minute took 15-18).
RUN --mount=type=cache,target=/root/.cache/bazel \
    JOBS=$(awk '/MemTotal/ { j = int($2 / 1572864); print (j < 1 ? 1 : j) }' /proc/meminfo) && \
    ./build-input/bazel-linux build //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli \
      --strategy=Genrule=standalone --spawn_strategy=standalone --jobs="$JOBS" && \
    cp bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli /tmp/tgcalls_cli

# ============================================================
# Stage 2: Runtime (minimal)
# ============================================================
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libc++1-18 libc++abi1-18 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/tgcalls_cli /usr/local/bin/tgcalls_cli

ENTRYPOINT ["tgcalls_cli"]
CMD ["--help"]
