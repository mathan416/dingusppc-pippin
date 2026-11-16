#!/bin/bash
set -euo pipefail
nice -n 15 g++ -std=c++17 -O2 -Wall -Wextra -I/work/dingusppc $(sdl2-config --cflags) /work/test-boundaries.cpp -o /session/test-boundaries
/session/test-boundaries | tee /session/boundaries.log
nice -n 15 g++ -std=c++17 -O2 -Wall -Wextra -I/work/dingusppc $(sdl2-config --cflags) /work/test-timing-state.cpp -o /session/test-timing-state
/session/test-timing-state | tee /session/timing-state.log
cc -Wall -Wextra -shared -fPIC /work/gameplay-hook.c -o /session/gameplay-hook.so $(sdl2-config --cflags --libs) -ldl
nice -n 15 g++ -std=c++17 -O2 -Wall -Wextra -I/work/dingusppc /work/test-fragments-pointer.cpp -o /session/test-fragments-pointer
/session/test-fragments-pointer | tee /session/fragments-pointer.log
nice -n 15 ionice -c2 -n7 cmake --build /work/build --parallel 2
cp /work/build/bin/dingusppc /session/sheep-pippin-v48-audio-input-v3
sha256sum /session/sheep-pippin-v48-audio-input-v3
readelf -h /session/sheep-pippin-v48-audio-input-v3 | head -20
echo COMPLETE
