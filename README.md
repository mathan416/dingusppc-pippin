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
- Pi 4 build flags: `-mcpu=cortex-a72` and single-partition LTO (`pippin/build.sh`).

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

## Downloading a release

Prebuilt binaries are on the [Releases](https://github.com/mathan416/dingusppc-pippin/releases)
page: one tuned for the Pi 4 and one for the Pi 5. Both need **64-bit Raspberry Pi OS
Bookworm** or newer, plus these libraries (most are already installed on the desktop
image):

```
sudo apt install libsdl2-2.0-0 libasound2 libpulse0 libsamplerate0
```

The steps are the same on both boards; only the file name changes. Set `PI` to `pi4` or
`pi5`, then download, unpack and start a game:

```
PI=pi4    # or pi5
wget https://github.com/mathan416/dingusppc-pippin/releases/download/v1.0.1/dingusppc-pippin-v1.0.1-$PI.tar.gz
tar xf dingusppc-pippin-v1.0.1-$PI.tar.gz
cd dingusppc-pippin-v1.0.1-$PI
./run.sh /path/to/kinka13.rom /path/to/disc.bin
```

Add `PIPPIN_AUTHENTIC=on` in front of `./run.sh` to run at a real Pippin's CPU speed.
No ROM or game discs are included: you need your own, and the disc image must be
writable. The Pi 4 build also runs on a Pi 5, a little slower than the Pi 5 build.

**Pi 5 only:** before the first run, switch to the 4 KB-page kernel so the fast memory
path is used (one-time; see [Raspberry Pi 5](#raspberry-pi-5)):

```
echo 'kernel=kernel8.img' | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

After the reboot, `getconf PAGESIZE` should print `4096`.

## Documentation

| Document | What's in it |
| --- | --- |
| [docs/configuration.md](docs/configuration.md) | Every option and environment variable, keyboard shortcuts, and the files the emulator writes |
| [docs/controllers.md](docs/controllers.md) | Gamepads, trackballs and keyboards without extra software; adding a Pippin system to RetroPie |
| [docs/architecture.md](docs/architecture.md) | The Pippin's hardware, the PowerPC 603, and how the JIT, fastmem and threading work |
| [docs/testing.md](docs/testing.md) | Self-tests, unit tests, verify mode, speed checks, and what CI and releases run |
| [PIPPIN.md](PIPPIN.md) | Provenance, licences, the change history with measurements, and known issues |

## Building

The JIT emits ARM64 code, so build on 64-bit ARM Linux. We build and test on a Raspberry
Pi 4 running 64-bit Raspberry Pi OS (see below for the Pi 5). You need a C++20 compiler
(we use GCC 13), CMake and the SDL2 development headers.

```
git clone --recursive -b pippin https://github.com/mathan416/dingusppc-pippin
cd dingusppc-pippin
sudo apt install cmake libsdl2-dev
```

Then build:

```
pippin/build.sh
```

The script picks the right CPU tuning for a Pi 4 (`cortex-a72`) or Pi 5 (`cortex-a76`)
and puts the binary in `build/bin/dingusppc`. If you build by hand, keep its two rules:

- Keep LTO to a single partition. The CPU loop's inline assembly uses symbol offsets that
  break if LTO splits it up.
- Don't use PGO. It moves globals that the CPU loop expects at fixed offsets. The startup
  self-tests catch this.

## Running

You need a Pippin ROM (we test with Kinka 1.3, `kinka13.rom`) and the game's data track
as a raw image. Neither is included: you have to supply your own.

The simplest way to start a game is the launcher, which sets everything below:

```
pippin/run.sh kinka13.rom disc.bin
PIPPIN_AUTHENTIC=on pippin/run.sh kinka13.rom disc.bin   # real Pippin CPU speed
```

The full command it runs:

```
SHEEP_PIPPIN_JIT=on SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 \
SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off \
SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on mesa_glthread=false \
DINGUS_AUDIO_GUEST_SYNC=1 \
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

The analog stick isn't used; steer the pointer with a mouse or trackball. See
[docs/controllers.md](docs/controllers.md) for PlayStation and Nintendo layouts, unusual
pads, and setting up RetroPie.

## Raspberry Pi 5

The Pi 5 runs the same code. On a Pi 500 (same chip as the Pi 5) the L-Zone intro runs
at about 3.6 times the Pi 4's speed, so every game has plenty of headroom.

- **Use the 4 KB-page kernel.** Raspberry Pi OS boots the Pi 5 with 16 KB memory pages,
  and fastmem only works with 4 KB pages. With 16 KB pages it switches itself off and
  the emulator still runs, just slower. Add this to `/boot/firmware/config.txt` and
  reboot:

  ```
  kernel=kernel8.img
  ```

  `getconf PAGESIZE` should then print `4096`.
- **Use the Pi 5 release build, or build with `pippin/build.sh`**, which tunes for the
  Pi 5's Cortex-A76.
- **Audio:** the Pi 5 has no headphone jack, so sound goes to HDMI or a USB audio device.
  If you have no sound, check which ALSA device SDL is using.
- **Display:** under KMS/DRM the Pi 5 numbers its display devices differently from the
  Pi 4. If SDL picks the wrong one, set `SDL_KMSDRM_DEVICE_INDEX`.

## About DingusPPC

DingusPPC is written by the DingusPPC Development Team: Divingkatae, maximumspatium,
joevt, mihaip, kkaisershot, Cacodemon345, Wack0 and others (see `CREDITS.md`). It
emulates several Old World and early New World Power Macs as well as the Pippin, and
includes a debugger with a disassembler. For its other machines and general use, see the
[upstream project](https://github.com/dingusdev/dingusppc).
