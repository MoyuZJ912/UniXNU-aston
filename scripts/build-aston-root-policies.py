#!/usr/bin/env python3
"""Sign the OS and SystemOS root images of an OS profile with this board's owner key, verify every block with the compiled C
verifier, and write the policy directory build-sheng-source-storage.py --policy-dir reads plus the .vroot tables of the root image.

After markakash/UniXNU-ishtar scripts/build-root-policies.py, for a Linux host: the C verifier is VerifiedRoot's
src/verified_root.c with tests/openssl_provider.c built as a shared object (see --library). The private key never enters the
repository; only the signed headers, the public anchor and the verification records do.

  build-aston-root-policies.py --unixnu ../UniXNU --profile ios-27.0.1-24A446 --images DIR --key owner.pem --public owner.pub.pem \
      --library verified-root.so --policies scripts/policies/ios-27.0.1-24A446 --tables OUT

DIR holds the decrypted images (raw APFS containers, sha256 as the profile's [root] names). Writes POLICIES/{OS,SystemOS}/
{manifest.header.bin, trusted-key.bin, verification.json} and OUT/{OS,SystemOS}.vroot (the bare block hash tables).
"""
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--unixnu', type=Path, required=True)
parser.add_argument('--profile', required=True)
parser.add_argument('--images', type=Path, required=True)
parser.add_argument('--key', type=Path, required=True)
parser.add_argument('--public', type=Path, required=True)
parser.add_argument('--library', type=Path, required=True)
parser.add_argument('--policies', type=Path, required=True)
parser.add_argument('--tables', type=Path, required=True)
args = parser.parse_args()

kit = args.unixnu / 'kits/VerifiedRoot/tools'
spec = importlib.util.spec_from_file_location('verified_manifest', kit / 'manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)
profile = tomllib.loads((args.unixnu / f'profiles/os/{args.profile}/profile.toml').read_text())


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


images = {}
for path in sorted(args.images.glob('*.dmg')):
    images[sha256_file(path)] = path
args.tables.mkdir(parents=True, exist_ok=True)
anchors = set()
for name in ('OS', 'SystemOS'):
    prefix = name.lower()
    volume, digest = profile['root'][f'{prefix}_volume_uuid'], profile['root'][f'{prefix}_image_sha256']
    image = images.get(digest)
    if image is None:
        sys.exit(f'{name}: no image in {args.images} hashes to {digest}')
    with tempfile.TemporaryDirectory(prefix=f'policy-{prefix}-') as work:
        out = Path(work)
        with image.open('rb') as stream:
            record = manifest.make(stream, image.stat().st_size, args.key, args.public, volume, 1, out)
        if record['image_sha256'] != digest:
            sys.exit(f'{name}: {image.name} hashes to {record["image_sha256"]} while signing, profile says {digest}')
        subprocess.run([sys.executable, kit / 'verify_image.py', '--library', args.library, '--manifest', out / 'manifest.bin',
                        '--trusted-key', out / 'trusted-key.bin', '--image', image, '--volume-uuid', volume,
                        '--minimum-generation', '1', '--output', out / 'verification.json'], check=True)
        verification = json.loads((out / 'verification.json').read_text())
        if verification['image_sha256'] != digest or verification['volume_uuid'] != volume.upper():
            sys.exit(f'{name}: the verification record does not describe the profile image')
        policy = args.policies / name
        policy.mkdir(parents=True, exist_ok=True)
        (policy / 'manifest.header.bin').write_bytes((out / 'manifest.bin').read_bytes()[:544])
        shutil.copyfile(out / 'trusted-key.bin', policy / 'trusted-key.bin')
        shutil.copyfile(out / 'verification.json', policy / 'verification.json')
        shutil.copyfile(out / 'hashes.bin', args.tables / f'{name}.vroot')
        anchors.add((out / 'trusted-key.bin').read_bytes())
        table = args.tables / f'{name}.vroot'
        if table.stat().st_size != record['block_count'] * 32:
            sys.exit(f'{name}: the table is not {record["block_count"]} block hashes')
        print(f'{name}: {image.name} {volume} {digest[:16]} blocks {record["block_count"]} table {sha256_file(table)[:16]}')
if len(anchors) != 1:
    sys.exit('the two policies do not share one owner anchor')
print(f'anchor sha256 {hashlib.sha256(anchors.pop()).hexdigest()}')
