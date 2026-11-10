#!/usr/bin/env python3
"""Stage this board's signed root policies as the tree's build/verified-root-<build> before any storage step runs.

build-sheng-source-storage.py looks there first. component:ShengUfs runs it without --policy-dir, and without that directory it
searches scripts/policies/*<build>, which in an iOS tree matches both UniXNU's ipados-27.0.1-24A446 and this board's
ios-27.0.1-24A446 and stops. The staged copy holds only the signed headers, the public anchor and the verification records.

usage (an archive step, run in the tree): stage-aston-root-policies.py scripts/policies/ios-27.0.1-24A446
"""
import json
import os
import shutil
import sys
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit(__doc__)
build = os.environ.get('UNIXNU_IPSW_BUILD')
if not build:
    sys.exit('UNIXNU_IPSW_BUILD is not set; run through unixnu build kernel')
source = Path(sys.argv[1]).resolve()
target = Path.cwd() / f'build/verified-root-{build}'
for name, prefix in (('OS', 'UNIXNU_ROOT_OS'), ('SystemOS', 'UNIXNU_ROOT_SYSTEMOS')):
    verification = json.loads((source / name / 'verification.json').read_text())
    if verification['image_sha256'] != os.environ.get(prefix + '_SHA256') or \
            verification['volume_uuid'] != os.environ.get(prefix + '_UUID', '').upper():
        sys.exit(f'{source / name} does not describe the profile\'s {name} image')
if target.exists():
    shutil.rmtree(target)
for name in ('OS', 'SystemOS'):
    (target / name).mkdir(parents=True)
    for file in ('manifest.header.bin', 'trusted-key.bin', 'verification.json'):
        shutil.copyfile(source / name / file, target / name / file)
print(f'staged {source.name} as {target.relative_to(Path.cwd())}')
