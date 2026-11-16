#!/bin/bash
# Q233: Q227 + Taos 59.94 Hz VBL; Q224a (Q219 source, cortex-a72 + LTO) plus cached exec-entry environment lookups.
set -euo pipefail
p=/home/pi/sheepshaver-pippin-port-20261110
q=$p/performance-v233-taos-vbl
root=/home/pi/dingusppc-pippin-20261110/rootfs
if [[ "${1:-}" != inside ]];then
 exec 9>"$p/performance-comparison.lock";flock -n 9
 exec 8>"$p/cabinet-v48/launch.lock";flock -n 8
 exec 7>"$p/test.lock";flock -n 7
 python3 "$p/cabinet-v48/assert-idle.py"
 sudo unshare --mount --propagation private bash "$0" inside
 exit $?
fi
[[ "$EUID" == 0 && -f "$root/.dingus-private-runtime" ]]
[[ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ]]
mount --bind "$root" "$root";mount -o remount,bind,ro "$root"
mount --rbind /dev "$root/dev";mount --make-rslave "$root/dev"
mount -t proc proc "$root/proc";mount -t tmpfs -o mode=1777 tmpfs "$root/tmp"
mount --bind "$q/hybrid" "$root/work"
mount --bind "$q/build" "$root/session"
chroot --userspec=1000:1000 "$root" /usr/bin/env -i HOME=/tmp PATH=/usr/bin:/bin LANG=C FLAGS="-mcpu=cortex-a72 -flto=2 -flto-partition=one -fno-fat-lto-objects" /bin/bash -c '
set -e
cd /work/build
cmake -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$FLAGS -Wl,-u,pippin_cpu_owner_guest_time_ns" -DCMAKE_AR=/usr/bin/gcc-ar -DCMAKE_RANLIB=/usr/bin/gcc-ranlib . > /dev/null
nice -n 15 ionice -c2 -n7 cmake --build . --parallel 2
cp bin/dingusppc /session/pippin-v233; sha256sum /session/pippin-v233'
