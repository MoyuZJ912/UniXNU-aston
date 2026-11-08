#!/usr/bin/env python3
"""Writes a unixnu image (make-unixnu-payload-image.py or `unixnu build userdata`) to the Ace 3's unixnu partition from TWRP.

Refuses unless all of these hold: the phone is in TWRP over adb with the expected serial; /dev/block/by-name/unixnu is
/dev/block/sda15 with PARTNAME=unixnu, exactly 34359738368 bytes starting at byte 115424000 * 4096 (dual_sys plan-32g);
userdata is sda14 and is not the target; nothing on sda15 is mounted; the image fits. Before writing, the region the image
covers is identified: all zero, or a --known image on this PC followed by zeros (nothing to save), or else it is streamed back
to this PC. The image is written in 1 GiB pieces (a root image of 16 GB does not fit TWRP's /tmp): each is pushed, checked on
the phone, and written by dd at its offset; then the whole region is read back on the phone and its SHA-256 compared. Nothing else is written;
Android's userdata is never opened. Large images are hashed and sent in pieces, never held in memory.

usage: flash-unixnu-image.py --adb ADB [--known FILE ...] IMAGE        (asks for the confirmation "<serial> unixnu write")
"""
import argparse
import hashlib
import subprocess
from pathlib import Path

SERIAL = '868b7abb'
PART = '/dev/block/sda15'
PART_BYTES = 34359738368
PART_START_BYTES = 115424000 * 4096
MIB = 1 << 20
ZERO_MIB = bytes(MIB)
CHUNK = 1024 * MIB          # one piece in TWRP's /tmp (RAM) at a time


def adb(args, *cmd, capture=True, binary=False):
    # adb never reads this script's stdin: `adb shell` would otherwise consume the typed confirmation.
    result = subprocess.run([args.adb, '-s', SERIAL, *cmd], capture_output=capture, check=True, stdin=subprocess.DEVNULL)
    return result.stdout if binary else result.stdout.decode().strip() if capture else None


def shell(args, command):
    return adb(args, 'shell', command)


def require(ok, why):
    if not ok:
        raise SystemExit(f'STOP: {why}; nothing written')


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(8 * MIB), b''):
            digest.update(block)
    return digest.hexdigest()


def zero_sha256(nbytes):
    digest = hashlib.sha256()
    for _ in range(nbytes // MIB):
        digest.update(ZERO_MIB)
    return digest.hexdigest()


def region_sha256(args, skip_mib, count_mib):
    if not count_mib:
        return hashlib.sha256().hexdigest()
    return shell(args, f'dd if={PART} bs=1048576 skip={skip_mib} count={count_mib} 2>/dev/null | sha256sum').split()[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--known', type=Path, action='append', default=[],
                        help='an image on this PC that unixnu may hold now (followed by zeros): no backup is read back then')
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    size = args.image.stat().st_size
    with open(args.image, 'rb') as stream:
        require(stream.read(8) == b'SHNGROOT', 'the image does not start with a root manifest')
    require(size % MIB == 0 and 0 < size <= PART_BYTES, 'the image is not a whole number of MiB that fits unixnu')
    print(f'hashing {args.image.name} ({size} bytes)')
    digest = file_sha256(args.image)
    blocks = size // MIB

    require(adb(args, 'get-state') == 'recovery', 'the phone is not in recovery')
    require(shell(args, 'getprop ro.twrp.version') != '', 'not TWRP')
    require(shell(args, 'getprop ro.serialno') == SERIAL, 'wrong device')
    require(shell(args, 'readlink -f /dev/block/by-name/unixnu') == PART, 'by-name/unixnu is not sda15')
    require(shell(args, 'readlink -f /dev/block/by-name/userdata') == '/dev/block/sda14', 'userdata is not sda14')
    require('PARTNAME=unixnu' in shell(args, 'cat /sys/class/block/sda15/uevent'), 'sda15 is not named unixnu')
    require(int(shell(args, f'blockdev --getsize64 {PART}')) == PART_BYTES, 'unixnu size differs from the plan')
    require(int(shell(args, 'cat /sys/class/block/sda15/start')) * 512 == PART_START_BYTES, 'unixnu start differs from the plan')
    require(shell(args, f'grep -c "^{PART} " /proc/mounts || true') in ('0', ''), 'something on sda15 is mounted')

    before = region_sha256(args, 0, blocks)
    if before == digest:
        print('unixnu already holds this image; nothing to do')
        return
    backup = None
    if before == zero_sha256(size):
        print(f'unixnu region of {size} bytes is all zero before the write')
    else:
        for known in args.known:
            known_size = known.stat().st_size
            if known_size % MIB or known_size > size:
                continue
            if region_sha256(args, 0, known_size // MIB) == file_sha256(known) and \
                    region_sha256(args, known_size // MIB, (size - known_size) // MIB) == zero_sha256(size - known_size):
                backup = known
                print(f'unixnu now holds {known.name} followed by zeros; that file is the backup')
                break
        if backup is None:
            backup = args.image.with_name(args.image.name + f'.unixnu-before-{before[:12]}.bin')
            print(f'saving the current {size} bytes of unixnu to {backup}')
            with open(backup, 'wb') as out:
                subprocess.run([args.adb, '-s', SERIAL, 'exec-out', f'dd if={PART} bs=1048576 count={blocks} 2>/dev/null'],
                               stdout=out, stdin=subprocess.DEVNULL, check=True)
            require(file_sha256(backup) == before, 'backup readback mismatch')
            print(f'previous contents saved to {backup}')

    confirm = f'{SERIAL} unixnu write'
    print(f'Write {args.image.name} ({size} bytes, sha256 {digest}) to {PART} (unixnu), from byte 0.')
    require(input(f'Type exactly: {confirm}\n> ') == confirm, 'confirmation did not match')

    # In pieces through adb's file sync (push), each checked on the phone before dd writes it at its offset. Not `adb exec-in`:
    # on 2026-11-08 it dropped bytes from the stream here (59 at the first MiB, about 17 KB over 113 MiB) while push never has.
    remote = '/tmp/unixnu-chunk.bin'
    free = int(shell(args, "df -k /tmp | tail -1 | awk '{print $4}'")) * 1024
    require(free >= CHUNK + 256 * MIB, f'TWRP /tmp has {free >> 20} MiB free, a piece needs {CHUNK >> 20} MiB')
    local = args.image.with_name(args.image.name + '.chunk')
    with open(args.image, 'rb') as stream:
        for offset in range(0, size, CHUNK):
            length = min(CHUNK, size - offset)
            piece = hashlib.sha256()
            with open(local, 'wb') as out:
                for _ in range(length // MIB):
                    data = stream.read(MIB)
                    piece.update(data)
                    out.write(data)
            adb(args, 'push', str(local), remote)
            if shell(args, f'sha256sum {remote}').split()[0] != piece.hexdigest():
                shell(args, f'rm -f {remote}')
                raise SystemExit(f'STOP: the piece at {offset} arrived corrupted; unixnu holds the pieces before it. Do not boot slot A.')
            shell(args, f'dd if={remote} of={PART} bs=1048576 seek={offset // MIB} count={length // MIB} conv=notrunc,fsync 2>&1')
            shell(args, f'rm -f {remote}')
            print(f'written {offset + length >> 20}/{size >> 20} MiB', flush=True)
    local.unlink()
    shell(args, 'sync; echo 3 > /proc/sys/vm/drop_caches')
    after = region_sha256(args, 0, blocks)
    if after != digest:
        raise SystemExit(f'READBACK MISMATCH: unixnu holds {after}, expected {digest}. Do not boot slot A; rewrite, or restore '
                         f'{backup if backup else "zeros"}')
    print(f'DONE: unixnu bytes 0..{size - 1} read back as {digest}')


if __name__ == '__main__':
    main()
