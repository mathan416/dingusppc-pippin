#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/support/check-pointer.sh
# Purpose: Check pointer input handling.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
# Full history: PIPPIN.md and Git history.
set -euo pipefail
cc -Wall -Wextra /work/controller/test-pointer.c -o /session/test-pointer $(sdl2-config --cflags --libs)
env SDL_VIDEODRIVER=dummy DINGUS_CABINET_ROUTER=1 DINGUS_CABINET_AUTOSTART=0 LD_PRELOAD=/work/controller/session.so /session/test-pointer
