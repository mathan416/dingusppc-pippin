#!/bin/sh
# Project: DingusPPC-Pippin
# File: pippin/package.sh
# Purpose: Collect a built emulator into a self-contained folder (the shape of a release).
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-18 - Added: one package for standalone use and for the RetroPie installer.
# Full history: PIPPIN.md and Git history.
#
# Usage: pippin/package.sh [BUILD_DIR] [OUT_DIR]   (defaults: build, dist)
#   Creates OUT_DIR/dingusppc-pippin/ with the emulator, run.sh, the disc helper, the
#   runtime scripts, the RetroPie installer, the licences and the documentation.
#   The 64-bit runtime itself is never packaged: run.sh and the installer create it on
#   systems that need it.
set -eu

src=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$src/build"}
out=${2:-"$src/dist"}
bin="$build/bin/dingusppc"
[ -x "$bin" ] || { echo "No emulator at $bin; build it first with pippin/build.sh" >&2; exit 1; }

dir="$out/dingusppc-pippin"
rm -rf "$dir"
mkdir -p "$dir/runtime" "$dir/retropie"
install -m 755 "$bin" "$dir/dingusppc"
install -m 755 "$src/pippin/run.sh" "$src/pippin/prepare-disc.py" "$dir/"
install -m 755 "$src/pippin/runtime/make-runtime.sh" "$src/pippin/runtime/enter.sh" "$dir/runtime/"
install -m 755 "$src/pippin/retropie/install.sh" "$dir/retropie/"
cp "$src/README.md" "$src/PIPPIN.md" "$src/LICENSE" "$src/CREDITS.md" "$dir/"
cp -R "$src/docs" "$dir/docs"
version=$(git -C "$src" describe --tags --always --dirty 2>/dev/null || echo unknown)
printf 'version=%s\nbuilt=%s\n' "$version" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$dir/VERSION"
echo "Packaged $dir ($version)"
