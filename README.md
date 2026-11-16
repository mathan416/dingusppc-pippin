# DingusPPC-Pippin

A fork of [DingusPPC](https://github.com/dingusdev/dingusppc) that runs the **Apple/Bandai
Pippin** at full speed on a **Raspberry Pi 4**, with an ARM64 JIT from the
[SheepShaver](https://github.com/kanjitalk755/macemu) lineage.

## The Pippin

The Pippin (1995–1997) was Apple's game-console and multimedia platform, sold by Bandai
as the *Pippin Atmark* in Japan and *@WORLD* in the US. Under the hood it's a cut-down
Power Macintosh:

| | |
| --- | --- |
| CPU | PowerPC 603 at 66 MHz |
| RAM | 5–6 MB (expandable) |
| Video | Taos/Athens: 640×480, NTSC/PAL or VGA output |
| Sound | AWACS, 16-bit stereo |
| Storage | 4× CD-ROM, no hard disk; small flash for saves |
| Controller | AppleJack (ADB) gamepad with trackball |
| OS | A System 7.5 derivative booted from each game disc |

DingusPPC already emulates the Pippin hardware (Aspen memory controller, Taos video,
MESH SCSI CD-ROM, AppleJack controller), but its PowerPC interpreter is too slow on a
Pi 4 to run games at real speed. This fork adds a JIT and a set of speed-ups, plus a
few Pippin accuracy fixes.

## What this fork changes

### Speed

- ARM64 JIT (`cpu/ppc/nw_jit.*`, `nw_io.*`, `sheep_jit_bridge.*`) that translates
  PowerPC blocks to native code. Anything it doesn't handle falls back to the DingusPPC
  interpreter. `SHEEP_PIPPIN_JIT=verify` checks it instruction by instruction against a
  C reference.
- Fast dispatch for ROM code, and translated ROM code is kept across MMU mode changes.
- Guest RAM mapped straight into host memory, with `tlbie` invalidating only what it
  needs to instead of flushing the whole TLB.
- Faster MMU lookups for the common cases.
- The guest CPU runs on its own thread, separate from video and audio.
- Pi 4 build flags: `-mcpu=cortex-a72` and single-partition LTO (`pippin/pi4-build/`).

### Accuracy

- `--realtime` ties guest timers to the host clock, so music plays at the right pitch
  and tempo.
- Taos video timing fixed: NTSC now refreshes at 59.94 Hz (it was 35 Hz).
- Audio is paced by the guest and played through cubeb.
- AppleJack button and trackball mapping for modern gamepads.
- Saves to Pippin flash and NVRAM.

### Authentic-speed mode

Off by default. It limits the CPU to roughly a real 603 and can also slow CD-ROM reads to
4× speed. Both were tuned against video of a real Pippin. See
[PIPPIN.md](PIPPIN.md#authentic-speed-mode-q234).

## Results on a Raspberry Pi 4 (stock 1.5 GHz)

- The CPU-heavy part of the L-Zone intro runs at about 146% of a real Pippin.
- Every game we tested runs faster than real hardware; Power Rangers is the closest, at
  about 23% faster.
- Anything paced by timers or audio runs at the correct speed, checked against a
  recording of a real console.

## SheepShaver lineage

The JIT comes from the SheepShaver ARM64 work by **Bill Cavalieri**
([billcavalieri/macemu](https://github.com/billcavalieri/macemu), branch `arm64-jit`,
commit `1e6abc0`). SheepShaver itself is © 1997–2008 Christian Bauer, Marc Hellwig and
the macemu contributors.

This fork takes SheepShaver's ARM64 code generator and connects it to DingusPPC's Pippin
machine model through a new bridge (`sheep_jit_bridge.*`).

## Licence and credits

GPL-3.0-or-later (see `LICENSE`). The emulator core and Pippin machine model are the
work of the DingusPPC team (see `CREDITS.md`); the ARM64 code generator comes from the
SheepShaver/macemu authors. The SheepShaver-derived files keep their GPL-2.0-or-later
notices.

No ROMs or games are included. You'll need your own Pippin ROM (tested with Kinka 1.3)
and discs.

See [PIPPIN.md](PIPPIN.md) for provenance, change history, build instructions and known
issues.

## Building

The JIT emits ARM64 code, so build on 64-bit ARM Linux. We build and test on a Raspberry
Pi 4 running 64-bit Raspberry Pi OS. You need a C++20 compiler (we use GCC 13), CMake and
the SDL2 development headers.

```
git clone --recursive -b pippin https://github.com/mathan416/dingusppc-pippin
cd dingusppc-pippin
sudo apt install cmake libsdl2-dev
```

Pi 4 release build:

```
FLAGS="-mcpu=cortex-a72 -flto=2 -flto-partition=one -fno-fat-lto-objects"
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$FLAGS -Wl,-u,pippin_cpu_owner_guest_time_ns" \
      -DCMAKE_AR=$(which gcc-ar) -DCMAKE_RANLIB=$(which gcc-ranlib) ..
cmake --build . --parallel 2
```

The binary is `build/bin/dingusppc`.

- Keep LTO to a single partition. The CPU loop's inline assembly uses symbol offsets that
  break if LTO splits it up.
- Don't use PGO. It moves globals that the CPU loop expects at fixed offsets. The startup
  self-tests catch this.

See `pippin/pi4-build/` for the exact script behind our deployed builds.

## Running

You need a Pippin ROM (we test with Kinka 1.3, `kinka13.rom`) and the game's data track
as a raw image.

```
SHEEP_PIPPIN_JIT=on SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 \
SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on \
./dingusppc -r --realtime --start-date=1997-06-01 -m pippin -b kinka13.rom \
  --cdr_config=ScsiMesh:3 --cdr_img2=disc.bin --rambank3_size=8 \
  --adb_devices=AppleJack,Keyboard
```

| Option | Meaning |
| --- | --- |
| `-r` | Run the emulator (`-d` opens the debugger instead) |
| `--realtime` | Tie guest time to the host clock. Without it, music and timers run slow |
| `--start-date` | Date the Pippin's clock starts at |
| `-m pippin -b FILE` | Machine and boot ROM |
| `--cdr_img2=FILE` | Disc image |
| `--adb_devices=AppleJack,Keyboard` | Attach an AppleJack controller and a keyboard |

Without the `SHEEP_*` variables it runs on the plain DingusPPC interpreter, which is
accurate but slow. `SHEEP_PIPPIN_JIT=verify` runs the start-up self-tests and checks the
JIT against the interpreter as it goes. The authentic-speed settings are listed in
[PIPPIN.md](PIPPIN.md#authentic-speed-mode-q234).

### Controls

A mouse or trackball moves the pointer and clicks, much like the trackball built into the
real AppleJack. A gamepad maps to the AppleJack like this:

| Gamepad | AppleJack |
| --- | --- |
| A / B / X / Y | Red / Green / Yellow / Blue |
| D-pad | Directional pad |
| LB / RB | Left / right trigger (these also act as mouse clicks) |
| Back / Guide / Start | The three front buttons |

## About DingusPPC

DingusPPC is written by the DingusPPC Development Team: Divingkatae, maximumspatium,
joevt, mihaip, kkaisershot, Cacodemon345, Wack0 and others (see `CREDITS.md`). It
emulates several Old World and early New World Power Macs as well as the Pippin, and
includes a debugger with a disassembler. For its other machines and general use, see the
[upstream project](https://github.com/dingusdev/dingusppc).
