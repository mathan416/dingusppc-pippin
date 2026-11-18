#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/retropie/install.sh
# Purpose: Install a built DingusPPC-Pippin package into RetroPie.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-18 - Added: RetroPie installer for fully 64-bit and 32-bit-userland systems.
# Full history: PIPPIN.md and Git history.
#
# Usage: retropie/install.sh [options]        (run as the RetroPie user, not with sudo)
#   --package DIR   the built package to install (default: the package this script is in,
#                   that is, an unpacked release or the output of pippin/package.sh)
#   --name NAME     install as /opt/retropie/emulators/NAME (default: dingusppc-pippin);
#                   the menu entries are NAME and NAME-authentic
#   --no-menu       install the emulator only: no emulator entries and no Pippin system
#                   (for testing alongside another install)
#   --uninstall     remove the emulator and the menu entries this script added under NAME;
#                   games, saves and the BIOS are kept
#
# What it does:
#   - copies the package to /opt/retropie/emulators/NAME (owned by root);
#   - on a 64-bit kernel with a 32-bit userland (the official RetroPie image), creates the
#     64-bit runtime there (downloads Ubuntu Base 24.04.5, about 650 MB installed);
#   - adds the emulator entries to /opt/retropie/configs/pippin/emulators.cfg (the first
#     install becomes the default) and, if EmulationStation has no Pippin system yet, adds
#     one to /opt/retropie/configs/all/emulationstation/es_systems.cfg;
#   - creates ~/RetroPie/roms/pippin and ~/RetroPie/saves/pippin (one folder per game) and
#     checks for the BIOS at ~/RetroPie/BIOS/kinka13.rom.
set -euo pipefail

die() { echo "install.sh: $*" >&2; exit 1; }
note() { echo "  $*"; }
here=$(cd "$(dirname "$0")" && pwd)
package=$(cd "$here/.." && pwd); name=dingusppc-pippin; menu=1; uninstall=0
while [ $# -gt 0 ]; do
  case $1 in
    --package) [ $# -ge 2 ] || die "--package needs a folder"; package=$(cd "$2" && pwd) || exit 1; shift 2 ;;
    --name) [ $# -ge 2 ] || die "--name needs a value"; name=$2; shift 2 ;;
    --no-menu) menu=0; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '/^# Usage/,/^set -euo/{/^set/d;s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
[[ $name =~ ^[a-z0-9][a-z0-9._-]*$ ]] || die "--name may only use a-z, 0-9, '.', '_' and '-'"
[ "$(id -u)" != 0 ] || die "run as the RetroPie user (normally pi), not with sudo; it asks for sudo itself"
[ -d /opt/retropie ] && [ -d "$HOME/RetroPie" ] || die "RetroPie not found (/opt/retropie and ~/RetroPie)"

dest=/opt/retropie/emulators/$name
cfg=/opt/retropie/configs/pippin/emulators.cfg
es_user=/opt/retropie/configs/all/emulationstation/es_systems.cfg
es_global=/etc/emulationstation/es_systems.cfg
manifest=$dest/.retropie-install

# emulators.cfg and es_systems.cfg edits. Our system block is wrapped in marker comments so
# it can be removed exactly.
edit_config() {
  python3 - "$@" <<'PY'
import os, re, sys
action, cfg, name, dest = sys.argv[1:5]
keys = {name: dest + '/launch.sh %ROM%', name + '-authentic': dest + '/launch-authentic.sh %ROM%'}
lines = open(cfg).read().splitlines() if os.path.exists(cfg) else []
entry = re.compile(r'^\s*([\w.-]+)\s*=\s*"(.*)"\s*$')
def key(line):
    m = entry.match(line)
    return m.group(1) if m else None
default = {key(l): l for l in lines}.get('default')
ours_default = default is not None and entry.match(default).group(2) == name
# Drop our own entries (and our default) before re-adding or removing them.
out = [l for l in lines if key(l) not in keys and not (key(l) == 'default' and ours_default)]
has_default = any(key(l) == 'default' for l in out)
if action == 'add':
    at = next((i for i, l in enumerate(out) if key(l) == 'default'), len(out))
    out[at:at] = ['%s = "%s"' % kv for kv in keys.items()]
    if not has_default:
        out.append('default = "%s"' % name)
        print('default')
elif ours_default:
    rest = [key(l) for l in out if key(l)]
    if rest:
        out.append('default = "%s"' % rest[0])
if out:
    open(cfg, 'w').write('\n'.join(out) + '\n')
elif os.path.exists(cfg):
    os.remove(cfg)
PY
}

edit_systems() {
  python3 - "$@" <<'PY'
import os, re, sys
action, es_user, es_global, name = sys.argv[1:5]
home = os.path.expanduser('~')
begin, end = '<!-- %s: begin -->' % name, '<!-- %s: end -->' % name
def read(p):
    return open(p, encoding='utf-8').read() if os.path.exists(p) else ''
if action == 'check':
    if begin in read(es_user):
        print('ours')
        sys.exit()
    for path in (es_user, es_global):
        text = read(path)
        if text:
            print('present' if re.search(r'<name>\s*pippin\s*</name>', text) else 'absent')
            break
    else:
        print('absent')
    sys.exit()
if action == 'add':
    text = read(es_user) or read(es_global) or '<?xml version="1.0"?>\n<systemList>\n</systemList>\n'
    block = '''  %s
  <system>
    <name>pippin</name>
    <fullname>Bandai Pippin</fullname>
    <path>%s/RetroPie/roms/pippin</path>
    <extension>.cue .CUE .chd .CHD .iso .ISO</extension>
    <command>/opt/retropie/supplementary/runcommand/runcommand.sh 0 _SYS_ pippin %%ROM%%</command>
    <platform>pippin</platform>
    <theme>pippin</theme>
  </system>
  %s
''' % (begin, home, end)
    if '</systemList>' not in text:
        sys.exit('es_systems.cfg has no </systemList>')
    text = text.replace('</systemList>', block + '</systemList>', 1)
    os.makedirs(os.path.dirname(es_user) or '.', exist_ok=True)
    open(es_user, 'w', encoding='utf-8').write(text)
else:
    text = read(es_user)
    if begin in text:
        text = re.sub(r'[ \t]*' + re.escape(begin) + r'.*?' + re.escape(end) + r'\n?', '', text, flags=re.S)
        open(es_user, 'w', encoding='utf-8').write(text)
PY
}

if ((uninstall)); then
  [ -f "$manifest" ] || die "$dest was not installed by this script"
  # shellcheck disable=SC1090
  . "$manifest"
  if [ "${added_menu:-0}" = 1 ]; then edit_config remove "$cfg" "$name" "$dest"; echo "Removed the $name entries from $cfg"; fi
  if [ "${added_system:-0}" = 1 ]; then edit_systems remove "$es_user" "$es_global" "$name"; echo "Removed the Pippin system from $es_user"; fi
  sudo rm -rf "$dest"
  echo "Removed $dest. Games, saves and the BIOS in ~/RetroPie were left in place."
  exit 0
fi

# Check the package and the system.
for f in dingusppc run.sh prepare-disc.py runtime/make-runtime.sh runtime/enter.sh; do
  [ -e "$package/$f" ] || die "$package is not a DingusPPC-Pippin package (missing $f)"
done
[ "$(uname -m)" = aarch64 ] || die "this system runs a $(uname -m) kernel; DingusPPC-Pippin needs a 64-bit kernel.
On a Raspberry Pi 4, add 'arm_64bit=1' to /boot/config.txt and reboot, then run this again."
command -v python3 >/dev/null || die "python3 is needed (sudo apt install python3)"
if [ -e /lib/ld-linux-aarch64.so.1 ]; then mode=64-bit; else mode=runtime; fi
if [ -e "$dest" ] && [ ! -f "$manifest" ]; then die "$dest exists but was not installed by this script; leaving it alone"; fi

echo "Installing DingusPPC-Pippin ($(sed -n 's/^version=//p' "$package/VERSION" 2>/dev/null || echo unknown)) to $dest"
if [ "$mode" = 64-bit ] && ! /sbin/ldconfig -p | grep -q 'libSDL2-2.0.so.0 (.*AArch64'; then
  echo "Installing SDL2..."
  sudo apt-get install -y libsdl2-2.0-0
fi

# Copy the package, keeping an existing runtime so a reinstall doesn't download it again.
keep=
if [ -d "$dest/runtime/root" ]; then keep=$(sudo mktemp -d /opt/retropie/emulators/.keep-XXXXXX); sudo mv "$dest/runtime/root" "$keep/root"; fi
sudo rm -rf "$dest"
sudo mkdir -p "$dest"
(cd "$package" && tar --exclude=./runtime/root -cf - .) | sudo tar -xf - -C "$dest" --no-same-owner
if [ -n "$keep" ]; then sudo mv "$keep/root" "$dest/runtime/root"; sudo rmdir "$keep"; fi

if [ "$mode" = runtime ]; then
  if [ -f "$dest/runtime/root/.dingusppc-pippin-runtime" ]; then
    echo "Updating the 64-bit runtime's packages..."
  else
    echo "This system has a 32-bit userland: creating the 64-bit runtime (one-time download)..."
  fi
  sudo "$dest/runtime/make-runtime.sh" "$dest/runtime/root"
fi

# RetroPie launchers: the BIOS from ~/RetroPie/BIOS, one save folder per game.
bios=$HOME/RetroPie/BIOS/kinka13.rom; saves=$HOME/RetroPie/saves/pippin
for variant in "" -authentic; do
  setting=; [ -n "$variant" ] && setting=" PIPPIN_AUTHENTIC=on"
  printf '#!/bin/bash\n# Written by retropie/install.sh: the RetroPie launcher for %s%s.\nexec env PIPPIN_SAVES=%q%s %q %q "$1"\n' \
    "$name" "$variant" "$saves" "$setting" "$dest/run.sh" "$bios" | sudo tee "$dest/launch$variant.sh" >/dev/null
  sudo chmod 755 "$dest/launch$variant.sh"
done
mkdir -p "$HOME/RetroPie/roms/pippin" "$saves" "$HOME/RetroPie/BIOS"

added_menu=0; added_system=0
if ((menu)); then
  mkdir -p "$(dirname "$cfg")"
  if [ -n "$(edit_config add "$cfg" "$name" "$dest")" ]; then note "$name is the default Pippin emulator"; fi
  added_menu=1
  note "added $name and $name-authentic to $cfg"
  case $(edit_systems check "$es_user" "$es_global" "$name") in
    absent) edit_systems add "$es_user" "$es_global" "$name"; added_system=1
            note "added the Pippin system to $es_user" ;;
    ours) edit_systems remove "$es_user" "$es_global" "$name"   # refresh our own entry
          edit_systems add "$es_user" "$es_global" "$name"; added_system=1 ;;
    *) note "EmulationStation already has a Pippin system; left it as it is" ;;
  esac
fi
printf 'name=%q\nadded_menu=%s\nadded_system=%s\n' "$name" "$added_menu" "$added_system" | sudo tee "$manifest" >/dev/null

echo "Installed ($mode)."
if [ -f "$bios" ]; then
  if [ "$(sha1sum "$bios" | cut -d' ' -f1)" = 8e512af6e34dd823f3defec77d43ecbff1ecad54 ]; then note "BIOS: $bios (Kinka 1.3, the tested version)"
  else note "BIOS: $bios (not Kinka 1.3; other versions are untested)"; fi
else
  note "BIOS missing: copy your Pippin ROM (Kinka 1.3) to $bios"
fi
command -v chdman >/dev/null || note "CHD discs need chdman: sudo apt install mame-tools (CUE/BIN discs work without it)"
note "Games go in ~/RetroPie/roms/pippin (CUE/BIN, CHD or ISO). Restart EmulationStation to see the Pippin system."
