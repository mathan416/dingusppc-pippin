#!/usr/bin/env python3
# Project: DingusPPC-Pippin
# File: pippin/prepare-disc.py
# Purpose: Turn a CUE/BIN, CHD, BIN or ISO disc into the data-track image DingusPPC reads,
#          and give each disc a stable id for its save folder.
# Author: Iain Bennett
# Copyright (c) 2026 Iain Bennett
# SPDX-License-Identifier: GPL-3.0-or-later
# Change log:
#   2026-11-18 - Added: CUE/CHD support for run.sh and the RetroPie installer.
# Full history: PIPPIN.md and Git history.
#
# Usage: prepare-disc.py DISC
# Prints "ID<tab>IMAGE". ID is the first 16 hex digits of the SHA-256 of the disc's resolved
# path. IMAGE is the file to give DingusPPC: the disc itself for .bin/.iso, or a cached
# copy of the first data track for .cue/.chd, kept in
# ${XDG_CACHE_HOME:-~/.cache}/dingusppc-pippin/discs/ID/ and rebuilt when the source changes.
# Works with Python 3.7 and later.
import hashlib, json, os, pathlib, shlex, shutil, subprocess, sys, tempfile


def fail(message):
    print('prepare-disc: ' + message, file=sys.stderr)
    sys.exit(1)


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tracks(cue):
    """FILE/TRACK/INDEX 01 entries of a cue sheet."""
    result, current = [], None
    for line in cue.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        words = shlex.split(line, posix=True)
        if not words:
            continue
        key = words[0].upper()
        if key == 'FILE':
            if words[-1].upper() != 'BINARY':
                fail('only BINARY files in cue sheets are supported: ' + str(cue))
            current = (cue.parent / words[1]).resolve()
        elif key == 'TRACK':
            result.append({'file': current, 'number': int(words[1]), 'mode': words[2].upper(), 'start': 0})
        elif key == 'INDEX' and words[1] == '01' and result:
            m, s, f = (int(x) for x in words[2].split(':'))
            result[-1]['start'] = (m * 60 + s) * 75 + f
    return result


def extract_first_track(cue, out):
    ts = tracks(cue)
    if not ts or ts[0]['number'] != 1 or ts[0]['mode'] not in ('MODE1/2352', 'MODE1/2048'):
        fail('the first track must be a MODE1 data track: ' + str(cue))
    t = ts[0]
    if not t['file'] or not t['file'].is_file():
        fail('track file not found: ' + str(t['file']))
    sector = 2352 if t['mode'].endswith('2352') else 2048
    start, end = t['start'] * sector, t['file'].stat().st_size
    if len(ts) > 1 and ts[1]['file'] == t['file']:
        end = ts[1]['start'] * sector
    if not 0 <= start < end:
        fail('invalid data-track bounds in ' + str(cue))
    with open(t['file'], 'rb') as src, open(out, 'wb') as dst:
        src.seek(start)
        remain = end - start
        while remain:
            block = src.read(min(remain, 4 << 20))
            if not block:
                fail('disc image is shorter than its cue sheet says: ' + str(t['file']))
            dst.write(block)
            remain -= len(block)


def source_stamp(disc):
    """What the cached track depends on: the disc and, for a cue, its track files."""
    stamp = {'disc': str(disc), 'size': disc.stat().st_size, 'mtime_ns': disc.stat().st_mtime_ns}
    if disc.suffix.lower() == '.cue':
        stamp['files'] = [[str(t['file']), t['file'].stat().st_size, t['file'].stat().st_mtime_ns]
                          for t in tracks(disc) if t['file'] and t['file'].is_file()]
    return stamp


def main():
    if len(sys.argv) != 2:
        fail('usage: prepare-disc.py DISC')
    disc = pathlib.Path(sys.argv[1]).resolve()
    if not disc.is_file():
        fail('disc not found: ' + sys.argv[1])
    ident = hashlib.sha256(str(disc).encode()).hexdigest()[:16]
    kind = disc.suffix.lower()
    if kind in ('.bin', '.iso', '.img', '.toast'):
        print(ident + '\t' + str(disc))
        return
    if kind not in ('.cue', '.chd'):
        fail('unsupported disc type ' + kind + ' (use .cue, .chd, .bin or .iso)')
    chdman = shutil.which('chdman')
    if kind == '.chd' and not chdman:
        fail('CHD discs need chdman: sudo apt install mame-tools')

    cache = pathlib.Path(os.environ.get('XDG_CACHE_HOME') or pathlib.Path.home() / '.cache')
    folder = cache / 'dingusppc-pippin' / 'discs' / ident
    folder.mkdir(parents=True, exist_ok=True)
    image, manifest = folder / 'disc.bin', folder / 'source.json'
    stamp = source_stamp(disc)
    if image.is_file() and manifest.is_file():
        try:
            old = json.loads(manifest.read_text())
            # Reuse the cached track while the source and the cached file are both unchanged.
            # (Size and timestamp, not a checksum: hashing 360 MB takes seconds on a Pi 4.)
            info = image.stat()
            if old.get('source') == json.loads(json.dumps(stamp)) and \
                    old.get('image') == [info.st_size, info.st_mtime_ns]:
                print(ident + '\t' + str(image))
                return
        except (ValueError, OSError):
            pass

    with tempfile.TemporaryDirectory(prefix='prepare-', dir=str(folder)) as temp:
        temp = pathlib.Path(temp)
        cue = disc
        if kind == '.chd':
            cue = temp / 'extract.cue'
            subprocess.run([chdman, 'extractcd', '-i', str(disc), '-o', str(cue), '-ob', str(temp / 'extract.bin')],
                           check=True, stdout=sys.stderr)
        extract_first_track(cue, temp / 'disc.bin')
        digest = sha1(temp / 'disc.bin')
        os.replace(str(temp / 'disc.bin'), str(image))
    info = image.stat()
    manifest.write_text(json.dumps({'source': stamp, 'sha1': digest, 'image': [info.st_size, info.st_mtime_ns]},
                                   indent=2))
    print(ident + '\t' + str(image))


if __name__ == '__main__':
    main()
