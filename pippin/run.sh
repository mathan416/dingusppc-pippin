#!/bin/sh
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
# Full history: PIPPIN.md and Git history.
# Usage: run.sh ROM DISC [extra dingusppc options]
#   ROM   Pippin boot ROM (tested with Kinka 1.3, kinka13.rom)
#   DISC  the game's data track as a raw image
# Set PIPPIN_AUTHENTIC=on to cap the CPU at the real Pippin's speed (26 MIPS).
# Set DINGUSPPC to the binary's path if it is not next to this script or in build/bin.
set -eu

[ $# -ge 2 ] || { sed -n '/^# Usage/,/^# Set DINGUSPPC/s/^# \{0,1\}//p' "$0" >&2; exit 2; }
rom=$1; disc=$2; shift 2
[ -f "$rom" ] || { echo "ROM not found: $rom" >&2; exit 1; }
[ -f "$disc" ] || { echo "Disc image not found: $disc" >&2; exit 1; }
[ -w "$disc" ] || { echo "Disc image is read-only: $disc (DingusPPC opens it read/write; run chmod u+w on it)" >&2; exit 1; }
# -w changes DingusPPC's working directory before it opens these, so pass absolute paths.
case $rom in /*) ;; *) rom=$(pwd)/$rom ;; esac
case $disc in /*) ;; *) disc=$(pwd)/$disc ;; esac

here=$(cd "$(dirname "$0")" && pwd)
bin=${DINGUSPPC:-}
if [ -z "$bin" ]; then
  for candidate in "$here/dingusppc" "$here/../build/bin/dingusppc"; do
    if [ -x "$candidate" ]; then bin=$candidate; break; fi
  done
fi
[ -n "$bin" ] && [ -x "$bin" ] || { echo "dingusppc binary not found; build it with pippin/build.sh or set DINGUSPPC" >&2; exit 1; }

authentic=
if [ "${PIPPIN_AUTHENTIC:-off}" = on ]; then
  # CPU cap only; the CD timing model stays off (it can stall the L-Zone intro, see PIPPIN.md).
  authentic="SHEEP_PIPPIN_AUTHENTIC=on SHEEP_PIPPIN_CD_SPEED=off"
fi

# shellcheck disable=SC2086
exec env SHEEP_PIPPIN_JIT=on SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 \
  SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off \
  SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on mesa_glthread=false \
  DINGUS_AUDIO_GUEST_SYNC=1 $authentic \
  "$bin" -r --realtime --start-date=1997-06-01 -m pippin -b "$rom" \
  --cdr_config=ScsiMesh:3 --cdr_img2="$disc" --rambank3_size=8 \
  --adb_devices=AppleJack,Keyboard "$@"
