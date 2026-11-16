# Testing a build

How to check that a build of DingusPPC-Pippin is correct, from a quick smoke test to the
checks every release goes through. All of these run on a 64-bit ARM Linux machine (a
Raspberry Pi 4 or 5, or an ARM64 server); the JIT doesn't run on x86.

## Contents

- [Quick check: the JIT self-tests](#quick-check-the-jit-self-tests)
- [Unit tests](#unit-tests)
- [Verify mode: the JIT against the interpreter](#verify-mode-the-jit-against-the-interpreter)
- [Speed](#speed)
- [What CI runs](#what-ci-runs)
- [What a release goes through](#what-a-release-goes-through)

## Quick check: the JIT self-tests

```
truncate -s 4M zero.rom
build/bin/dingusppc --jit-selftest -r -m pippin -b zero.rom --rambank3_size=8 \
  --log-to-stderr --log-verbosity=0 2>&1 | grep "PIPPIN JIT SELFTEST"
```

This builds the Pippin machine, runs the JIT's start-up self-tests and exits, printing
`PIPPIN JIT SELFTEST PASS` (exit status 0) or `FAIL` (exit status 1). No guest code runs, so
the ROM can be any 4 MB file; a file of zeros is fine. On a computer without a display, add
`SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`.

The self-tests compile and run small fixtures that cover:

- translated loads and stores, including fastmem faults and halfword edge cases
- every branch form and the condition register
- the ROM dispatcher, ROM window and decode limits
- literal islands and block layout
- the guest clock and instruction-count bookkeeping
- the authentic-speed CD timing model

The same tests run every time the emulator starts with the JIT on. If one fails, the
emulator stops instead of running.

## Unit tests

A few pieces (timing state, frame boundaries, pointer input) have stand-alone unit tests in
`pippin/support/`. Build and run them with CTest:

```
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDPPC_BUILD_PIPPIN_TESTS=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

## Verify mode: the JIT against the interpreter

The strongest check. With `SHEEP_PIPPIN_JIT=verify`, every translated block is also run
through DingusPPC's interpreter, and the two results (registers and memory) must match
exactly. It runs real games, so it tests what the self-tests can't: the instruction mixes
real software produces.

```
SHEEP_PIPPIN_JIT=verify SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 \
SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off \
SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on DINGUS_AUDIO_GUEST_SYNC=1 \
build/bin/dingusppc -r --realtime --start-date=1997-06-01 -m pippin -b kinka13.rom \
  --cdr_config=ScsiMesh:3 --cdr_img2=disc.bin --rambank3_size=8 \
  --adb_devices=AppleJack,Keyboard --log-to-stderr 2>&1 | grep "SHEEP-PIPPIN JIT"
```

That's the launcher's command with `verify` instead of `on`. Let the game run for a minute
or two, then quit (Ctrl+C). At exit it prints a summary such as:

```
SHEEP-PIPPIN JIT mode=1 blocks=277591251 instructions=1747562851 verified=1747562851 mismatches=0 ...
```

`verified` should equal `instructions`, and `mismatches` must be 0. On a mismatch the
emulator stops at once and logs the guest instructions and both sets of register values.
Verify mode is several times slower than normal, so games run below full speed; that's
expected.

## Speed

The emulator prints its counters at exit. The most useful number is guest instructions per
second during steady play. In a normal run (`SHEEP_PIPPIN_JIT=on`) the summary line
reports `steady_insns`, the instructions retired by the fast inner loop: divide by the
seconds the game ran. For reference, the L-Zone intro on v1.0.0:

| Machine | Million guest instructions per second |
| --- | --- |
| Real Pippin (603 at 66 MHz) | about 26 |
| Raspberry Pi 4, 1.5 GHz | about 41 |
| Raspberry Pi 500 (Pi 5 chip, 4 KB-page kernel) | about 139 |

Speed varies between scenes and titles. Compare builds on the same scene, several times,
with nothing else running.

## What CI runs

Every push to the `pippin` branch runs `.github/workflows/pippin.yml` on a GitHub ARM64
runner (Ubuntu 24.04). It:

1. builds a Release binary with the unit tests enabled,
2. runs the unit tests with CTest,
3. runs `--jit-selftest` against a 4 MB file of zeros and checks for `PASS`.

CI can't run games (there's no ROM or disc), so it doesn't do verify mode or speed tests.

## What a release goes through

Before v1.0.0 was published, each release binary was:

1. built with `pippin/build.sh` on Raspberry Pi OS Bookworm (`-mcpu=cortex-a72` for the
   Pi 4 build, `cortex-a76` for the Pi 5 build);
2. self-tested and unit-tested as above;
3. run in verify mode on L-Zone: 479 million instructions on a Pi 4 and 1.75 billion on a
   Pi 500, with no mismatches;
4. speed-tested on L-Zone against the previous build, alternating the two, three runs each;
5. run on L-Zone and Tamagotchi in normal mode and on L-Zone in authentic-speed mode,
   checking for clean shutdown, no audio drops and the expected mode line;
6. checked with `run.sh` exactly as a user would run it;
7. and the source archive was unpacked and built from scratch to confirm nothing is
   missing.
