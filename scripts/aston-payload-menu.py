#!/usr/bin/env python3
"""Rewrites the menu of a `unixnu build payload` volume for the Ace 3 (in place, with mtools).

On this phone the payload volume is the only configuration UniXNU sees when it is present (Ace3-UniXNU efi/AstonConfigDxe
publishes it instead of the firmware's own menu), so it must keep the way back to Android: the generated menu boots iOS by
default after 3 s and has no "Switch to slot B". The new menu keeps the generated iOS entries unchanged (kernel, initrd,
arguments, the install confirmation) and adds the firmware's slot switch, which is the default after 10 s.

--variant 'ID|TITLE|EDITS' adds an entry after "xnu" that boots the same kernel and Data with changed arguments, so one image
can compare settings on the device. EDITS are space separated: KEY=VALUE sets (or replaces) KEY, !KEY drops it (and must name
an argument the generated entry has). Nothing else of the entry changes.

usage: aston-payload-menu.py payload.img [--timeout 10] [--default slot-b] [--variant 'ID|TITLE|EDITS' ...]
Prints the SHA-256 of the rewritten volume; writes payload.img.menu.json beside it.
"""
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import tomllib
from pathlib import Path

CONFIG = '::/EFI/UniXNU/unixnu.toml'
# Ace3-UniXNU efi/AstonSlotSwitch (FILE_GUID), resolved by UniXNU from the firmware volume.
SLOT_SWITCH = 'fv:7C4F0B9E-3D2A-4B61-9E85-2F6A1C0D7B43'
KEEP_TYPES = ('xnu',)


def value(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, list) and all(isinstance(x, str) for x in v):
        return '[' + ', '.join(json.dumps(x, ensure_ascii=False) for x in v) + ']'
    raise SystemExit(f'unsupported value in the generated menu: {v!r}')


def table(name, items):
    lines = [f'[{name}]']
    for key, v in items.items():
        if isinstance(v, dict):
            raise SystemExit(f'nested table {name}.{key} in the generated menu')
        lines.append(f'{key} = {value(v)}')
    return '\n'.join(lines) + '\n'


def edit_args(line, edits):
    words = line.split()
    for edit in edits.split():
        if edit.startswith('!'):
            key = edit[1:]
            if not any(w.split('=', 1)[0] == key for w in words):
                raise SystemExit(f'a variant drops {key}, which the generated arguments do not have')
            words = [w for w in words if w.split('=', 1)[0] != key]
        else:
            key = edit.split('=', 1)[0]
            if any(w.split('=', 1)[0] == key for w in words):
                words = [edit if w.split('=', 1)[0] == key else w for w in words]
            else:
                words.append(edit)
    return ' '.join(words)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path)
    parser.add_argument('--timeout', type=int, default=10)
    parser.add_argument('--default', default='slot-b', choices=('slot-b', 'xnu'))
    parser.add_argument('--variant', action='append', default=[], metavar="'ID|TITLE|EDITS'")
    args = parser.parse_args()
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    text = subprocess.run(['mtype', '-i', str(args.image), CONFIG], env=env, check=True, capture_output=True).stdout
    generated = tomllib.loads(text.decode())
    entries = [e for e in generated.get('entry', []) if e.get('type') in KEEP_TYPES]
    if not entries or not any(e.get('id') == 'xnu' for e in entries):
        raise SystemExit('the generated menu has no xnu entry: not a `unixnu build payload` volume')
    for key in generated:
        if key not in ('version', 'boot', 'kits', 'entry', 'ui'):
            raise SystemExit(f'unexpected section [{key}] in the generated menu: review this script first')
    boot = dict(generated.get('boot', {}))
    boot.update(timeout=args.timeout, default=args.default, rotate=0)
    out = f'version = {value(generated.get("version", 1))}\n\n'
    out += '# Rewritten by UniXNU-aston scripts/aston-payload-menu.py: the way back to Android stays the default.\n'
    out += table('boot', boot) + '\n'
    if 'ui' in generated:
        out += table('ui', generated['ui']) + '\n'
    out += table('kits', generated.get('kits', {'autoload': False})) + '\n'
    for entry in entries:
        out += table('[entry]', entry) + '\n'
        if entry.get('id') == 'xnu':
            for spec in args.variant:
                vid, title, edits = spec.split('|', 2)
                out += table('[entry]', dict(entry, id=vid, title=title, args=edit_args(entry['args'], edits))) + '\n'
    out += table('[entry]', {'id': 'slot-b', 'title': 'Switch to slot B', 'type': 'efi', 'path': SLOT_SWITCH}) + '\n'
    out += table('[entry]', {'id': 'shutdown', 'title': 'Power off', 'type': 'action', 'action': 'shutdown'})
    check = tomllib.loads(out)
    ids = [e['id'] for e in check['entry']]
    assert check['boot']['default'] in ids and len(set(ids)) == len(ids)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'unixnu.toml'
        path.write_text(out, encoding='utf-8', newline='\n')
        subprocess.run(['mcopy', '-o', '-m', '-i', str(args.image), str(path), CONFIG], env=env, check=True, capture_output=True)
    back = subprocess.run(['mtype', '-i', str(args.image), CONFIG], env=env, check=True, capture_output=True).stdout.decode()
    assert back == out, 'readback of the rewritten menu differs'
    digest = hashlib.sha256(args.image.read_bytes()).hexdigest()
    report = {'image': args.image.name, 'sha256': digest, 'bytes': args.image.stat().st_size, 'entries': ids,
              'default': args.default, 'timeout': args.timeout, 'generated_entries': [e.get('id') for e in generated.get('entry', [])]}
    Path(str(args.image) + '.menu.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out)
    print(f'menu rewritten: {ids}, default {args.default}; payload sha256 {digest}')


if __name__ == '__main__':
    main()
