#!/usr/bin/env python3
"""Writes a whole boot_a image (Ace3-UniXNU out/phone/boot_a-*-avb.img) to the Ace 3's boot_a from TWRP; never boot_b.

Refuses unless all of these hold: the phone is in TWRP over adb with the expected serial and runs slot B (daily Android);
/dev/block/by-name/boot_a is /dev/block/sde13 of exactly 201326592 bytes, not mounted; the image has exactly that size. The
current boot_a is read back first: if its SHA-256 matches a file given with --known (copies already on this PC), that file is the
backup; otherwise the partition is saved next to the image. The write is a dd of the whole image, then boot_a is read back on
the phone and its SHA-256 compared with the image. The slot attributes are not changed (switch to slot A in TWRP yourself).

usage: flash-aston-boot-a.py --adb ADB [--known FILE ...] IMAGE        (asks for the confirmation "<serial> boot_a write")
"""
import argparse
import hashlib
import subprocess
from pathlib import Path

SERIAL = '868b7abb'
PART = '/dev/block/sde13'
PART_BYTES = 201326592


def adb(args, *cmd, binary=False):
    # adb never reads this script's stdin: `adb shell` would otherwise consume the typed confirmation.
    result = subprocess.run([args.adb, '-s', SERIAL, *cmd], capture_output=True, check=True, stdin=subprocess.DEVNULL)
    return result.stdout if binary else result.stdout.decode().strip()


def shell(args, command):
    return adb(args, 'shell', command)


def require(ok, why):
    if not ok:
        raise SystemExit(f'STOP: {why}; nothing written')


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--adb', required=True)
    parser.add_argument('--known', type=Path, action='append', default=[], help='a PC copy that may match the current boot_a')
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    require(args.image.stat().st_size == PART_BYTES, f'the image is not exactly {PART_BYTES} bytes')
    digest = sha256_file(args.image)

    require(adb(args, 'get-state') == 'recovery', 'the phone is not in recovery')
    require(shell(args, 'getprop ro.twrp.version') != '', 'not TWRP')
    require(shell(args, 'getprop ro.serialno') == SERIAL, 'wrong device')
    require(shell(args, 'getprop ro.boot.slot_suffix') == '_b', 'the phone is not running slot B')
    require(shell(args, 'readlink -f /dev/block/by-name/boot_a') == PART, 'by-name/boot_a is not sde13')
    require(shell(args, 'readlink -f /dev/block/by-name/boot_b') != PART, 'boot_b resolves to the target')
    require('PARTNAME=boot_a' in shell(args, 'cat /sys/class/block/sde13/uevent'), 'sde13 is not named boot_a')
    require(int(shell(args, f'blockdev --getsize64 {PART}')) == PART_BYTES, 'boot_a size differs')
    require(shell(args, f'grep -c "^{PART} " /proc/mounts || true') in ('0', ''), 'boot_a is mounted')

    before = shell(args, f'dd if={PART} bs=1048576 2>/dev/null | sha256sum').split()[0]
    if before == digest:
        print('boot_a already holds this image; nothing to do')
        return
    copies = [path for path in args.known if path.is_file() and sha256_file(path) == before]
    if copies:
        print(f'current boot_a ({before[:16]}…) is already on this PC: {copies[0]}')
    else:
        backup = args.image.with_name(f'boot_a-before-{before[:12]}.img')
        data = adb(args, 'exec-out', f'dd if={PART} bs=1048576 2>/dev/null', binary=True)
        require(hashlib.sha256(data).hexdigest() == before, 'backup readback mismatch')
        backup.write_bytes(data)
        print(f'current boot_a saved to {backup}')

    confirm = f'{SERIAL} boot_a write'
    print(f'Write {args.image.name} ({PART_BYTES} bytes, sha256 {digest}) to {PART} (boot_a).')
    require(input(f'Type exactly: {confirm}\n> ') == confirm, 'confirmation did not match')

    remote = '/tmp/boot_a-image.img'
    adb(args, 'push', str(args.image), remote)
    require(shell(args, f'sha256sum {remote}').split()[0] == digest, 'upload corrupted')
    shell(args, f'dd if={remote} of={PART} bs=1048576 conv=fsync 2>&1; sync; echo 3 > /proc/sys/vm/drop_caches')
    after = shell(args, f'dd if={PART} bs=1048576 2>/dev/null | sha256sum').split()[0]
    shell(args, f'rm -f {remote}')
    if after != digest:
        raise SystemExit(f'READBACK MISMATCH: boot_a holds {after}, expected {digest}. Do not switch to slot A; write the backup back')
    print(f'DONE: boot_a read back as {digest}')


if __name__ == '__main__':
    main()
