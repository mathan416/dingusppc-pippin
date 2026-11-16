# How DingusPPC-Pippin works

This document explains the hardware inside an Apple/Bandai Pippin, how DingusPPC models it,
and what this fork adds to run it at full speed on a Raspberry Pi: the ARM64 JIT, mapping
guest memory into host memory, and the threading and timing that keep video and sound
right.

It's written for someone who knows roughly what a CPU emulator is but hasn't read this
codebase. Source files are named so you can follow along.

## Contents

1. [The Pippin hardware](#1-the-pippin-hardware)
2. [The PowerPC 603](#2-the-powerpc-603)
3. [The software the Pippin runs](#3-the-software-the-pippin-runs)
4. [DingusPPC: the base emulator](#4-dingusppc-the-base-emulator)
5. [Why the interpreter alone is too slow](#5-why-the-interpreter-alone-is-too-slow)
6. [How a guest instruction gets executed](#6-how-a-guest-instruction-gets-executed)
7. [The JIT: turning PowerPC into ARM64](#7-the-jit-turning-powerpc-into-arm64)
8. [Memory: the MMU, fastmem and the ROM window](#8-memory-the-mmu-fastmem-and-the-rom-window)
9. [Exceptions and interrupts](#9-exceptions-and-interrupts)
10. [Time: keeping the guest on schedule](#10-time-keeping-the-guest-on-schedule)
11. [Threads: CPU, display and sound](#11-threads-cpu-display-and-sound)
12. [Devices](#12-devices)
13. [Making sure it's right](#13-making-sure-its-right)
14. [Source map](#14-source-map)

## 1. The Pippin hardware

The Pippin (1995–1997) is a cut-down Power Macintosh built as a game and multimedia console.
Apple designed it and licensed it to Bandai, who sold it as the *Pippin Atmark* in Japan and
the *@WORLD* in the United States.

```
                      +-------------------------------+
                      |  PowerPC 603, 66 MHz          |
                      |  (integer, FPU, MMU, caches)  |
                      +---------------+---------------+
                                      | 60x processor bus
        +-----------------------------+-----------------------------+
        |  Aspen: memory controller and PCI host bridge             |
        +------+-----------------+--------------------+-------------+
               |                 |                    | PCI bus
        +------+------+   +------+------+    +--------+---------------------------+
        | RAM         |   | ROM 4 MB    |    | Grand Central I/O controller        |
        | 5 MB + slot |   | Mac OS      |    |  - interrupt controller             |
        +-------------+   | Toolbox     |    |  - DBDMA (DMA engine)               |
                          +-------------+    |  - MESH SCSI  --> 4x CD-ROM drive   |
        +---------------+                    |  - Curio SCSI (external)            |
        | Taos video    |                    |  - ESCC serial (2 ports)            |
        | + Athens clock|                    |  - AWACS sound (16-bit stereo)      |
        | NTSC/PAL/VGA  |                    |  - VIA + Cuda microcontroller       |
        +---------------+                    |     (ADB, clock, PRAM, power)       |
                                             +-------------------------------------+
                                                     | ADB
                                             AppleJack controller(s), keyboard
```

| Part | What it is | In the emulator |
| --- | --- | --- |
| PowerPC 603 | The CPU, 66 MHz | `cpu/ppc/` |
| Aspen | Memory controller and PCI host bridge, specific to the Pippin | `devices/memctrl/aspen.*` |
| RAM | 4 MB + 1 MB soldered, plus an expansion slot | `rambank1_size` to `rambank4_size` |
| ROM | 4 MB at `0xFFC00000`: start-up code, the Mac OS Toolbox and the 68K emulator | loaded from `kinka13.rom` |
| Flash | A small flash chip where games keep saves | `devices/memctrl/pippinflash.h` |
| Grand Central | Apple's I/O controller, shared with several Power Macs | `devices/ioctrl/grandcentral.cpp` |
| MESH | The internal SCSI controller the CD-ROM drive is on | `devices/common/scsi/mesh.*` |
| AWACS | Sound codec: 16-bit stereo in and out | `devices/sound/awacs.*` |
| VIA + Cuda | Timers, plus a microcontroller for ADB, the real-time clock, PRAM and power | `devices/common/viacuda.*` |
| Taos | Video controller: 640×480, 8-bit indexed or 16-bit colour, NTSC/PAL/VGA | `devices/video/taos.*` |
| AppleJack | ADB gamepad with a trackball | `devices/common/adb/adbapplejack.*` |

### Does the Pippin have coprocessors?

Not in the sense a modern console does. There's no graphics processor and no DSP: the CPU
draws every pixel into video memory, and Taos just scans it out to the TV. The chips that
do work on their own are:

- **The floating-point unit**, which is part of the 603 itself, not a separate chip.
- **The DBDMA engine** in Grand Central. The CPU writes a small list of commands ("move 8 KB
  from the CD controller to this RAM address, then interrupt me"), and the engine carries
  them out while the CPU does something else. It drives sound output and CD reads.
- **Cuda**, a small 68HC05-family microcontroller with its own firmware. It runs the ADB bus
  (controllers and keyboard), keeps the real-time clock and PRAM, and handles power. The
  emulator doesn't run Cuda's firmware; it reproduces Cuda's behaviour at the level of the
  commands the CPU sends it.
- **The CD-ROM drive** has its own controller and buffer, as every SCSI drive does.

AWACS is a codec (it converts samples to and from analog), not a sound processor: the CPU
mixes all audio itself.

## 2. The PowerPC 603

The 603 was the low-power member of the first PowerPC family, designed for laptops and
low-cost machines.

| Feature | 603 |
| --- | --- |
| Architecture | 32-bit PowerPC, big-endian (a little-endian mode exists but Mac OS doesn't use it) |
| Registers | 32 general-purpose (32-bit), 32 floating-point (64-bit), condition register (CR), link register (LR), count register (CTR), XER, and many special-purpose registers |
| Pipeline | Short pipeline, up to two instructions dispatched per cycle, five execution units: integer, floating point, branch, load/store and system register |
| Caches | 8 KB instruction + 8 KB data |
| MMU | Block address translation (BATs), 16 segment registers and a hashed page table. Unlike the 601 and 604, the 603 has no hardware table walker: TLB misses trap to a small software handler. |
| Timers | Time base (a 64-bit counter) and decrementer (counts down and interrupts at zero). The emulator runs them at 16.5 MHz. |

A few PowerPC features matter a lot for emulation:

- **Fixed-size instructions.** Every instruction is 32 bits and aligned, so finding
  instruction boundaries is trivial, unlike x86 or the 68K.
- **The condition register.** Compares write one of eight 4-bit fields in CR, and branches
  test a bit. Many integer instructions can also update CR field 0 (the `.` forms, such as
  `add.`).
- **Link and count registers.** Calls save the return address in LR; loops can count down
  in CTR and branch in a single instruction (`bdnz`).
- **Precise exceptions.** When an instruction faults, everything before it has completed
  and nothing after it has. An emulator has to preserve that exactly: Mac OS relies on it
  for page faults and for the 68K emulator's own tricks.
- **Big-endian memory.** The Pi's ARM cores run little-endian, so every multi-byte load or
  store has its bytes reversed on the way through (one ARM64 `REV` instruction).

The emulator models the 603's programmer-visible behaviour (registers, instructions, MMU,
exceptions), not its pipeline or caches. Instruction timing isn't cycle-exact; see
[Time](#10-time-keeping-the-guest-on-schedule).

## 3. The software the Pippin runs

A Pippin has no hard disk. Each game disc carries its own copy of a cut-down System 7.5
("Pippin OS"), which boots from the CD. The ROM holds the start-up code and the Mac OS
Toolbox (QuickDraw, the Sound Manager and so on).

The ROM also holds Apple's **68K emulator**, because most Mac software of the time, and
much of the Toolbox itself, was still written for the 68000-family processors of earlier
Macs. A 68K application (L-Zone is one) runs like this:

```
  68K game code
      |  interpreted by
      v
  Apple's 68K emulator  (PowerPC code in the Pippin ROM)
      |  emulated by
      v
  DingusPPC-Pippin      (PowerPC -> ARM64)
      |
      v
  Raspberry Pi's Cortex-A72 / A76
```

So for many titles, most guest PowerPC instructions are the 68K emulator's own loops, and
they all live in ROM. That shaped this fork's design: the JIT concentrates on ROM code,
which never changes, can be translated once, and is shared by every title.

## 4. DingusPPC: the base emulator

[DingusPPC](https://github.com/dingusdev/dingusppc) emulates several PowerPC Macintosh
models. This fork keeps its structure:

- **The interpreter** (`cpu/ppc/ppcexec.cpp`, `ppcopcodes.cpp`, `ppcfpopcodes.cpp`)
  fetches each instruction, looks up its handler in a table by opcode, and calls it. Every
  instruction, including floating point and system instructions, is implemented here. This
  is the reference for correct behaviour.
- **The MMU** (`cpu/ppc/ppcmmu.cpp`) translates guest addresses through the BATs, segment
  registers and page table, and caches the results in software TLBs: separate instruction
  and data TLBs, two levels each, and one set per MMU mode (real addressing, supervisor,
  user).
- **Devices** (`devices/`) are C++ objects mapped into the guest's physical address space.
  A load or store to a device address calls that device's `read` or `write` method.
- **The timer manager** (`core/timermanager.cpp`) holds every pending device event
  (vertical blank, the decrementer, a DMA completing) in guest time, and the CPU loop
  services it regularly.
- **The SDL front end** (`devices/video/display_sdl.cpp`, `core/hostevents_sdl.cpp`,
  `devices/sound/soundserver_cubeb.cpp`) shows the picture, plays sound through cubeb and
  reads input.

## 5. Why the interpreter alone is too slow

A real Pippin executes roughly 26 million PowerPC instructions per second in typical game
code; that's the figure the authentic-speed mode is calibrated to. On a Pi 4 the
interpreter manages a fraction of that. Each guest instruction costs a table lookup, an
indirect call, decoding the register fields, reading operands from memory, the operation,
writing results back, and a check for pending events. Each step is cheap, but together they
add up to dozens of host instructions for every guest one, and the indirect calls are hard
for the Pi's branch predictor.

The fork removes that overhead in layers, so the common case is fast while every rare
case still falls through to the interpreter, which remains the reference for correctness.

## 6. How a guest instruction gets executed

The main loop (`ppc_exec_inner` in `cpu/ppc/ppcexec.cpp`) tries the fastest path first and
falls back step by step:

```
  guest PC
     |
     v
  1. Steady inner loop        hand-written ARM64 loop that runs straight-line code and
     (sheep_cpu_loop_a64)     calls translated blocks without returning to C++
     |  (can't continue: page change, event due, unusual instruction)
     v
  2. ROM dispatch table       one 64-bit word per ROM instruction (1,048,576 words):
     (sheep_rom_dispatch_     either "call this translated block", a pre-decoded handler,
      words)                  or "don't try to translate here"
     |
     v
  3. Hot-block cache          65,536 slots of recently translated blocks
     (sheep_hot_slots)
     |  (miss)
     v
  4. Translate now            compile up to 32 instructions into ARM64 (section 7)
     (sheep_jit_try)
     |  (instruction not supported, or translation failed)
     v
  5. Interpreter              DingusPPC's handler for this one instruction, with a fast
                              path for simple branches and a compact decoder for ROM code
```

Some details that make this work:

- **Rejection with back-off.** If a spot can't be translated (it starts with an unsupported
  instruction, say), a per-address counter remembers that, so the loop doesn't retry the
  compiler on every visit. Some rejections are permanent; others expire so the spot is
  tried again later.
- **Budgets.** The loop knows how many instructions it may run before the next device event
  is due, and never lets a translated block run past it, so timers fire on time.
- **Pages.** Translation never crosses a 4 KB page boundary, because the next page might be
  mapped differently or not at all.

## 7. The JIT: turning PowerPC into ARM64

The translator (`cpu/ppc/nw_jit.cpp`) comes from Bill Cavalieri's ARM64 JIT for
SheepShaver, a Mac emulator. `cpu/ppc/sheep_jit_bridge.cpp` connects it to DingusPPC's CPU
state, MMU and execution loop.

### What a translated block looks like

A block is a straight run of up to 32 guest instructions, ending at a branch, an `isync`,
an instruction the translator doesn't handle, or the end of the page. Each guest
instruction becomes a few ARM64 instructions. Guest registers live in DingusPPC's CPU-state
structure in memory, and a host register points at it. For example, `addi r3,r3,1` becomes
roughly:

```
  ldr  w9, [x0, #offset_of_r3]     ; load guest r3
  add  w9, w9, #1                  ; the operation
  str  w9, [x0, #offset_of_r3]     ; store guest r3
```

That's a *template* translator: each guest instruction maps to a fixed pattern of host
instructions. It doesn't allocate guest registers to host registers across a block. That
keeps it simple and exact, and the main costs it removes (dispatch, decoding and indirect
calls) are most of what made the interpreter slow.

### What it translates

Integer arithmetic and logic, rotates and shifts, compares and condition-register
operations, loads and stores of bytes, halfwords and words (including the update and
indexed forms), branches (relative, through LR and CTR, conditional and counting), and a
few special-purpose registers (LR, CTR, XER and, in supervisor mode, SRR0/SRR1).

Floating point, cache-control and most system instructions run in the interpreter. They're
rare in the hot paths of Pippin titles.

### Getting faults exactly right

A translated load or store can fault: a page isn't mapped, or the address belongs to a
device. The block then stops *before* that instruction and reports how many instructions
it completed. The CPU state is exactly what it would be after running those instructions
one by one, and the faulting instruction is re-run by the interpreter, which raises the
exception exactly as DingusPPC always has. This is how translated code keeps the
PowerPC's precise exceptions.

### Where translated code is kept

- A 16 MB code area, split into two 8 MB banks. When the active bank fills, the other is
  wiped and reused, and every translation in it is forgotten (and retranslated if needed).
- Each bank is divided into 1 MB **islands**. A block never crosses an island, so its
  constants and table addresses can sit at the island's start, within reach of a single
  ARM64 load instruction.
- **Island packing** (`SHEEP_PIPPIN_ISLAND_PACK=on`) puts each block's frequently used path
  in the first half of an island and its rarely used paths (fault handling, fallbacks) in
  the second half, so hot code is packed densely in the Pi's small instruction cache.

### Why ROM code is special

The Pippin's ROM can never change, so a translation of ROM code is valid forever, as long
as the ROM is mapped at the same guest address. The fork exploits that heavily:

- `SHEEP_PIPPIN_ROM_ONLY=1` translates only ROM code. Code in RAM can be overwritten
  (the guest loads programs, and the 68K emulator patches itself), and tracking that
  safely costs more than it saves on the Pippin.
- The ROM dispatch table has a word for every ROM instruction, so finding the translation
  for a ROM address is a single array lookup.
- RAM code still benefits: the steady inner loop interprets it with far less overhead than
  the general loop.

## 8. Memory: the MMU, fastmem and the ROM window

### The problem

Every guest load or store uses an *effective address* that the MMU translates into a
*physical* address, which is then RAM, ROM or a device. Doing that in C++ for every access
is slow: a TLB lookup, a check for the region type, then the access with a byte swap.

### Fastmem

Fastmem (`cpu/ppc/pippin_fastmem.cpp`) uses the Pi's own MMU to do the guest's address
translation:

```
  Host virtual address space (reserved up front, nothing mapped yet)

  +-----------------+-----------------+-----------------+-----------------+
  | real-mode reads | real-mode writes| supervisor reads| supervisor ...  |  ...
  |   4 GB window   |   4 GB window   |   4 GB window   |                 |
  +-----------------+-----------------+-----------------+-----------------+
           |
           |  guest page 0x00123000 is RAM at offset 0x123000:
           |  map that one 4 KB page of the guest-RAM file into the window
           v
  window_base + 0x00123000  --->  the same physical memory as guest RAM
```

- Guest RAM is allocated in a memory file (`memfd`), so the same physical pages can appear
  at several host addresses at once.
- For each MMU mode (real addressing, supervisor and user, each split into a read window
  and a write window) the emulator reserves 4 GB of host address space, one byte per
  possible guest address. Nothing is mapped at first.
- When the guest's own TLB says "this guest page is that RAM page", the matching 4 KB page
  of the window is mapped onto the RAM file. Writes go through the write window only if
  the page is writable for the guest.
- A translated load is then one ARM64 instruction, `ldr w9, [window_base, guest_address]`,
  plus a byte swap.
- If the page isn't mapped (never touched, a device, or protected), the Pi raises a
  segmentation fault. The fork's signal handler checks the fault came from a known spot in
  translated code, and resumes at that spot's slow path, which goes through DingusPPC's
  normal MMU and devices. If the address turns out to be a device or unmapped (rather than
  RAM that just isn't in the window yet), the JIT patches that instruction to always take
  the slow path from then on.
- When the guest changes its page tables (`tlbie`, segment or BAT changes), the affected
  window pages are unmapped again.

Device registers are never mapped into a window, so every device access still reaches the
device's C++ code, in order. Fastmem needs 4 KB host pages because guest pages are 4 KB;
on a kernel with 16 KB pages (the Pi 5's default) it switches itself off.

### The ROM window

Before running a translated ROM block, the loop must know the guest's current instruction
translation still maps that address to the ROM. Checking the TLB every time is costly. The
**ROM window** (`SHEEP_PIPPIN_ROM_WINDOW=on`) records a range of guest addresses already
proven to map straight onto ROM, and stays valid until something could change that: an
MMU mode switch, a page-table change, a BAT write. Most exceptions and returns from
exceptions leave instruction translation alone, so the window survives them.

### Precise `tlbie`

When the guest invalidates one page translation with `tlbie`, upstream DingusPPC flushes its
whole TLB. The 603 only drops the entries for that page's congruence class (a small set of
TLB slots), so the fork does the same: correct, and much less work for the TLB, fastmem
and the ROM window afterwards.

## 9. Exceptions and interrupts

- **Synchronous exceptions** (page faults, illegal instructions, system calls) happen in the
  interpreter, as in upstream DingusPPC. Translated code hands over at the exact faulting
  instruction (section 7).
- **Interrupts** (the decrementer, Grand Central's interrupt line for VBL, DMA, SCSI,
  sound and so on) are checked between instructions or blocks, when the loop services
  pending events. Blocks are short and bounded by the event budget, so an interrupt is
  never delayed by more than a block.
- **Returning from exceptions** (`rfi`) and changes to the MSR switch the MMU mode, which
  selects a different TLB set, fastmem window and possibly ROM window.

## 10. Time: keeping the guest on schedule

An emulator has two clocks: how many guest instructions have run, and how much real time
has passed. DingusPPC's default derives guest time from the instruction count. That's
deterministic, but a JIT that runs code at a different speed from real hardware makes
music and animation drift.

- **`--realtime`** makes guest time follow the host's clock. The decrementer, time base,
  vertical blank and every device timer then fire on real time, so music plays at the
  right tempo and pitch whatever speed the CPU runs at. Every Pippin configuration uses it.
- **Polling.** The loop services due events at least every 0.5 ms of guest time, and sizes
  each batch of instructions to reach the next event on time.
- **Video timing.** Taos produces vertical blank at 59.94 Hz for NTSC (fixed in this fork;
  upstream ran at about 35 Hz), driven by the guest-time timer.
- **Authentic-speed mode** adds a governor (`cpu/ppc/pippin_authentic.cpp`): when the CPU
  gets ahead of 26 million instructions per second of real time, it sleeps briefly. It
  doesn't touch any timer, so the game's own timing is unaffected; only how much work it
  gets done per second changes, as on real hardware.

## 11. Threads: CPU, display and sound

With `SHEEP_CPU_OWNER=on`, the work is split across threads:

```
  CPU thread ("pippin-cpu")              Main thread ("pippin-sdl")
  ---------------------------            ---------------------------
  interpreter + JIT                      SDL window, rendering
  MMU, devices, timers                   input events (keyboard, gamepads, mouse)
  builds each video frame                presents frames
        |                                       ^
        |  frame mailbox (newest frame wins) ---+
        |
        |  <--- input queue (button and key events, in order)
        |
        +--> audio queue ---> cubeb audio thread ---> speakers
```

- **The CPU thread owns all guest state.** Only it touches CPU registers, guest memory and
  device objects, so none of them need locks.
- **Frames.** At each vertical blank the CPU thread snapshots the framebuffer and posts it
  to a mailbox. With `SHEEP_RAW_FRAME=on` the snapshot is the raw 8-bit or 15-bit pixels
  plus the palette, and the main thread converts it to RGB. If the main thread falls
  behind, an unpresented frame is replaced by the newer one rather than queued, so video
  never lags.
- **Input** events are queued from the main thread to the CPU thread, which delivers them to
  the emulated AppleJack and keyboard in order. Nothing is dropped.
- **Sound.** The guest's DMA writes samples into a queue; cubeb's audio thread plays them.
  With `DINGUS_AUDIO_GUEST_SYNC=1` playback keeps about 75 ms buffered and adjusts its
  rate slightly to follow the guest, which avoids both gaps and drift.
- **Shutdown** is orderly: a quit request stops the CPU thread at the next safe point, then
  the main thread finishes any frames still posted, and saves flash and NVRAM.

## 12. Devices

What the fork changed, in addition to the upstream device models:

- **MESH SCSI and DMA**: a DMA read from the controller reports the bytes it actually
  delivered and keeps leftover FIFO bytes in order (upstream reported 0).
- **CD-ROM timing model** (authentic mode, off by default): delays data in guest time as a
  4× drive would (seek, rotation, transfer rate and read-ahead buffer).
- **Taos**: half-line accurate timing, VBL per field, the Athens clock crystal.
- **AppleJack**: trigger-only changes are reported, and SDL game controllers are mapped to
  it (see [controllers.md](controllers.md)).
- **Flash**: Pippin flash saves are written to `pippin-flash.bin`.
- **Sound**: cubeb output with guest pacing (above).

## 13. Making sure it's right

A JIT that's fast but subtly wrong is worse than useless, so correctness checking is built
in:

- **Verify mode** (`SHEEP_PIPPIN_JIT=verify`) runs every translated block on a copy of the
  CPU state, then runs the same instructions through DingusPPC's interpreter from the same
  starting point, and compares the registers (general-purpose, CR, XER, LR, CTR, SRR0/1,
  PC) and any memory written. A difference is logged with the instructions involved, and
  the emulator stops.
  Release builds are checked this way on real games: v1.0.0 ran over 2 billion verified
  instructions across the Pi 4 and Pi 5 with no mismatches.
- **Start-up self-tests** run before any guest code, every time the JIT is on. They compile
  and run fixtures covering fastmem faults, branch forms, the ROM dispatcher, literal
  islands, clock handling and more. If any fail, the emulator stops with an error rather
  than run a JIT it can't trust.
  `--jit-selftest` runs only these.
- **Unit tests** for timing, pointer and boundary logic (`pippin/support/test-*.cpp`, built
  with `-DDPPC_BUILD_PIPPIN_TESTS=ON`).
- **CI** builds on a GitHub ARM64 runner and runs all of the above on every push.

## 14. Source map

| Area | Files |
| --- | --- |
| Main loop, dispatch, timing | `cpu/ppc/ppcexec.cpp` |
| Interpreter instructions | `cpu/ppc/ppcopcodes.cpp`, `ppcfpopcodes.cpp` |
| MMU, TLBs, ROM window, precise `tlbie` | `cpu/ppc/ppcmmu.cpp`, `ppcmmu.h` |
| ARM64 translator (from SheepShaver) | `cpu/ppc/nw_jit.cpp`, `nw_jit.h`, `nw_io.*` |
| Bridge between translator and DingusPPC | `cpu/ppc/sheep_jit_bridge.*`, `sheep_jit_abi.cpp` |
| Fastmem | `cpu/ppc/pippin_fastmem.*` |
| Authentic-speed mode | `cpu/ppc/pippin_authentic.*` |
| Self-test fixtures | `cpu/ppc/*-tests.inc` and the other `.inc` files |
| CPU thread and queues | `main.cpp`, `main_cpu_owner.h` |
| Display, frames | `devices/video/display_sdl.cpp`, `frame_pixel_pool.h`, `indexed_snapshot.h`, `taos.*` |
| Input | `core/hostevents_sdl.cpp`, `core/applejack_button_*.h`, `devices/common/adb/adbapplejack.*` |
| Sound | `devices/sound/soundserver_cubeb.cpp`, `guestpace.h`, `guestpcm*.h` |
| SCSI, CD-ROM | `devices/common/scsi/` |
| Pippin machine | `machines/machinepippin.cpp`, `devices/memctrl/aspen.*`, `pippinflash.h` |
| Build and launch | `pippin/build.sh`, `pippin/run.sh` |

The history of every change, with measurements, is in [PIPPIN.md](../PIPPIN.md).
