#!/bin/bash
set -euo pipefail
cc -Wall -Wextra /work/controller/test-pointer.c -o /session/test-pointer $(sdl2-config --cflags --libs)
env SDL_VIDEODRIVER=dummy DINGUS_CABINET_ROUTER=1 DINGUS_CABINET_AUTOSTART=0 LD_PRELOAD=/work/controller/session.so /session/test-pointer
