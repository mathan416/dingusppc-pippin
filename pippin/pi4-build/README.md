# Building for Raspberry Pi 4 (Cortex-A72)

Deployed builds use these flags:

    -O3 (CMake Release default) -mcpu=cortex-a72 -flto=2 -flto-partition=one -fno-fat-lto-objects
    linker: -Wl,-u,pippin_cpu_owner_guest_time_ns
    CMAKE_AR=gcc-ar  CMAKE_RANLIB=gcc-ranlib

- LTO must use a single partition: the inline-asm dispatcher uses
  symbol-difference encodings that break across LTO partitions.
- Do not use PGO: it reorders globals, and `sheep_cpu_loop_a64`
  encodes 9-bit offsets from `g_icycles` (the startup self-tests
  detect this).
- `build-pi4-chroot.sh` is the console script used for deployed
  builds (it builds inside a private chroot with SDL2 and gcc 13).
