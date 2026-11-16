# DingusPPC-Pippin

A fork of [DingusPPC](https://github.com/dingusdev/dingusppc) for running the
Apple/Bandai Pippin at full speed on a Raspberry Pi 4, with an ARM64 JIT
from the SheepShaver lineage.

## Provenance

| Part | Origin | Licence |
| --- | --- | --- |
| Emulator core: machine models (incl. Pippin), devices, MMU, interpreter, main loop | DingusPPC, branched from `dingusdev/dingusppc@810c3f9` (2026-10-28) | GPL-3.0-or-later |
| `cpu/ppc/nw_jit.*`, `cpu/ppc/nw_io.*`: New World ARM64 JIT | SheepShaver lineage, © 2026 Bill Cavalieri; SheepShaver © 1997-2008 Christian Bauer and Marc Hellwig | GPL-2.0-or-later |
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
| Q224a | Build: `-mcpu=cortex-a72`, single-partition LTO (see `pippin/pi4-build/`) | L-Zone +2.5%, Tamagotchi +0.9% |
| Q226 | Read the exec-entry `getenv` lookups once | part of Q227 |
| Q227 | Precise `tlbie` (congruence class), ROM window kept across same-mode `mmu_change_mode` | L-Zone +7.8%, Tamagotchi +2.2%; fastmem remaps −80% |
| Q233 | Taos VBL at 59.94 Hz (half-line timing decode, 17.075 MHz Athens crystal) | Refresh 35.2 → 59.94 Hz; about −3% CPU |
| Q234 | Optional authentic-speed mode (off by default): CPU governor and CD-ROM timing model | Default unchanged (+0.7%, noise); L-Zone intro at 95% of a real Pippin with the preset |

On the L-Zone intro, the CPU-bound fly-in runs at about 146% of a real
Pippin. Timer- and audio-paced content runs at true speed under
`--realtime`: music pitch and tempo match real hardware.

## Building and running

See `pippin/pi4-build/README.md`. Typical cabinet invocation:

    pippin-v233 -r --realtime --start-date=1997-06-01 -m pippin -b kinka13.rom \
      --cdr_config=ScsiMesh:3 --cdr_img2=disc.bin --rambank3_size=8 \
      --adb_devices=AppleJack,Keyboard

Runtime policy environment, as deployed: `SHEEP_PIPPIN_JIT=on
SHEEP_PIPPIN_FASTMEM=on SHEEP_PIPPIN_ROM_ONLY=1 SHEEP_CPU_OWNER=on
SHEEP_RAW_FRAME=on SHEEP_PIPPIN_ROM_WINDOW=on SHEEP_PIPPIN_ISLAND_PACK=on`.
`SHEEP_PIPPIN_JIT=verify` runs the startup self-tests and the interpreter oracle.

## Authentic-speed mode (Q234)

Off by default; acts only under `--realtime`. Guest timers, video and audio are untouched.

| Variable | Meaning |
| --- | --- |
| `SHEEP_PIPPIN_AUTHENTIC=on` | Calibrated preset: 26 MIPS CPU cap and a 4× CD (600 KiB/s, 180 ms average access, 50 ms minimum, 256 KiB buffer) |
| `SHEEP_PIPPIN_MAX_MIPS=N\|off` | CPU cap in million guest instructions per second |
| `SHEEP_PIPPIN_CD_SPEED=Nx\|KiB/s\|off` | CD transfer rate (1× = 150 KiB/s) |
| `SHEEP_PIPPIN_CD_SEEK_MS`, `SHEEP_PIPPIN_CD_SEEK_MIN_MS`, `SHEEP_PIPPIN_CD_READAHEAD_KB` | CD access-time model and drive buffer |

The cabinet's `pippin-authentic-speed` launcher entry uses `SHEEP_PIPPIN_AUTHENTIC=on
SHEEP_PIPPIN_CD_SPEED=off` (CPU cap only): with the CD model on, the known L-Zone intro
stall occurred in 2 of 17 runs.

## Candidates for upstream DingusPPC

- Taos timing: decode half-lines, VBL per field, Athens crystal.
- Precise `tlbie` instead of a full PAT flush.
- Reading environment settings once at exec entry.

## Known issues

- The L-Zone intro occasionally (~2%) stays black after a CD access at
  about 37 s. It also occurs on the Q219 baseline; the cause is not yet known.
- The Taos dot clock for VGA output is unverified (NTSC is correct).
