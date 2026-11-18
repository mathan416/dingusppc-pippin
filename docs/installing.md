# Building and installing

How to get DingusPPC-Pippin onto a Raspberry Pi 4 or 5, either on its own or as an emulator
in RetroPie. It works on two kinds of system:

- **Fully 64-bit:** 64-bit Raspberry Pi OS (Bookworm or newer), with or without RetroPie on
  top. The emulator runs directly.
- **64-bit kernel, 32-bit userland:** for example the official RetroPie image on a Pi 4 with
  a 64-bit kernel. The emulator is a 64-bit program, so it runs inside a small 64-bit
  *runtime*: Canonical's Ubuntu Base 24.04 with the libraries it needs, used as a chroot.
  It isn't a virtual machine. The emulator runs natively on the same kernel at full speed;
  only its libraries come from the runtime. The runtime takes about 650 MB, or about
  1.1 GB with the build tools.

## Contents

- [Which kind of system do you have?](#which-kind-of-system-do-you-have)
- [Step 1: get the emulator](#step-1-get-the-emulator)
- [Step 2a: run it on its own](#step-2a-run-it-on-its-own)
- [Step 2b: install it into RetroPie](#step-2b-install-it-into-retropie)
- [Where everything goes](#where-everything-goes)
- [Troubleshooting](#troubleshooting)

## Which kind of system do you have?

```
uname -m                       # aarch64 = 64-bit kernel; armv7l = 32-bit kernel
ls /lib/ld-linux-aarch64.so.1  # present = 64-bit userland; missing = 32-bit userland
```

| `uname -m` | 64-bit loader | You have | What to do |
| --- | --- | --- | --- |
| `aarch64` | present | Fully 64-bit | Everything works directly. |
| `aarch64` | missing | 64-bit kernel, 32-bit userland | Works through the runtime. The scripts set it up. |
| `armv7l` | missing | 32-bit kernel | Switch to the 64-bit kernel first (below), then it's the row above. |

**Switching a Pi 4 to the 64-bit kernel:** add `arm_64bit=1` to `/boot/config.txt` (on
newer systems, `/boot/firmware/config.txt`) and reboot. Nothing else on the system changes:
your 32-bit programs keep working.

**Pi 5:** use 64-bit Raspberry Pi OS Bookworm and switch to the 4 KB-page kernel (see the
[README](../README.md#raspberry-pi-5)).

## Step 1: get the emulator

### Download a release

The [Releases](https://github.com/mathan416/dingusppc-pippin/releases) page has a build
tuned for the Pi 4 and one for the Pi 5. Either kind of system can use them.

```
V=v1.1.0  # the release you want
PI=pi4    # or pi5
wget https://github.com/mathan416/dingusppc-pippin/releases/download/$V/dingusppc-pippin-$V-$PI.tar.gz
tar xf dingusppc-pippin-$V-$PI.tar.gz
cd dingusppc-pippin-$V-$PI
```

That folder holds the emulator, `run.sh`, the disc helper, the runtime scripts, the
RetroPie installer and the documentation.

### Or build it yourself

```
git clone --recursive -b pippin https://github.com/mathan416/dingusppc-pippin
cd dingusppc-pippin
```

On a fully 64-bit system, install the build tools first:

```
sudo apt install cmake libsdl2-dev
```

Then, on either kind of system:

```
pippin/build.sh
pippin/package.sh
```

`pippin/build.sh` works out which kind of system it's on. On a fully 64-bit system it
builds directly. On a 64-bit kernel with a 32-bit userland it creates a build runtime in
`build-runtime/` the first time (it downloads Ubuntu Base 24.04.5, checks its SHA-256 and
installs the compiler and headers into it, about 1.1 GB), then builds inside it. That needs
`sudo`, and nothing is installed on the system itself; you don't need the `apt install`
step.

`pippin/package.sh` then collects the result in `dist/dingusppc-pippin`, the same
folder a release unpacks to.

Building takes under 3 minutes on a Pi 5 and about 13 minutes on a Pi 4 (including
setting up the build runtime the first time), because the CPU loop is built with
whole-program optimisation. Set `PIPPIN_JOBS=2` on a Pi 4 with 2 GB of RAM or
less.

## Step 2a: run it on its own

```
cd dingusppc-pippin
./run.sh /path/to/kinka13.rom "/path/to/Game (Japan).cue"
```

The disc can be a `.cue` (with its `.bin` files), a `.chd` or a raw `.bin`/`.iso`. See
[configuration.md](configuration.md#the-launcher-runsh) for every option.

**64-bit kernel, 32-bit userland:** create the runtime once, inside the package folder:

```
sudo runtime/make-runtime.sh runtime/root
```

After that, `run.sh` notices the 32-bit userland and runs the emulator inside the runtime
by itself. It asks for `sudo` to do so; RetroPie's `pi` user doesn't need a password.

## Step 2b: install it into RetroPie

Run the installer from the package folder, as your normal RetroPie user (not with `sudo`):

```
cd dingusppc-pippin
retropie/install.sh
```

It:

- copies the package to `/opt/retropie/emulators/dingusppc-pippin`;
- on a 32-bit userland, creates the runtime there (a one-time download; about 1.5 minutes
  on a Pi 4);
- adds two emulators to `/opt/retropie/configs/pippin/emulators.cfg`: `dingusppc-pippin`
  (full speed) and `dingusppc-pippin-authentic` (a real Pippin's CPU speed). The first
  install becomes the default; you can pick the other in RetroPie's runcommand menu;
- adds a Bandai Pippin system to EmulationStation if there isn't one yet (in
  `/opt/retropie/configs/all/emulationstation/es_systems.cfg`);
- creates `~/RetroPie/roms/pippin` and `~/RetroPie/saves/pippin`;
- checks for the BIOS.

Then:

1. Copy your Pippin ROM to `~/RetroPie/BIOS/kinka13.rom` (Kinka 1.3 is the tested version).
2. Put games in `~/RetroPie/roms/pippin`, as `.cue`/`.bin`, `.chd` or `.iso`. Games can sit
   in their own subfolders. CHD needs `chdman`: `sudo apt install mame-tools`. The first
   launch of a CUE or CHD game copies its data track to the cache; a CHD takes about half a
   minute on a Pi 4. Later launches start straight away.
3. Restart EmulationStation.

Gamepads, trackballs and keyboards work as described in [controllers.md](controllers.md).

**Updating:** unpack the new release or build a new package and run `retropie/install.sh`
again. The runtime and your saves are kept.

**Removing:** `retropie/install.sh --uninstall` removes the emulator and the menu entries it
added. Your games, saves and BIOS stay.

**Options:**

| Option | Meaning |
| --- | --- |
| `--package DIR` | Install a different package folder (default: the one the script is in) |
| `--name NAME` | Install as `/opt/retropie/emulators/NAME`, with menu entries `NAME` and `NAME-authentic` |
| `--no-menu` | Install the emulator only, without menu entries or the Pippin system. Useful for testing a build next to an existing install; start it with `/opt/retropie/emulators/NAME/launch.sh DISC` |
| `--uninstall` | Remove what the installer added under `NAME` |

## Where everything goes

| What | Where |
| --- | --- |
| Emulator, launchers, runtime | `/opt/retropie/emulators/dingusppc-pippin/` |
| Menu entries | `/opt/retropie/configs/pippin/emulators.cfg` |
| BIOS | `~/RetroPie/BIOS/kinka13.rom` |
| Games | `~/RetroPie/roms/pippin/` |
| Saves | `~/RetroPie/saves/pippin/<id>/`, one folder per game; `<id>` comes from the disc's path, so moving or renaming a game starts a fresh save |
| Unpacked disc data | `~/.cache/dingusppc-pippin/discs/<id>/` (rebuilt when needed; safe to delete) |

## Troubleshooting

- **"needs a 64-bit (aarch64) kernel":** add `arm_64bit=1` to `config.txt` and reboot (see
  above).
- **"runs inside a 64-bit runtime ... Create it once":** run the `make-runtime.sh` command
  the message shows, or install through `retropie/install.sh`, which does it for you.
- **sudo asks for a password every time:** running in the runtime needs root to set up its
  private mounts. Give your user passwordless sudo for
  `/opt/retropie/emulators/dingusppc-pippin/runtime/enter.sh`, or run on a fully 64-bit
  system.
- **"CHD discs need chdman":** `sudo apt install mame-tools`, or use the CUE/BIN version.
- **Black screen or "SDL_Init error" without a desktop:** don't set
  `SDL_KMSDRM_DEVICE_INDEX`; SDL finds the display itself, and the card numbers can change
  between boots.
