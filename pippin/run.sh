#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/run.sh
# Purpose: Run a Pippin disc with the tested JIT settings.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-17 - v1.0.0: launcher with the settings every release build was verified with.
#   2026-11-17 - Add DINGUS_AUDIO_GUEST_SYNC=1 (audio paced to the game, as on every tested setup).
#   2026-11-17 - Pass absolute ROM and disc paths, so relative paths work together with -w.
#   2026-11-18 - CUE/CHD discs, per-game save folders (PIPPIN_SAVES), and the 64-bit runtime
#                on systems with a 32-bit userland. Now a bash script.
# Full history: PIPPIN.md and Git history.
# Usage: run.sh ROM DISC [extra dingusppc options]
#   ROM   Pippin boot ROM (tested with Kinka 1.3, kinka13.rom)
#   DISC  the game: a .cue sheet (with its .bin files), a .chd, or a raw .bin/.iso data track
# Set PIPPIN_AUTHENTIC=on to cap the CPU at the real Pippin's speed (26 MIPS).
# Set PIPPIN_SAVES to a folder to keep each game's flash and NVRAM in its own subfolder
#   (otherwise they go in the current folder, or the folder given with -w).
# On a 64-bit kernel with a 32-bit userland, the emulator runs inside the runtime made by
#   runtime/make-runtime.sh (runtime/root next to this script, or PIPPIN_RUNTIME); needs sudo.
# Set DINGUSPPC to the binary's path if it is not next to this script or in build/bin.
set -euo pipefail

die() { echo "$*" >&2; exit 1; }
[ $# -ge 2 ] || { sed -n '/^# Usage/,/^# Set DINGUSPPC/s/^# \{0,1\}//p' "$0" >&2; exit 2; }
rom=$1; disc=$2; shift 2
[ -f "$rom" ] || die "ROM not found: $rom"
[ -f "$disc" ] || die "Disc image not found: $disc"
case $rom in /*) ;; *) rom=$(pwd)/$rom ;; esac

here=$(cd "$(dirname "$0")" && pwd)
command -v python3 >/dev/null || die "run.sh needs python3 to read the disc"
prep=$(python3 "$here/prepare-disc.py" "$disc") || exit 1
id=${prep%%$'\t'*}; image=${prep#*$'\t'}
[ -w "$image" ] || die "Disc image is read-only: $image (DingusPPC opens it read/write; run chmod u+w on it)"

bin=${DINGUSPPC:-}
if [ -z "$bin" ]; then
  for candidate in "$here/dingusppc" "$here/../build/bin/dingusppc"; do
    if [ -x "$candidate" ]; then bin=$candidate; break; fi
  done
fi
[ -n "$bin" ] && [ -x "$bin" ] || die "dingusppc binary not found; build it with pippin/build.sh or set DINGUSPPC"

# Save folder: -w as given, else PIPPIN_SAVES/<disc id>, else the current folder.
workdir=; extra=()
while [ $# -gt 0 ]; do
  case $1 in
    -w|--workingdir) [ $# -ge 2 ] || die "$1 needs a folder"; workdir=$2; shift 2 ;;
    -w=*|--workingdir=*) workdir=${1#*=}; shift ;;
    *) extra+=("$1"); shift ;;
  esac
done
if [ -z "$workdir" ] && [ -n "${PIPPIN_SAVES:-}" ]; then workdir=$PIPPIN_SAVES/$id; fi
if [ -n "$workdir" ]; then mkdir -p "$workdir"; workdir=$(cd "$workdir" && pwd); fi

settings=(SHEEP_PIPPIN_JIT=on SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1
  SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off
  SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on mesa_glthread=false
  DINGUS_AUDIO_GUEST_SYNC=1)
if [ "${PIPPIN_AUTHENTIC:-off}" = on ]; then
  # CPU cap only; the CD timing model stays off (it can stall the L-Zone intro, see PIPPIN.md).
  settings+=(SHEEP_PIPPIN_AUTHENTIC=on SHEEP_PIPPIN_CD_SPEED=off)
fi
options=(-r --realtime --start-date=1997-06-01 -m pippin --cdr_config=ScsiMesh:3
  --rambank3_size=8 --adb_devices=AppleJack,Keyboard)

# A fully 64-bit system runs the emulator directly.
if [ -e /lib/ld-linux-aarch64.so.1 ]; then
  exec env "${settings[@]}" "$bin" "${options[@]}" -b "$rom" --cdr_img2="$image" \
    ${workdir:+-w "$workdir"} "${extra[@]}"
fi

# 32-bit userland: run inside the 64-bit runtime.
[ "$(uname -m)" = aarch64 ] || die "This system runs a $(uname -m) kernel; DingusPPC-Pippin needs a 64-bit (aarch64) kernel.
On a Raspberry Pi 4, add 'arm_64bit=1' to /boot/config.txt and reboot."
runtime=${PIPPIN_RUNTIME:-$here/runtime/root}
[ -f "$runtime/.dingusppc-pippin-runtime" ] || die "This system has a 32-bit userland, so DingusPPC-Pippin runs inside a 64-bit runtime.
Create it once with:  sudo $here/runtime/make-runtime.sh $runtime"
enter=("$here/runtime/enter.sh" "$runtime" --user
  --ro "$bin:/tmp/pippin/dingusppc" --ro "$rom:/tmp/pippin/rom" --bind "$image:/tmp/pippin/disc.bin")
[ -n "$workdir" ] && enter+=(--bind "$workdir:/tmp/pippin/saves")
[ -f "$HOME/.asoundrc" ] && enter+=(--ro "$HOME/.asoundrc:/tmp/pippin/home/.asoundrc")
uid=$(id -u)
[ -d "/run/user/$uid" ] && enter+=(--bind "/run/user/$uid:/run/user/$uid")
[ -d /tmp/.X11-unix ] && [ -n "${DISPLAY:-}" ] && enter+=(--ro "/tmp/.X11-unix:/tmp/.X11-unix")
enter+=(--env HOME=/tmp/pippin/home)
# The caller's display, audio and emulator variables, then the launcher's settings (which
# win, as they do when running directly).
while IFS= read -r line; do
  enter+=(--env "$line")
done < <(env | grep -E '^(SDL_|SHEEP_|DINGUS_|PIPPIN_|MESA_|LIBGL_|ALSA_|PULSE_|DISPLAY=|WAYLAND_DISPLAY=|XDG_RUNTIME_DIR=)' || true)
for setting in "${settings[@]}"; do enter+=(--env "$setting"); done
exec sudo "${enter[@]}" -- /tmp/pippin/dingusppc "${options[@]}" -b /tmp/pippin/rom \
  --cdr_img2=/tmp/pippin/disc.bin ${workdir:+-w /tmp/pippin/saves} "${extra[@]}"
