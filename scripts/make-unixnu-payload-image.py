#!/usr/bin/env python3
"""A unixnu partition image that holds only the iOS payload: the SHNGROOT root manifest of UniXNU scripts/unixnu/userdata.py
(version 2) with one data object, "Payload", and no OS windows.

It is the first-boot image of the Ace 3: Ace3-UniXNU's AstonConfigDxe finds the object, checks both SHA-256 values and
publishes the payload volume, so UniXNU and DarwinBoot start the kernel. Without OS windows the kernel stops when it looks for
the root volume; `unixnu build userdata --object Payload=payload.img` makes the full image with the same object later.

usage: make-unixnu-payload-image.py payload.img OUT.img
Writes OUT.img (manifest block, padding to 1 MiB, the payload, padding to 1 MiB) and OUT.img.json.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

MIB = 1 << 20
UNIXNU_BYTES = 34359738368          # sda15 on the Ace 3 (dual_sys plan-32g)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    payload, out = Path(sys.argv[1]), Path(sys.argv[2])
    data = payload.read_bytes()
    if not data or len(data) % 512:
        raise SystemExit(f'{payload}: a payload volume is a whole number of 512-byte sectors')
    if data[510:512] != b'\x55\xaa':
        raise SystemExit(f'{payload}: no FAT boot sector signature')
    offset = MIB
    total = offset + (len(data) + MIB - 1) // MIB * MIB
    if total > UNIXNU_BYTES:
        raise SystemExit('the image is larger than the unixnu partition')
    digest = hashlib.sha256(data).digest()
    header = bytearray(4096)
    header[0:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 2, 1, total)
    struct.pack_into('<32sQQII32s40s', header, 24, b'Payload', offset, len(data), 4096, 3, digest, bytes(40))
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
    with out.open('wb') as f:
        f.truncate(total)
        f.write(header)
        f.seek(offset)
        f.write(data)
    report = {'image': out.name, 'image_bytes': total, 'image_sha256': hashlib.sha256(out.read_bytes()).hexdigest(),
              'manifest_sha256': hashlib.sha256(bytes(header)).hexdigest(),
              'objects': [{'name': 'Payload', 'offset': offset, 'size': len(data), 'sha256': digest.hex(), 'source': payload.name}],
              'windows': [], 'target': 'unixnu (never userdata)', 'hardware_tested': False}
    Path(str(out) + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
