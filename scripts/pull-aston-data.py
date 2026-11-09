#!/usr/bin/env python3
"""Copies the persistent iPadOS Data store of the Ace 3 (the pages the running system changed) from TWRP, read only.

The Data disk is the packed installation seed (DBRDISK1, the payload's Data ramdisk) with the changed pages on top; those live in
the store window at the end of unixnu (RamBlock PersistOverlay.h: header block "SHNGDATA", presence bitmap, page i at
1 + bitmap blocks + i), which ends where the 1024 MiB log reserve begins (NativeUfs native_ufs_store_open). After
rootstockos/UniXNU-asteroids scripts/pull-asteroids-data.py, for unixnu (sda15) instead of userdata, with every read checked on the
phone: the pages are hashed there and here.

Nothing on the phone is written except the read script in TWRP's /tmp (RAM). Writes OUT/pages.bin, OUT/runs.json and OUT/store.json;
scripts/build-aston-data-image.py (Linux) then lays the seed and the pages into a sparse data.img to mount read only.

usage: pull-aston-data.py --adb ADB --seed data.dbr --out DIR
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

SERIAL = '868b7abb'
PART = '/dev/block/sda15'
PART_BYTES = 34359738368
BLOCK = 4096
RESERVE_BLOCKS = 1024 * 256     # the log reserve at the end of unixnu (1024 MiB)


def adb(args, *cmd, data=False):
    out = subprocess.run([args.adb, '-s', SERIAL, *cmd], check=True, capture_output=True, stdin=subprocess.DEVNULL).stdout
    return out if data else out.decode().strip()


def require(ok, why):
    if not ok:
        raise SystemExit(f'STOP: {why}')


def dd(args, skip, count):
    blob = adb(args, 'exec-out', f'dd if={PART} bs={BLOCK} skip={skip} count={count} 2>/dev/null', data=True)
    require(len(blob) == count * BLOCK, f'short read at block {skip}: {len(blob)} of {count * BLOCK} bytes')
    there = adb(args, 'shell', f'dd if={PART} bs={BLOCK} skip={skip} count={count} 2>/dev/null | sha256sum').split()[0]
    require(hashlib.sha256(blob).hexdigest() == there, f'read at block {skip} differs from the phone')
    return blob


def seed_logical(seed):
    with open(seed, 'rb') as f:
        magic, version, page, logical, _count = struct.unpack('<8sIIQQ', f.read(32))
    require(magic == b'DBRDISK1' and version == 1 and page == BLOCK, 'not a DBRDISK1 seed')
    return logical


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--seed', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()

    logical = seed_logical(args.seed)
    pages = logical // BLOCK
    bitmap_blocks = (pages + 8 * BLOCK - 1) // (8 * BLOCK)
    window = 1 + bitmap_blocks + pages
    start = PART_BYTES // BLOCK - RESERVE_BLOCKS - window

    require(adb(args, 'get-state') == 'recovery', 'the phone is not in recovery')
    require(adb(args, 'shell', 'getprop ro.twrp.version') != '', 'not TWRP')
    require(adb(args, 'shell', 'readlink -f /dev/block/by-name/unixnu') == PART, 'by-name/unixnu is not sda15')
    require('PARTNAME=unixnu' in adb(args, 'shell', 'cat /sys/class/block/sda15/uevent'), 'sda15 is not named unixnu')
    require(int(adb(args, 'shell', f'blockdev --getsize64 {PART}')) == PART_BYTES, 'unixnu size differs from the plan')

    header = dd(args, start, 1)
    require(header[:8] == b'SHNGDATA', f'no SHNGDATA header at block {start:#x}')
    count, = struct.unpack_from('<Q', header, 16)
    generation, = struct.unpack_from('<I', header, 56)
    require(count == pages, f'store has {count} pages, the seed {pages}: the boot used another seed')
    bitmap = dd(args, start + 1, bitmap_blocks)
    present = [byte * 8 + bit for byte, value in enumerate(bitmap) if value
               for bit in range(8) if value >> bit & 1 and byte * 8 + bit < pages]
    runs = []
    for index in present:
        if runs and runs[-1][0] + runs[-1][1] == index:
            runs[-1][1] += 1
        else:
            runs.append([index, 1])
    print(f'store at block {start:#x}, generation {generation}, {len(present)} changed pages ({len(present) * BLOCK >> 20} MiB) '
          f'in {len(runs)} runs', flush=True)

    args.out.mkdir(parents=True, exist_ok=True)
    base = start + 1 + bitmap_blocks
    script = args.out / 'pull.sh'
    script.write_text(''.join(f'dd if={PART} bs={BLOCK} skip={base + i} count={n} 2>/dev/null\n' for i, n in runs), newline='\n')
    adb(args, 'push', str(script), '/tmp/aston-pull.sh')
    there = adb(args, 'shell', 'sh /tmp/aston-pull.sh | sha256sum').split()[0]
    digest = hashlib.sha256()
    total = 0
    with open(args.out / 'pages.bin', 'wb') as out:
        proc = subprocess.Popen([args.adb, '-s', SERIAL, 'exec-out', 'sh /tmp/aston-pull.sh'], stdout=subprocess.PIPE,
                                stdin=subprocess.DEVNULL)
        for chunk in iter(lambda: proc.stdout.read(1 << 20), b''):
            digest.update(chunk)
            out.write(chunk)
            total += len(chunk)
        require(proc.wait() == 0, 'adb exec-out failed')
    adb(args, 'shell', 'rm -f /tmp/aston-pull.sh')
    require(total == len(present) * BLOCK, f'pulled {total} bytes, expected {len(present) * BLOCK}')
    require(digest.hexdigest() == there, 'the pages differ from what the phone read')
    (args.out / 'runs.json').write_text(json.dumps(runs) + '\n')
    (args.out / 'store.json').write_text(json.dumps({
        'device': PART, 'store_block': start, 'generation': generation, 'pages': pages, 'changed_pages': len(present),
        'runs': len(runs), 'pages_sha256': there, 'seed': str(args.seed)}, indent=1) + '\n')
    print(f'pages: {args.out / "pages.bin"} ({total} bytes, sha256 {there})')


if __name__ == '__main__':
    main()
