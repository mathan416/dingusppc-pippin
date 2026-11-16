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

---

*The original DingusPPC README follows.*

# DingusPPC

Written by The DingusPPC Development Team, who are Divingkatae, maximumspatium, joevt, mihaip, kkaisershot, Cacodemon345, Wack0, et. al. Check CREDITS.md for more info.

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
