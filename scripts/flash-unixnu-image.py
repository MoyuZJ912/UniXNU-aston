#!/usr/bin/env python3
"""Writes a unixnu image (make-unixnu-payload-image.py or `unixnu build userdata`) to the Ace 3's unixnu partition from TWRP.

Refuses unless all of these hold: the phone is in TWRP over adb with the expected serial; /dev/block/by-name/unixnu is
/dev/block/sda15 with PARTNAME=unixnu, exactly 34359738368 bytes starting at byte 115424000 * 4096 (dual_sys plan-32g);
userdata is sda14 and is not the target; nothing on sda15 is mounted; the image fits. Before writing, the region the image
covers is read back to this PC (or recorded as all zero). The write is a dd of the image length only, then the region is read
back on the phone and its SHA-256 compared with the image. Nothing else is written; Android's userdata is never opened.

usage: flash-unixnu-image.py --adb ADB IMAGE        (asks for the confirmation "<serial> unixnu write")
"""
import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

SERIAL = '868b7abb'
PART = '/dev/block/sda15'
PART_BYTES = 34359738368
PART_START_BYTES = 115424000 * 4096
MIB = 1 << 20


def adb(args, *cmd, capture=True, binary=False):
    # adb never reads this script's stdin: `adb shell` would otherwise consume the typed confirmation.
    result = subprocess.run([args.adb, '-s', SERIAL, *cmd], capture_output=capture, check=True, stdin=subprocess.DEVNULL)
    return result.stdout if binary else result.stdout.decode().strip() if capture else None


def shell(args, command):
    return adb(args, 'shell', command)


def require(ok, why):
    if not ok:
        raise SystemExit(f'STOP: {why}; nothing written')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    image = args.image.read_bytes()
    require(image[:8] == b'SHNGROOT', 'the image does not start with a root manifest')
    require(len(image) % MIB == 0 and 0 < len(image) <= PART_BYTES, 'the image is not a whole number of MiB that fits unixnu')
    digest = hashlib.sha256(image).hexdigest()
    blocks = len(image) // MIB

    require(adb(args, 'get-state') == 'recovery', 'the phone is not in recovery')
    require(shell(args, 'getprop ro.twrp.version') != '', 'not TWRP')
    require(shell(args, 'getprop ro.serialno') == SERIAL, 'wrong device')
    require(shell(args, 'readlink -f /dev/block/by-name/unixnu') == PART, 'by-name/unixnu is not sda15')
    require(shell(args, 'readlink -f /dev/block/by-name/userdata') == '/dev/block/sda14', 'userdata is not sda14')
    require('PARTNAME=unixnu' in shell(args, 'cat /sys/class/block/sda15/uevent'), 'sda15 is not named unixnu')
    require(int(shell(args, f'blockdev --getsize64 {PART}')) == PART_BYTES, 'unixnu size differs from the plan')
    require(int(shell(args, 'cat /sys/class/block/sda15/start')) * 512 == PART_START_BYTES, 'unixnu start differs from the plan')
    require(shell(args, f'grep -c "^{PART} " /proc/mounts || true') in ('0', ''), 'something on sda15 is mounted')

    before = shell(args, f'dd if={PART} bs=1048576 count={blocks} 2>/dev/null | sha256sum').split()[0]
    zero = hashlib.sha256(bytes(len(image))).hexdigest()
    backup = args.image.with_name(args.image.name + f'.unixnu-before-{before[:12]}.bin')
    if before == zero:
        print(f'unixnu region of {len(image)} bytes is all zero before the write')
    else:
        data = adb(args, 'exec-out', f'dd if={PART} bs=1048576 count={blocks} 2>/dev/null', binary=True)
        require(hashlib.sha256(data).hexdigest() == before, 'backup readback mismatch')
        backup.write_bytes(data)
        print(f'previous contents saved to {backup}')
    if before == digest:
        print('unixnu already holds this image; nothing to do')
        return

    confirm = f'{SERIAL} unixnu write'
    print(f'Write {args.image.name} ({len(image)} bytes, sha256 {digest}) to {PART} (unixnu), from byte 0.')
    require(input(f'Type exactly: {confirm}\n> ') == confirm, 'confirmation did not match')

    remote = '/tmp/unixnu-image.img'
    adb(args, 'push', str(args.image), remote, capture=True)
    require(shell(args, f'sha256sum {remote}').split()[0] == digest, 'upload corrupted')
    shell(args, f'dd if={remote} of={PART} bs=1048576 count={blocks} conv=fsync 2>&1; sync; echo 3 > /proc/sys/vm/drop_caches')
    after = shell(args, f'dd if={PART} bs=1048576 count={blocks} 2>/dev/null | sha256sum').split()[0]
    shell(args, f'rm -f {remote}')
    if after != digest:
        raise SystemExit(f'READBACK MISMATCH: unixnu holds {after}, expected {digest}. Do not boot slot A; rewrite or restore {backup}')
    print(f'DONE: unixnu bytes 0..{len(image) - 1} read back as {digest}')


if __name__ == '__main__':
    main()
