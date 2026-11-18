# Configuration reference

Everything you can set when running DingusPPC-Pippin: the launcher, command-line options,
machine properties, environment variables and keyboard shortcuts.

If you just want to play a game, use the launcher and skip the rest of this page:

```
./run.sh kinka13.rom disc.bin
```

## Contents

- [The launcher: run.sh](#the-launcher-runsh)
- [Command-line options](#command-line-options)
- [Pippin machine properties](#pippin-machine-properties)
- [Environment variables](#environment-variables)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Files the emulator writes](#files-the-emulator-writes)

## The launcher: run.sh

`run.sh` (in the release archive, or `pippin/run.sh` in the source tree) starts a disc with
the settings every release build was tested with.

```
run.sh ROM DISC [extra dingusppc options]
```

| Argument or variable | Meaning |
| --- | --- |
| `ROM` | Pippin boot ROM. Tested with Kinka 1.3 (`kinka13.rom`). |
| `DISC` | The game: a `.cue` sheet (with its `.bin` files), a `.chd`, or a raw `.bin`/`.iso` data track. For CUE and CHD the launcher copies the first data track once into `~/.cache/dingusppc-pippin/discs/` and reuses it until the disc changes; CHD needs `chdman` (`sudo apt install mame-tools`). A raw image is used as it is and must be writable, because DingusPPC opens disc images read/write. |
| extra options | Passed straight to `dingusppc`, after the launcher's own. |
| `PIPPIN_AUTHENTIC=on` | Cap the CPU at a real Pippin's speed (26 MIPS). The CD-ROM timing model stays off. |
| `PIPPIN_SAVES=FOLDER` | Keep each game's saves in its own subfolder of `FOLDER`, named after the disc. The RetroPie installer uses `~/RetroPie/saves/pippin`. |
| `PIPPIN_PRELOAD=LIB.so` | Load a 64-bit shared library into the emulator (`LD_PRELOAD`), also inside the runtime. For front ends that hook SDL, for example to route controllers or add a gamepad quit combination. |
| `PIPPIN_RUNTIME=FOLDER` | On a 64-bit kernel with a 32-bit userland, the runtime to run in (default: `runtime/root` next to `run.sh`). See [installing.md](installing.md). |
| `DINGUSPPC=/path/to/dingusppc` | Use this binary. Otherwise the launcher looks next to itself, then in `../build/bin/`. |

The launcher needs `python3` (to read the disc). On a 32-bit userland it runs the emulator
inside the 64-bit runtime through `sudo`; on a fully 64-bit system it runs it directly.

It runs:

```
SHEEP_PIPPIN_JIT=on SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 \
SHEEP_CPU_OWNER=on SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off \
SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on mesa_glthread=false \
DINGUS_AUDIO_GUEST_SYNC=1 \
dingusppc -r --realtime --start-date=1997-06-01 -m pippin -b ROM \
  --cdr_config=ScsiMesh:3 --cdr_img2=DISC --rambank3_size=8 \
  --adb_devices=AppleJack,Keyboard
```

Saves go to the current directory (see [Files the emulator writes](#files-the-emulator-writes)).
To keep each game's saves separate, set `PIPPIN_SAVES`, or pass `-w FOLDER` as an extra
option.

## Command-line options

The options a Pippin user needs. `dingusppc --help-all` lists every option, including
those for the other Macintosh models DingusPPC emulates.

| Option | Meaning |
| --- | --- |
| `-m pippin` | Emulate the Bandai Pippin. Without `-m`, DingusPPC tries to recognise the ROM. |
| `-b FILE` | Boot ROM (default `bootrom.bin`). |
| `-r` | Run straight away. |
| `-d` | Start in DingusPPC's built-in debugger instead. The fast CPU thread (`SHEEP_CPU_OWNER=on`) needs `-r`. |
| `--realtime` | Drive the guest's timers from the host clock. Without it, guest time follows the instruction count, so music and animation run at the wrong speed. Authentic-speed mode needs it. |
| `--start-date=YYYY-MM-DD` | Date the Pippin's clock starts at, keeping the current time of day. Some titles behave better with a 1990s date; the launcher uses 1997-06-01. |
| `-w DIR` | Working directory: where `nvram.bin`, `pram.bin` and `pippin-flash.bin` are read and written. |
| `--jit-selftest` | Build the machine, run the JIT self-tests, print `PIPPIN JIT SELFTEST PASS` or `FAIL`, and exit (0 = pass). Needs no real ROM: any 4 MB file will do. |
| `--log-to-stderr` | Print the log to the terminal instead of `dingusppc.log`. |
| `--log-verbosity=N` | Log detail. `0` is the default (INFO); `-1` warnings only; higher numbers add detail. |
| `--log-no-uptime` | Drop the uptime prefix from log lines. |
| `--auto-grab-mouse` | Capture the mouse when the guest shows its cursor. |
| `--swap-command-option` | Swap the Command and Option keys. |
| `--deterministic` | Deterministic execution for debugging; not for play. |

## Pippin machine properties

Machine properties are passed as options too (`--name=value`). `dingusppc list properties`
prints them for every machine.

| Property | Values | Meaning |
| --- | --- | --- |
| `cdr_config` | `ScsiMesh:3` | Where the CD-ROM drive sits: bus and SCSI ID. The Pippin's drive is on the internal MESH bus at ID 3. The machine's built-in default is `ScsiCurio:3`; use `ScsiMesh:3`, which is what every release was tested with. |
| `cdr_img2` | path | Disc image for a drive on the MESH bus (the "2" is MESH's bus number). With `cdr_config=ScsiMesh:3` this is the game disc. |
| `cdr_img` | path | Disc image for the external (Curio) SCSI bus. Not needed for the Pippin. |
| `rambank1_size` | `4` | Soldered RAM, fixed at 4 MB. |
| `rambank2_size` | `1` | Soldered RAM, fixed at 1 MB. |
| `rambank3_size`, `rambank4_size` | `0`, `1`, `4`, `8`, `16` | Expansion RAM in MB. The launcher adds 8 MB (13 MB total), which every tested title is happy with. |
| `adb_devices` | e.g. `AppleJack,Keyboard` | ADB devices to attach. `AppleJack` is the Pippin controller (with its trackball acting as a mouse); `Keyboard` lets you type. |
| `video_out` | `NTSC`, `PAL`, `VGA` | The video standard the Taos chip reports to the game. NTSC (59.94 Hz) is the tested default. |
| `serial_backend` | `null`, `stdio`, `socket` | Where the serial ports go. |
| `emmo` | `on`, `off` | Run the factory hardware tests at start-up. |

## Environment variables

Most of the speed-ups are controlled by environment variables. They're grouped below by how
safe they are to change.

### The release settings

These are what `run.sh` sets. Every release build was verified and speed-tested with exactly
this combination, so change them only to compare or troubleshoot.

| Variable | Values (default first) | What it does |
| --- | --- | --- |
| `SHEEP_PIPPIN_JIT` | unset, `on`, `verify` | Unset: the plain DingusPPC interpreter (correct but slow; the emulator logs a warning). `on`: translate PowerPC code to ARM64. `verify`: translate, but also run every block through a reference interpreter and compare the results (much slower; for testing). |
| `SHEEP_PIPPIN_FASTMEM` | unset, `on` | Map guest RAM straight into host memory so translated loads and stores are single ARM64 instructions. Needs 64-bit Linux with 4 KB pages; on anything else it logs why and switches itself off. |
| `SHEEP_PIPPIN_ROM_ONLY` | unset, `1` | Translate only code running from the 4 MB ROM. The Mac OS Toolbox and the 68K emulator, where Pippin titles spend most of their time, live there, and ROM code can never change. Code in RAM is interpreted, through the fast inner loop (`SHEEP_PIPPIN_CPU_RAM`). |
| `SHEEP_PIPPIN_ROM_WINDOW` | unset, `on` | Keep translated ROM code valid across MMU mode changes and interrupts, instead of re-checking the translation every time. |
| `SHEEP_PIPPIN_ISLAND_PACK` | unset, `on` | Lay out translated code with frequently used paths together and rarely used ones apart, which helps the host's instruction cache. |
| `SHEEP_CPU_OWNER` | unset/`off`, `on` | Run the emulated CPU on its own thread, separate from the SDL thread that draws the screen, plays audio and reads input. Needs `-r`. |
| `SHEEP_RAW_FRAME` | unset/`off`, `on` | With the CPU thread on, hand the screen to the SDL thread as raw 8-bit or 15-bit pixels plus a palette, and convert to RGB there. Takes conversion work off the CPU thread. |
| `SHEEP_FRAME_POOL` | unset/`off`, `on` | An alternative frame hand-off that reuses three RGB buffers. Leave it off: raw frames are faster, and turning the pool on disables them. |
| `DINGUS_AUDIO_GUEST_SYNC` | unset, any value | Pace audio to the game's own output, with a 75 ms buffer and small rate corrections, instead of playing samples as soon as they arrive. Smooths out sound when the CPU thread runs ahead or falls behind. |
| `mesa_glthread` | `false` | A Mesa graphics-driver setting. Turns off Mesa's extra GL thread, which only adds latency and CPU contention for a 640×480 picture on a Pi. |

### Authentic-speed mode

Off by default, and only active with `--realtime`. Guest timers, video and audio are
unaffected; only how fast the guest CPU (and optionally the CD-ROM) works changes.

| Variable | Values | What it does |
| --- | --- | --- |
| `SHEEP_PIPPIN_AUTHENTIC` | `off`, `on` | `on` applies the calibrated preset: CPU capped at 26 million instructions per second, and a 4× CD-ROM (600 KiB/s, 180 ms average access, 50 ms minimum, 256 KiB buffer). |
| `SHEEP_PIPPIN_MAX_MIPS` | `off`, `0`, `1`–`2000` | CPU cap in million guest instructions per second. Overrides the preset. |
| `SHEEP_PIPPIN_CD_SPEED` | `off`, `0`, `1x`–`64x`, `16`–`99999` | CD transfer rate, as a multiple of 1× (150 KiB/s) or in KiB/s. `off` keeps the CD at full emulator speed. |
| `SHEEP_PIPPIN_CD_SEEK_MS` | `0`–`2000` | Average seek time (default 180). |
| `SHEEP_PIPPIN_CD_SEEK_MIN_MS` | `0`–`2000` | Shortest seek, for nearby reads (default 50). |
| `SHEEP_PIPPIN_CD_READAHEAD_KB` | `0`–`4096` | Drive buffer size (default 256). |

The launcher's `PIPPIN_AUTHENTIC=on` sets `SHEEP_PIPPIN_AUTHENTIC=on SHEEP_PIPPIN_CD_SPEED=off`:
CPU cap only. With the CD-ROM model on, the L-Zone intro sometimes stalls (about 1 run in 8;
see [PIPPIN.md](../PIPPIN.md#known-issues)), so it stays off until that's understood.
Every setting is logged at start-up on a line beginning `PIPPIN-AUTHENTIC`.

### Fine-grained switches (on by default)

These are on unless set to `off`. They exist so a single speed-up can be turned off to
isolate a problem. There's no reason to change them otherwise.

| Variable | What `off` disables |
| --- | --- |
| `SHEEP_PIPPIN_FAST_CACHE` | The ROM fast dispatcher: a table that jumps straight to translated code for a ROM address. |
| `SHEEP_PIPPIN_ROM_DIRECT` | Calling translated ROM blocks directly from the dispatch table (needs `SHEEP_PIPPIN_ROM_ONLY=1`). |
| `SHEEP_PIPPIN_CPU_LOOP` | The hand-written ARM64 inner loop that runs straight-line guest code without returning to C++ between instructions. |
| `SHEEP_PIPPIN_CPU_RAM` | Letting that inner loop run code from RAM as well as ROM. |
| `SHEEP_PIPPIN_PRECISE_TLBIE` | Invalidating only the affected translations on a `tlbie` instruction, instead of flushing them all. |
| `SHEEP_PIPPIN_MODE_WINDOW` | Keeping the ROM window across MMU mode switches that don't change the ROM mapping. |

### Diagnostics

| Variable | What it does |
| --- | --- |
| `PIPPIN_INPUT_TRACE` (any value) | Log every gamepad button and pointer-button change the emulator receives. Useful when setting up a controller. |
| `SHEEP_PIPPIN_CD_TRACE=1` | Log each CD-ROM read the timing model sees, with its seek and start time. |
| `DINGUS_AUDIO_TRACE=PREFIX` | Write the raw audio the game produces to `PREFIX-RATE-N.pcm` (16-bit stereo). |
| `NW_JIT`, `NW_JIT_STATS` | Switches inherited from SheepShaver's JIT. Leave unset; the Pippin build is controlled by `SHEEP_PIPPIN_JIT`. |

### SDL settings that sometimes help

DingusPPC uses [SDL 2](https://wiki.libsdl.org/SDL2/FrontPage), so the usual SDL variables work.

| Variable | When to use it |
| --- | --- |
| `SDL_GAMECONTROLLERCONFIG` | Add a mapping for a gamepad SDL doesn't recognise. See [controllers.md](controllers.md). |
| `SDL_AUDIODRIVER` | Choose the sound system (`alsa`, `pulseaudio`, `pipewire`, `dummy`). |
| `SDL_VIDEODRIVER` | Choose the video system (`kmsdrm` on a console without a desktop, `x11`, `wayland`). |
| `SDL_KMSDRM_DEVICE_INDEX` | Without a desktop, force a particular display device. Normally leave it unset: SDL picks the device that has a display, and on a Pi the card numbers can change between boots, so a fixed index can break after a reboot. |

### Variables you may see on the cabinet

`DINGUS_CABINET_ROUTER`, `DINGUS_CABINET_NATIVE` and `PIPPIN_V48_CABINET` appear in our
cabinet's launcher. The emulator doesn't read them: they mark the process so Controller
Router can recognise it. They're harmless anywhere else.

## Keyboard shortcuts

These work with a keyboard attached, and aren't passed to the game. "Ctrl" means the left
Control key.

| Keys | Action |
| --- | --- |
| Ctrl+F | Toggle full screen |
| Ctrl+Shift+F | Toggle full screen (alternative mode) |
| Ctrl+= / Ctrl+- | Bigger / smaller window |
| Ctrl+S | Cycle the scaling quality |
| Ctrl+G | Capture or release the mouse |
| Ctrl+L | Cycle the log detail |
| Ctrl+Alt+R | Toggle `--realtime` |
| Ctrl+Alt+Shift+= / - | Make the instruction clock faster or slower (only matters without `--realtime`) |
| Ctrl+D | Stop and enter the built-in debugger (in the terminal). Not useful while playing. |
| Right Alt+Delete | Send Ctrl+Alt+Delete to the guest |

To quit, close the window, press Ctrl+C in the terminal you started it from, or end the
process. There's no quit button on the gamepad itself; see
[controllers.md](controllers.md#quitting-a-game).

## Files the emulator writes

All in the working directory (`-w`, default: the current directory).

| File | Holds |
| --- | --- |
| `pippin-flash.bin` | The Pippin's flash memory, where games keep save data. |
| `nvram.bin` | Open Firmware / system NVRAM. |
| `pram.bin` | Parameter RAM (clock settings, volume and so on), held by the Cuda chip on real hardware. |
| `dingusppc.log` | The log, unless `--log-to-stderr` is given. |

To reset the console to factory state, quit the emulator and delete the three `.bin` files; they're recreated blank on the next start. Deleting `pippin-flash.bin` erases every game's saves.
