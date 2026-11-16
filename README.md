# DingusPPC-Pippin

A fork of [DingusPPC](https://github.com/dingusdev/dingusppc) that runs the **Apple/Bandai
Pippin** at full speed on a **Raspberry Pi 4**, with an ARM64 JIT from the
[SheepShaver](https://github.com/kanjitalk755/macemu) lineage.

## The Pippin

The Pippin (1995–1997) was Apple's game-console and multimedia platform, sold by Bandai
as the *Pippin Atmark* in Japan and *@WORLD* in the US. It is essentially a cut-down
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

DingusPPC already models the Pippin hardware (the Aspen memory controller, Taos video,
the MESH SCSI CD-ROM and the AppleJack controller). Its PowerPC interpreter is accurate
but too slow on a Pi 4 to run Pippin games at their real speed. This fork makes it fast
enough and keeps it accurate.

## What this fork changes

**Speed**
- **ARM64 JIT** (`cpu/ppc/nw_jit.*`, `nw_io.*`, `sheep_jit_bridge.*`): translates guest
  PowerPC blocks to native ARM64. A per-instruction C oracle cross-checks it
  (`SHEEP_PIPPIN_JIT=verify`). Anything the JIT does not handle falls back to the
  DingusPPC interpreter.
- **ROM fast dispatcher** and a **ROM window** that keeps the hot Pippin ROM code
  translated across MMU mode changes.
- **Fastmem**: guest RAM mapped directly into the host address space, with page
  guards and precise `tlbie` invalidation instead of full TLB flushes.
- **MMU fast paths**: a leaf DTLB-hit read path, and a live instruction TLB.
- **CPU-owner threading**: the guest CPU runs on its own thread, separate from video and
  audio presentation.
- **Pi 4 build**: `-mcpu=cortex-a72` with single-partition LTO (`pippin/pi4-build/`).

**Pippin accuracy**
- **Realtime guest clock** (`--realtime`): guest timers follow the host clock, so music
  pitch and tempo match real hardware.
- **Taos video timing**: decodes the half-line timing registers and the Athens crystal
  correctly. Refresh is 59.94 Hz NTSC; it was 35 Hz before.
- **Audio**: guest-paced PCM delivery through cubeb, with no dropped frames in testing.
- **AppleJack controller**: button and trackball mapping for modern gamepads.
- **Save flash**: Pippin flash and NVRAM persistence.

**Optional authentic-speed mode** (off by default): caps the CPU at a real 603's
throughput and can model 4× CD-ROM transfer and seek times, both calibrated against
real-hardware footage. See [PIPPIN.md](PIPPIN.md#authentic-speed-mode-q234).

## Results on a Raspberry Pi 4 (stock 1.5 GHz)

- The CPU-heavy part of the L-Zone intro runs at about **146%** of a real Pippin.
- Every title in our survey runs faster than real hardware. Power Rangers has the
  smallest margin, at about 23%.
- Timer- and audio-paced content runs at true speed, matched against a recording of
  real hardware.

A Pi 4 is enough; a Pi 5 is not required.

## SheepShaver lineage

The JIT comes from the SheepShaver ARM64 work by **Bill Cavalieri**
([billcavalieri/macemu](https://github.com/billcavalieri/macemu), branch `arm64-jit`,
commit `1e6abc0`). SheepShaver itself is © 1997–2008 Christian Bauer, Marc Hellwig and
the macemu contributors.

SheepShaver cannot boot the Pippin: it rejects the Pippin ROM and has no Pippin
hardware. This fork therefore takes only SheepShaver's ARM64 code generator and connects
it to DingusPPC's Pippin machine model through a new bridge (`sheep_jit_bridge.*`). Those
files keep their GPL-2.0-or-later notices.

## Licence and credits

The combined work is **GPL-3.0-or-later** (see `LICENSE`). Credit for the emulator core
and Pippin machine model belongs to the DingusPPC team (`CREDITS.md`). Credit for the
ARM64 emitter belongs to the SheepShaver/macemu authors. No firmware or game media is
included; you need your own Pippin ROM (Kinka 1.3 tested) and discs.

[PIPPIN.md](PIPPIN.md) covers the full provenance, change history, build instructions
and known issues.

---

*The original DingusPPC README follows unchanged.*

# DingusPPC

Written by The DingusPPC Development Team

(who are Divingkatae, maximumspatium, joevt, mihaip, kkaisershot, Cacodemon345, Wack0, et. al. Check CREDITS.md for more info)

Be warned the program is highly unfinished and could use a lot of testing. Any feedback is welcome.

## Philosophy of Use

While many other PowerPC emus exist (PearPC, Sheepshaver), none of them currently attempt emulation of PowerPC Macs
accurately (except for QEMU).

This program aims to not only improve upon what Sheepshaver, PearPC, and other PowerPC Mac emulators have done, but
also to provide a better debugging environment. This currently is designed to work best with PowerPC NuBus and Old
World ROMs, including those of the Power Mac 6100, 7200, and G3 Beige.

## Implemented Features

Several machines have been implemented to varying degrees, like many Old World PowerPC Macs, early New World
PowerPC Macs, and the Pippin.

This emulator has a debugging environment, complete with a disassembler. 

## How to Use

This program currently uses the command prompt to work.

There are a few command line arguments one can enter when starting the program.

```
-r, --run
```

Run the emulator. Currently only goes to an interpreter.

```
-d, --debugger
```

Enter the interactive debugger.

```
-b, --bootrom TEXT:FILE
```

Specifies the Boot ROM path. It otherwise looks for bootrom.bin.

```
-m, --machine TEXT
```

Specify machine ID. Otherwise, the emulator will attempt to determine machine ID from the boot rom otherwise.

As of now, the most complete machines are the Power Mac 6100, the Power Mac 7500, and the Power Mac G3 Beige.

To go into to the debugger regardless of how you started the emulator, press Control and C on the terminal window.

## How to Compile

You need to install development tools first.

At minimum, a C++20 compliant compiler and [CMake](https://cmake.org) are required.

Clone the repository using the appropriate command:

```
git clone https://github.com/dingusdev/dingusppc
```

If this is from a mirror, replace the argument with the source you want to use instead.

You will also have to recursive clone or run

```
git submodule update --init --recursive
```

This is because the CubeB, Capstone, and SDL2 modules are not included by default.

For SDL2, Linux users may also have to run:

```
sudo apt install libsdl2-dev
```

macOS users can use Homebrew or MacPorts to install SDL2.

CLI11 and loguru are already included in the thirdparty folder and compiled along with the rest of DingusPPC.

For Raspbian, you may also need the following command:

```
sudo apt install doxygen graphviz
```

To build the project in a Unix-like environment, create a build folder, change the directory to the build folder,
and use `cmake` to create the `Makefile` files in that build folder.
Use `make` to do the building. You don't need to execute `cmake` again unless you add/remove/change options or
source files.

```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

You may specify another build type using the variable `CMAKE_BUILD_TYPE`. 
Each build type should have its own build folder.

```
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

## Testing

DingusPPC includes a test suite for verifying the correctness of its PowerPC CPU
emulation. To build the tests, use the following terminal commands:

```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DDPPC_BUILD_PPC_TESTS=True ..
make testppc
```

## Intended Minimum Requirements

- Windows 7 or newer (64-bit), Linux 4.4 or newer, Mac OS X 10.9 or newer (64-bit)
- Intel Core 2 Duo or better
- 2 GB of RAM
- 2 GB of Hard Disk Space
- Graphics Card with a minimum resolution of 800*600
