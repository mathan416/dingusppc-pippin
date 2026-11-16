# DingusPPC-Pippin: provenance and history

A fork of [DingusPPC](https://github.com/dingusdev/dingusppc) for running the
Apple/Bandai Pippin at full speed on a Raspberry Pi 4, with an ARM64 JIT
from the SheepShaver lineage.

## Provenance

| Part | Origin | Licence |
| --- | --- | --- |
| Emulator core: machine models (incl. Pippin), devices, MMU, interpreter, main loop | DingusPPC, branched from `dingusdev/dingusppc@810c3f9` (2026-10-28) | GPL-3.0-or-later |
| `cpu/ppc/nw_jit.*`, `cpu/ppc/nw_io.*`: New World ARM64 JIT | SheepShaver lineage: `billcavalieri/macemu` branch `arm64-jit` @ `1e6abc0`, © 2026 Bill Cavalieri; SheepShaver © 1997-2008 Christian Bauer and Marc Hellwig | GPL-2.0-or-later |
| Bridge, ROM fast dispatcher, fastmem, CPU-owner threading, Pippin fixes | This fork | GPL-3.0-or-later |
| `thirdparty/capstone`, `thirdparty/cubeb` | Upstream submodules, unchanged and at the same pinned commits | their own licences |

The combined work is distributed under **GPL-3.0-or-later** (see `LICENSE`).
GPL-2.0-or-later code may be combined into a GPL-3.0 work. Keep every
copyright notice and `CREDITS.md`, and provide source with any binary you
distribute.

How the base commit was identified: compared with every upstream commit,
`810c3f9` has the most files byte-identical to the fork (243 of 265; its
neighbours match one fewer). Exactly 22 upstream files are modified, and the
two submodules are identical to upstream's pinned commits.

## History on this branch

| Commit | Change | Measured effect (Pi 4, realtime timers) |
| --- | --- | --- |
| Q219 baseline | JIT, dispatcher, fastmem, CPU owner and Pippin fixes, squashed. The earlier history (Q48-Q219) was never under version control. | — |
| Q224a | Build: `-mcpu=cortex-a72`, single-partition LTO (now `pippin/build.sh`) | L-Zone +2.5%, Tamagotchi +0.9% |
| Q226 | Read the exec-entry `getenv` lookups once | part of Q227 |
| Q227 | Precise `tlbie` (congruence class), ROM window kept across same-mode `mmu_change_mode` | L-Zone +7.8%, Tamagotchi +2.2%; fastmem remaps −80% |
| Q233 | Taos VBL at 59.94 Hz (half-line timing decode, 17.075 MHz Athens crystal) | Refresh 35.2 → 59.94 Hz; about −3% CPU |
| Q234 | Optional authentic-speed mode (off by default): CPU governor and CD-ROM timing model | Default unchanged (+0.7%, noise); L-Zone intro at 95% of a real Pippin with the preset |
| Review fixes | From an external code review: MESH DMA returns bytes delivered and keeps FIFO order; held CD transfers cancelled on DMA stop/pause, new commands and SCSI reset; CD timing scoped to the reading drive, reset on guest restart, needs `--realtime`; monotonic guest clock; governor batches never exceed the ungoverned size; controller hotplug; bounded window resize and per-axis DPI; README run command fixed | Pi 4 and Pi 500 speed unchanged (within noise) |
| F12 | ROM window kept across exceptions and `rfi` unless a deferred mapping change or endian switch is pending | ROM-window invalidations −50%; Pi 4 +0.5% (noise) |
| Pi 5 | `pippin/build.sh` tunes for Pi 4 or Pi 5; fastmem switches itself off on 16 KB-page kernels | Pi 500 (4 KB kernel): L-Zone about 137 M instr/s, 3.6× the Pi 4 |
| CI | `--jit-selftest`, CTest registration of the unit tests, ARM64 GitHub Actions job | — |
| v1.0.0 review | Frame pool off unless `SHEEP_FRAME_POOL=on`; `pippin/run.sh` launcher with the tested settings; warning when `-m pippin` runs without the JIT; `--jit-selftest` fails cleanly if the machine can't be built; cabinet-only build scripts removed | — |

On the L-Zone intro, the CPU-bound fly-in runs at about 146% of a real
Pippin. Timer- and audio-paced content runs at true speed under
`--realtime`: music pitch and tempo match real hardware.

## Building and running

Build with `pippin/build.sh` (see the README) and run with `pippin/run.sh ROM DISC`. Typical cabinet invocation:

    pippin-v233 -r --realtime --start-date=1997-06-01 -m pippin -b kinka13.rom \
      --cdr_config=ScsiMesh:3 --cdr_img2=disc.bin --rambank3_size=8 \
      --adb_devices=AppleJack,Keyboard

Runtime policy environment, as deployed: `SHEEP_PIPPIN_JIT=on
SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 SHEEP_CPU_OWNER=on
SHEEP_RAW_FRAME=on SHEEP_FRAME_POOL=off SHEEP_PIPPIN_ROM_WINDOW=on
SHEEP_PIPPIN_ISLAND_PACK=on mesa_glthread=false`. Raw frames need
`SHEEP_FRAME_POOL=off`; with `SHEEP_FRAME_POOL=on` (off by default since v1.0.0)
frames are converted to RGB on the guest thread instead.
`SHEEP_PIPPIN_JIT=verify` runs the startup self-tests and the interpreter oracle.
`--jit-selftest` runs only the self-tests and exits (0 = pass); it needs no real ROM, so CI
runs it with a zero-filled 4 MB file.

## Authentic-speed mode (Q234)

Off by default; acts only under `--realtime`. Guest timers, video and audio are untouched.

| Variable | Meaning |
| --- | --- |
| `SHEEP_PIPPIN_AUTHENTIC=on` | Calibrated preset: 26 MIPS CPU cap and a 4× CD (600 KiB/s, 180 ms average access, 50 ms minimum, 256 KiB buffer) |
| `SHEEP_PIPPIN_MAX_MIPS=N\|off` | CPU cap in million guest instructions per second |
| `SHEEP_PIPPIN_CD_SPEED=Nx\|KiB/s\|off` | CD transfer rate (1× = 150 KiB/s) |
| `SHEEP_PIPPIN_CD_SEEK_MS`, `SHEEP_PIPPIN_CD_SEEK_MIN_MS`, `SHEEP_PIPPIN_CD_READAHEAD_KB` | CD access-time model and drive buffer |

The cabinet's `pippin-authentic-speed` launcher entry uses `SHEEP_PIPPIN_AUTHENTIC=on
SHEEP_PIPPIN_CD_SPEED=off` (CPU cap only). With the CD model on, the L-Zone intro stall
(see Known issues) happens in roughly 1 run in 8, so the CD model stays off by default.

## Candidates for upstream DingusPPC

- Taos timing: decode half-lines, VBL per field, Athens crystal.
- Precise `tlbie` instead of a full PAT flush.
- Reading environment settings once at exec entry.
- MESH `xfer_from` returning the bytes it delivered (DBDMA expects that; upstream returns 0).
- Bounded window resizing and per-axis floating-point DPI scale.
- Opening game controllers connected after start-up.

## Known issues

- The L-Zone intro occasionally stays black and silent (about 2% historically; 0 of 64
  recent default-mode runs on Pi 4 and Pi 500). It also occurs on the Q219 baseline.
  Traced on 2026-11-16: every CD, SCSI/MESH, DMA, sound and VIA event matches a normal
  run up to the stall; the game's 68K code then spins on an asynchronous File Manager
  read whose `ioResult` never leaves "busy", and its Time Manager task stops re-arming.
  It is far more frequent with the CD timing model on (about 1 in 8). Making the MESH
  transfer-count register track programmed-I/O bytes made it more frequent, so the
  game's CD driver reads that register and the cause is likely in how the emulated
  controller reports transfers. Workaround: exit and relaunch.
- *Compton's Interactive Encyclopedia @World Edition* (USA beta, 1996-10-31) stops at its
  title screen and ignores input. It configures serial port B (the modem port) just
  before, so it most likely waits for a modem or online service, which is not emulated.
- The Taos dot clock for VGA output is unverified (NTSC is correct).
