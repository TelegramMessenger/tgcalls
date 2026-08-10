#!/usr/bin/env python3
"""Build tgcalls (libtgcall.so) for all 3 OHOS architectures.

The build reuses the prebuilt WebRTC artifacts (headers + gen +
libohos_webrtc.so) and links against libohos_webrtc.so dynamically.
The WebRTC source tree is never modified.
"""
import os
import shutil
import subprocess
import sys
import time

TGCALLS = "/home/yuanchu/webrtc-harmony-builder/tgcalls"
WEBRTC = "/home/yuanchu/webrtc-harmony-builder/webrtc_latest/src"
GN = os.path.join(WEBRTC, "buildtools/linux64/gn")
NINJA = "/home/yuanchu/depot_tools/ninja"
SDK_PATH = "/home/yuanchu/command-line-tools/sdk/default/openharmony/native"
JOBS = 8

ARCH_CONFIGS = [
    {"name": "arm64", "target_cpu": "arm64", "extra_args": "", "desc": "ARM64 (aarch64)"},
    {"name": "arm", "target_cpu": "arm", "extra_args": "", "desc": "ARM32"},
    {"name": "x86_64", "target_cpu": "x86_64", "extra_args": 'current_cpu="x64"', "desc": "x86_64"},
]


def run_cmd(cmd, cwd=None, desc=""):
    print(f"\n{'='*60}")
    print(f"[{desc}] Running: {' '.join(cmd[:3])}...")
    print(f"{'='*60}")
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.stdout:
        lines = result.stdout.strip().split("\n")
        if len(lines) > 20:
            print("\n".join(lines[:10]))
            print(f"... ({len(lines)-20} lines omitted)")
            print("\n".join(lines[-10:]))
        else:
            print(result.stdout)
    if result.returncode != 0:
        print(f"ERROR (rc={result.returncode}):")
        if result.stderr:
            print(result.stderr[-3000:])
        return False
    if result.stderr:
        print(result.stderr[-500:])
    return True


if __name__ == "__main__":
    only = sys.argv[1] if len(sys.argv) > 1 else None
    start_time = time.time()

    for arch in ARCH_CONFIGS:
        if only and arch["name"] != only:
            continue
        arch_start = time.time()
        out_dir = os.path.join(TGCALLS, f"out/{arch['name']}")

        print(f"\n{'#'*60}")
        print(f"# Building {arch['desc']} (target_cpu={arch['target_cpu']})")
        print(f"{'#'*60}")

        if os.path.exists(out_dir):
            shutil.rmtree(out_dir)

        args = (f'target_os="ohos" target_cpu="{arch["target_cpu"]}" '
                f'is_clang=true is_component_build=false '
                f'ohos_sdk_native_root="{SDK_PATH}"')
        if arch["extra_args"]:
            args += " " + arch["extra_args"]

        print(f"GN args: {args}")
        if not run_cmd([GN, "gen", out_dir, f"--args={args}"], TGCALLS, f"GN gen {arch['desc']}"):
            print(f"FAILED: gn gen for {arch['desc']}")
            sys.exit(1)

        if not run_cmd([NINJA, "-C", out_dir, "-j", str(JOBS), "libtgcall"], TGCALLS, f"Ninja build {arch['desc']}"):
            print(f"FAILED: ninja build for {arch['desc']}")
            sys.exit(1)

        so = os.path.join(out_dir, "libtgcall.so")
        if os.path.exists(so):
            size = os.path.getsize(so)
            print(f"\n>>> {arch['desc']} OK: {so} ({size/1024/1024:.1f}MB)")
        else:
            print(f"\n>>> WARNING: {so} not found")
            sys.exit(1)

        elapsed = time.time() - arch_start
        print(f">>> {arch['desc']} completed in {elapsed:.0f}s")

    total_elapsed = time.time() - start_time
    print(f"\n{'#'*60}")
    print(f"# ALL ARCHITECTURES COMPLETED in {total_elapsed:.0f}s")
    print(f"{'#'*60}")
