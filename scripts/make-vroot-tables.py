#!/usr/bin/env python3
"""The verified-root block tables ("<window>.vroot" userdata objects) of the 24A446 root windows, checked against the signed policies.

  make-vroot-tables.py --unixnu UNIXNU --policies scripts/policies/ipados-27.0.1-24A446 --out DIR NAME=PLAIN_DMG ...

The kernel (package/SourceRootPolicies.cpp) carries only each policy's 544-byte signed header (VerifiedRoot tools/manifest.py:
160-byte header, RSA-3072 signature); ShengUfs reads the table itself from the root image's "<window>.vroot" object, the SHA-256
of every 4096-byte block of the window in order. The reference board keeps those tables local, so they are made here from the
same disk images, read through UniXNU's own reader (scripts/unixnu/userdata.py disk_blocks: udif.py on Linux, hdiutil on macOS),
so a window and its table come from the same bytes.

Nothing is written unless all of these match the policy's header and verification.json: magic VROOT01, version 1, 4096-byte
blocks, image bytes and block count, the image SHA-256, the table SHA-256, the volume UUID, and the anchor (trusted-key.bin)
whose SHA-256 the header names. The header's RSA signature is checked with openssl against that anchor. Output: DIR/NAME.vroot
(block count x 32 bytes) and DIR/vroot-tables.json; pass them to `unixnu build userdata --object NAME.vroot=DIR/NAME.vroot`.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

HEADER = struct.Struct('<8s4I2Q16s32s32s32sQ')      # VerifiedRoot tools/manifest.py
SIGNATURE_BYTES = 384


def der_length(n):
    return bytes([n]) if n < 128 else bytes([0x80 | ((n.bit_length() + 7) // 8)]) + n.to_bytes((n.bit_length() + 7) // 8, 'big')


def der(tag, body):
    return bytes([tag]) + der_length(len(body)) + body


def der_int(value):
    raw = value.to_bytes((value.bit_length() + 8) // 8, 'big')     # a leading zero keeps it positive
    return der(0x02, raw)


def anchor_pem(anchor):
    """SubjectPublicKeyInfo PEM of a VROOTKEY anchor ("VROOTKEY", 384-byte modulus, exponent 65537)."""
    if len(anchor) != 395 or anchor[:8] != b'VROOTKEY' or anchor[392:] != b'\x01\x00\x01':
        raise SystemExit('trusted-key.bin is not a VROOTKEY RSA-3072 anchor')
    rsa = der(0x30, der_int(int.from_bytes(anchor[8:392], 'big')) + der_int(65537))
    algorithm = der(0x30, bytes.fromhex('06092a864886f70d010101') + b'\x05\x00')
    spki = der(0x30, algorithm + der(0x03, b'\x00' + rsa))
    import base64
    text = base64.encodebytes(spki).decode().replace('\n', '')
    return '-----BEGIN PUBLIC KEY-----\n' + '\n'.join(text[i:i + 64] for i in range(0, len(text), 64)) + '\n-----END PUBLIC KEY-----\n'


def check_signature(header, signature, anchor):
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / 'key.pem').write_text(anchor_pem(anchor))
        (d / 'header.bin').write_bytes(header)
        (d / 'signature.bin').write_bytes(signature)
        result = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', d / 'key.pem', '-signature', d / 'signature.bin',
                                 d / 'header.bin'], capture_output=True, text=True)
        return result.returncode == 0 and 'Verified OK' in result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--unixnu', type=Path, required=True, help='UniXNU checkout (its scripts/unixnu reads the disk images)')
    parser.add_argument('--policies', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('images', nargs='+', metavar='NAME=PLAIN_DMG')
    args = parser.parse_args()
    sys.path.insert(0, str(args.unixnu / 'scripts'))
    from unixnu import userdata, udif   # noqa: E402  (UniXNU's reader, the one userdata.py uses for the windows)
    import platform
    args.out.mkdir(parents=True, exist_ok=True)
    report = {}
    for pair in args.images:
        name, dmg = pair.split('=', 1)
        dmg = Path(dmg)
        policy = args.policies / name
        blob = (policy / 'manifest.header.bin').read_bytes()
        if len(blob) != HEADER.size + SIGNATURE_BYTES:
            raise SystemExit(f'{policy}/manifest.header.bin is not a 544-byte signed header')
        header, signature = blob[:HEADER.size], blob[HEADER.size:]
        magic, version, header_bytes, block, reserved, image_bytes, count, volume, image_hash, table_hash, anchor_hash, generation = \
            HEADER.unpack(header)
        verification = json.loads((policy / 'verification.json').read_text())
        anchor = (policy / 'trusted-key.bin').read_bytes()
        if (magic, version, header_bytes, block, reserved) != (b'VROOT01\0', 1, HEADER.size, 4096, 0):
            raise SystemExit(f'{name}: unexpected policy header fields')
        if hashlib.sha256(anchor).digest() != anchor_hash or verification['trusted_key_sha256'] != anchor_hash.hex():
            raise SystemExit(f'{name}: trusted-key.bin is not the anchor the header names')
        if not check_signature(header, signature, anchor):
            raise SystemExit(f'{name}: the header signature does not verify under its anchor')
        if (verification['image_bytes'], verification['image_sha256'], verification['volume_uuid'].upper()) != \
                (image_bytes, image_hash.hex(), str(uuid.UUID(bytes=volume)).upper()) or count * 4096 != image_bytes:
            raise SystemExit(f'{name}: verification.json and the header disagree')
        size = udif.Image(dmg).size if platform.system() != 'Darwin' else userdata.image_size(dmg)
        if size != image_bytes:
            raise SystemExit(f'{name}: {dmg.name} is {size} bytes of raw disk, the policy covers {image_bytes}')
        partial = args.out / f'{name}.vroot.partial'
        whole, table, done, pending = hashlib.sha256(), hashlib.sha256(), 0, b''
        print(f'{name}: hashing {image_bytes} bytes of {dmg.name}', flush=True)
        with partial.open('wb') as out:
            for data in userdata.disk_blocks(dmg, image_bytes):
                whole.update(data)
                done += len(data)
                data = pending + data if pending else data
                usable = len(data) - len(data) % 4096
                digests = b''.join(hashlib.sha256(data[i:i + 4096]).digest() for i in range(0, usable, 4096))
                table.update(digests)
                out.write(digests)
                pending = data[usable:]
                if done % (1 << 30) < len(data):
                    print(f'{name}: {done >> 20} MiB', flush=True)
        if pending or done != image_bytes:
            partial.unlink()
            raise SystemExit(f'{name}: read {done} bytes ({len(pending)} left over), expected {image_bytes}')
        if whole.digest() != image_hash or table.digest() != table_hash:
            partial.unlink()
            raise SystemExit(f'{name}: the disk image does not match the signed policy (image {whole.hexdigest()[:16]}, '
                             f'table {table.hexdigest()[:16]}); it is not the image the reference board signed')
        target = args.out / f'{name}.vroot'
        partial.rename(target)
        report[name] = dict(object=f'{name}.vroot', bytes=target.stat().st_size, blocks=count, image_bytes=image_bytes,
                            image_sha256=image_hash.hex(), table_sha256=table_hash.hex(), volume_uuid=verification['volume_uuid'],
                            generation=generation, signature_verified=True, source=dmg.name)
        print(f'{name}: {target} ({count} blocks) matches the signed policy', flush=True)
    (args.out / 'vroot-tables.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
