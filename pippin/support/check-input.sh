#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/support/check-input.sh
# Purpose: Check controller input handling.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
# Full history: PIPPIN.md and Git history.
set -euo pipefail
cc -Wall -Wextra /work/controller/test-session.c -o /session/test-session $(sdl2-config --cflags --libs)
cc -Wall -Wextra /work/controller/test-router.c -o /session/test-router $(sdl2-config --cflags --libs)
for mode in f4 combo;do env SDL_VIDEODRIVER=dummy DINGUS_CABINET_AUTOSTART=0 LD_PRELOAD=/work/controller/session.so /session/test-session "$mode";echo PASS-exit-$mode;done
env SDL_VIDEODRIVER=dummy DINGUS_CABINET_ROUTER=1 DINGUS_CABINET_AUTOSTART=0 LD_PRELOAD=/work/controller/session.so /session/test-router
