#!/bin/bash
# Project: DingusPPC-Pippin
# File: pippin/runtime/enter.sh
# Purpose: Run a command inside the 64-bit runtime (a chroot) in a private mount namespace.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-18 - Added: shared entry point for building and running in the runtime.
# Full history: PIPPIN.md and Git history.
#
# Usage: sudo enter.sh RUNTIME [options] -- COMMAND [ARGS...]
#   --bind SRC:DST    bind SRC (file or folder) at DST inside the runtime, read/write
#   --ro SRC:DST      the same, read-only
#   --user            run COMMAND as the user who called sudo, with their groups (default: root)
#   --env NAME=VALUE  set a variable for COMMAND (the environment is otherwise cleared)
#   --network         copy the host's DNS settings in (for apt)
# The runtime shares the host's kernel, devices and udev; nothing is mounted outside the
# private namespace, so every mount disappears when COMMAND exits.
set -euo pipefail

die() { echo "enter.sh: $*" >&2; exit 1; }
[[ $EUID == 0 ]] || die "run with sudo"
[[ $# -ge 1 ]] || die "usage: sudo enter.sh RUNTIME [options] -- COMMAND"
root=$(cd "$1" && pwd) || die "runtime not found: $1"
[[ -f $root/.dingusppc-pippin-runtime ]] || die "$root is not a DingusPPC-Pippin runtime (see make-runtime.sh)"

if [[ ${PIPPIN_ENTER_PRIVATE:-} != 1 ]]; then
  exec env PIPPIN_ENTER_PRIVATE=1 unshare --mount --propagation private "$0" "$@"
fi
shift

binds=(); envs=(PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 HOME=/tmp)
as_user=0; network=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --bind|--ro) [[ $# -ge 2 && $2 == *:/* ]] || die "$1 needs SRC:/DST"; binds+=("$1" "$2"); shift 2 ;;
    --user) as_user=1; shift ;;
    --env) [[ $# -ge 2 && $2 == *=* ]] || die "--env needs NAME=VALUE"; envs+=("$2"); shift 2 ;;
    --network) network=1; shift ;;
    --) shift; break ;;
    *) die "unknown option $1" ;;
  esac
done
[[ $# -ge 1 ]] || die "no command given"

mount --make-rprivate /
mount -t tmpfs -o mode=1777 tmpfs "$root/tmp"
mount --rbind /dev "$root/dev"; mount --make-rslave "$root/dev"
mount --rbind /sys "$root/sys"; mount --make-rslave "$root/sys"
mount -t proc proc "$root/proc"
if [[ -d /run/udev ]]; then
  mkdir -p "$root/run/udev"; mount --bind /run/udev "$root/run/udev"; mount -o remount,bind,ro "$root/run/udev"
fi
for ((i = 0; i < ${#binds[@]}; i += 2)); do
  mode=${binds[i]}; src=${binds[i+1]%%:/*}; dst=/${binds[i+1]#*:/}
  [[ -e $src ]] || die "bind source not found: $src"
  target=$root$dst
  if [[ -d $src ]]; then mkdir -p "$target"; else mkdir -p "$(dirname "$target")"; [[ -e $target ]] || : > "$target"; fi
  mount --bind "$src" "$target"
  [[ $mode == --ro ]] && mount -o remount,bind,ro "$target"
done
if ((network)); then
  rm -f "$root/etc/resolv.conf"; cp -L /etc/resolv.conf "$root/etc/resolv.conf"
fi

if ((as_user)); then
  uid=${SUDO_UID:?--user needs sudo}; gid=${SUDO_GID:?}; user=${SUDO_USER:?}
  groups=$(id -G "$user" | tr ' ' ',')
  exec chroot --userspec="$uid:$gid" --groups="$groups" "$root" /usr/bin/env -i "${envs[@]}" "$@"
fi
exec chroot "$root" /usr/bin/env -i "${envs[@]}" "$@"
