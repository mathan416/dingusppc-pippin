#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/runtime/make-runtime.sh
# Purpose: Create the 64-bit runtime for systems with a 64-bit kernel but a 32-bit userland
#          (for example the official RetroPie image on a Raspberry Pi 4).
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-18 - Added: Ubuntu Base 24.04 ARM64 runtime, pinned by SHA-256.
# Full history: PIPPIN.md and Git history.
#
# Usage: sudo make-runtime.sh [--dev] DIR
#   Downloads Canonical's Ubuntu Base 24.04.5 ARM64 image (about 30 MB), checks it against a
#   pinned SHA-256, unpacks it into DIR and installs the libraries DingusPPC-Pippin needs
#   (SDL2, Mesa, ALSA, PulseAudio; about 650 MB in total). --dev also installs the compiler
#   and headers needed to build it (about 1.1 GB in total; the two package sets overlap).
#   Running it again on an existing runtime updates and adds packages without downloading
#   the image again.
#   Nothing is installed on the host system itself.
set -euo pipefail

URL=https://cdimage.ubuntu.com/ubuntu-base/releases/24.04.5/release/ubuntu-base-24.04.5-base-arm64.tar.gz
SHA256=a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2
# SDL loads the display and audio libraries at run time, so they are listed explicitly:
# libegl1/libgles2 (glvnd loaders) with Mesa's EGL/GL drivers for KMS/DRM, plus ALSA and PulseAudio.
RUN_PACKAGES=(libsdl2-2.0-0 libegl1 libgles2 libgl1 libegl-mesa0 libglx-mesa0 libgl1-mesa-dri libgbm1
  libasound2t64 libasound2-plugins libpulse0)
DEV_PACKAGES=(build-essential cmake pkg-config libsdl2-dev libasound2-dev libpulse-dev)

die() { echo "make-runtime.sh: $*" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
dev=0
if [[ ${1:-} == --dev ]]; then dev=1; shift; fi
[[ $# -eq 1 ]] || die "usage: sudo make-runtime.sh [--dev] DIR"
dir=$1
[[ $EUID == 0 ]] || die "run with sudo"
if [[ $(uname -m) != aarch64 ]]; then
  die "this system runs a $(uname -m) kernel. The runtime needs a 64-bit kernel: on a Raspberry Pi 4,
add the line 'arm_64bit=1' to /boot/config.txt (or /boot/firmware/config.txt) and reboot."
fi

if [[ ! -f $dir/.dingusppc-pippin-runtime ]]; then
  if [[ -e $dir ]] && [[ -n $(ls -A "$dir") ]]; then die "$dir exists and is not empty"; fi
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  echo "Downloading Ubuntu Base 24.04.5 ARM64..."
  if command -v curl >/dev/null; then curl -fL --retry 3 -o "$tmp/base.tar.gz" "$URL"
  else wget -q --tries=3 -O "$tmp/base.tar.gz" "$URL"; fi
  echo "$SHA256  $tmp/base.tar.gz" | sha256sum -c --quiet - || die "checksum mismatch; download discarded"
  mkdir -p "$dir"
  tar --numeric-owner -xpf "$tmp/base.tar.gz" -C "$dir"
  # Package installs must not start services through the host's kernel.
  printf '#!/bin/sh\nexit 101\n' > "$dir/usr/sbin/policy-rc.d"; chmod 755 "$dir/usr/sbin/policy-rc.d"
  echo "ubuntu-base-24.04.5-arm64" > "$dir/.dingusppc-pippin-runtime"
fi

packages=("${RUN_PACKAGES[@]}")
((dev)) && packages+=("${DEV_PACKAGES[@]}")
echo "Installing into the runtime: ${packages[*]}"
"$here/enter.sh" "$dir" --network --env DEBIAN_FRONTEND=noninteractive -- /bin/sh -ec \
  "apt-get update -q && apt-get install -y -q --no-install-recommends $(printf '%q ' "${packages[@]}") && apt-get clean"
echo "Runtime ready: $dir ($(du -sh "$dir" | cut -f1))"
