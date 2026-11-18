#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/build.sh
# Purpose: Release build for Raspberry Pi 4 or Pi 5, on a fully 64-bit system or on a 64-bit
#          kernel with a 32-bit userland.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-16 - Added a model-aware Pi 4 / Pi 5 release build script.
#   2026-11-18 - Detects a 32-bit userland and builds inside a 64-bit build runtime. Now bash.
# Full history: PIPPIN.md and Git history.
# Usage: pippin/build.sh [BUILD_DIR]     (default: build)
#   Fully 64-bit system: builds directly.
#   64-bit kernel, 32-bit userland: creates a build runtime in build-runtime/ next to the
#   source the first time (Ubuntu Base 24.04.5 with the compiler and headers, about
#   1.1 GB; asks for sudo) and builds inside it. Set PIPPIN_BUILD_RUNTIME to keep it elsewhere.
# Set PIPPIN_CPU to override the -mcpu value (e.g. PIPPIN_CPU=cortex-a76), and
# PIPPIN_JOBS to limit parallel compile jobs (default: all cores).
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$src/build"}

[ "$(uname -m)" = aarch64 ] || {
  echo "The JIT needs a 64-bit ARM (aarch64) kernel; this is $(uname -m)." >&2
  echo "On a Raspberry Pi 4, add 'arm_64bit=1' to /boot/config.txt (or /boot/firmware/config.txt) and reboot." >&2
  exit 1
}

# 64-bit kernel with a 32-bit userland: run this same script inside a 64-bit build runtime,
# where the userland is 64-bit and it takes the normal path below.
if [ ! -e /lib/ld-linux-aarch64.so.1 ]; then
  echo "This system has a 32-bit userland; building inside a 64-bit build runtime."
  mkdir -p "$out"; out=$(cd "$out" && pwd)
  rt=${PIPPIN_BUILD_RUNTIME:-$src/build-runtime}
  sudo "$src/pippin/runtime/make-runtime.sh" --dev "$rt"
  pass=()
  for name in PIPPIN_CPU PIPPIN_JOBS; do
    if [ -n "${!name:-}" ]; then pass+=(--env "$name=${!name}"); fi
  done
  exec sudo "$src/pippin/runtime/enter.sh" "$rt" --user --bind "$src:/src" --bind "$out:/out" \
    "${pass[@]}" -- /src/pippin/build.sh /out
fi

model=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)
case "$model" in
  *"Raspberry Pi 5"*) cpu=cortex-a76 ;;
  *"Raspberry Pi 4"*) cpu=cortex-a72 ;;
  *)                  cpu=native ;;
esac
cpu=${PIPPIN_CPU:-$cpu}

page=$(getconf PAGESIZE)
if [ "$page" != 4096 ]; then
  echo "Note: this kernel uses ${page}-byte pages. Fastmem needs 4 KB pages and will switch itself off."
  echo "      On a Pi 5, add 'kernel=kernel8.img' to /boot/firmware/config.txt and reboot to get 4 KB pages."
fi

# Single-partition LTO: the CPU loop's inline assembly relies on symbol offsets that a
# partitioned LTO build breaks. Do not add PGO; it moves globals the CPU loop expects.
flags="-mcpu=$cpu -flto=2 -flto-partition=one -fno-fat-lto-objects"
echo "Building for: $model (-mcpu=$cpu)"

mkdir -p "$out"
cd "$out"
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
      -DCMAKE_EXE_LINKER_FLAGS="$flags -Wl,-u,pippin_cpu_owner_guest_time_ns" \
      -DCMAKE_AR="$(command -v gcc-ar)" -DCMAKE_RANLIB="$(command -v gcc-ranlib)" \
      "$src"
cmake --build . --parallel "${PIPPIN_JOBS:-$(nproc)}"
echo "Built $out/bin/dingusppc"
