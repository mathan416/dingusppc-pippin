#!/bin/sh
# Project: DingusPPC-Pippin
# File: pippin/build.sh
# Purpose: Release build for Raspberry Pi 4 or Pi 5.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-16 - Added a model-aware Pi 4 / Pi 5 release build script.
# Full history: PIPPIN.md and Git history.
# Usage: pippin/build.sh [BUILD_DIR]     (default: build)
# Set PIPPIN_CPU to override the -mcpu value (e.g. PIPPIN_CPU=cortex-a76), and
# PIPPIN_JOBS to limit parallel compile jobs (default: all cores).
set -eu

src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$src/build"}

[ "$(uname -m)" = aarch64 ] || { echo "The JIT needs a 64-bit ARM (aarch64) host; this is $(uname -m)." >&2; exit 1; }

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
