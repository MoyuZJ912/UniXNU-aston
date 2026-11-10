#!/usr/bin/env python3
"""Stage the original IOAcceleratorES ABI of this profile's kernelcache before build-adreno740-accelerator.py runs.

That builder reads build/gpu-analysis/ioaccelerator-base-abi-24A446.json and, when the file is missing, seeds it from UniXNU's
scripts/data record, which holds iPad17,3 addresses. For the iPhone17,2 kernelcache this board ships the same record ported with
markakash/UniXNU-ishtar scripts/kc-port.py (5 addresses verified, identical to ishtar's own port). The record is chosen by the root
OS volume of the selected profile and must name that kernelcache; the iPad profile keeps UniXNU's record.

usage (an archive step, run in the tree): stage-aston-accelerator-abi.py
"""
import json
import os
import sys
from pathlib import Path

RECORDS = {'2F8E6EED-68D5-4261-B8A3-12571BAA2DD4': None,
           'E3F4AED0-4454-4D5B-833C-E27E99C4F67B': ('ioaccelerator-base-abi-24A446-d94ap.json',
                                                    'fa07057bf34f33af1ec26681ad0c87c13e5aaa548a300740eddb0e92876e5f95')}
root_os = os.environ.get('UNIXNU_ROOT_OS_UUID', '').upper()
if root_os not in RECORDS:
    sys.exit(f'No accelerator ABI for the root OS volume {root_os or "(UNIXNU_ROOT_OS_UUID unset)"}')
if RECORDS[root_os] is None:
    print('iPad17,3: UniXNU\'s scripts/data record applies')
    sys.exit(0)
name, kernelcache = RECORDS[root_os]
record = json.loads((Path(__file__).resolve().with_name(name)).read_text())
if record['kernelcache_sha256'] != kernelcache:
    sys.exit(f'{name} names kernelcache {record["kernelcache_sha256"]}, not {kernelcache}')
target = Path.cwd() / 'build/gpu-analysis/ioaccelerator-base-abi-24A446.json'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps(record, indent=2) + '\n')
print(f'staged {name} as {target.relative_to(Path.cwd())}')
