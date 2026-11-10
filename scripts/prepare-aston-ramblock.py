#!/usr/bin/env python3
"""Stage the pinned RamBlock publication fix in this build tree only.

Run before either ShengUfs or RamBlock compiles: the source storage recipe also
compiles NativeRamBlock.cpp. Never patch the shared component checkout in place.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = '60a54ce8f86161db958fd8e6edf577081b3c9ae7'
SOURCE_SHA256 = '8e47a517e5c30ae85e0a706ff97ef7710aacadfaa7c6c4da5a22b52f4880b2f0'
PATCH = Path(__file__).resolve().parent / 'patches/ramblock-publish-after-store.patch'


def stage(tree):
    tree = Path(tree).resolve()
    link = tree / 'drivers/RamBlock'
    output = tree / 'build/aston-ramblock'
    report = output / 'source.json'
    # A normal tree assembly restores the original link. A repeated archive
    # invocation uses the recorded original checkout, never the patched copy.
    if not link.is_symlink():
        raise ValueError(f'{link}: expected the resolver-created component symlink')
    source = link.resolve()
    if source.parent == output:
        source = Path(json.loads(report.read_text())['source']).resolve()
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise ValueError(f'RamBlock revision {revision} differs from reviewed {PIN}')
    if subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--',
                       '*.cpp', '*.h', '*.plist']).returncode:
        raise ValueError('RamBlock has local source changes; review overrides before staging')
    code = (source / 'NativeRamBlock.cpp').read_text(encoding='utf-8')
    if hashlib.sha256(code.encode()).hexdigest() != SOURCE_SHA256:
        raise ValueError('RamBlock source differs from the reviewed input; inspect local overrides')
    patch_hash = hashlib.sha256(PATCH.read_bytes()).hexdigest()
    # A separate directory for each patch identity keeps prior build inputs
    # available for review and avoids deleting or editing a shared source tree.
    staged = output / patch_hash
    staged.mkdir(parents=True, exist_ok=True)
    inputs = {}
    for item in source.iterdir():
        if item.is_file() and (item.suffix in ('.h', '.cpp', '.plist') or item.name in ('LICENSE', 'README.md')):
            shutil.copyfile(item, staged / item.name)
            inputs[item.name] = hashlib.sha256(item.read_bytes()).hexdigest()
    (staged / 'NativeRamBlock.cpp').write_text(code, encoding='utf-8', newline='\n')
    command = ['git', 'apply', '--no-index', '--whitespace=error-all',
               '--directory', staged.relative_to(tree).as_posix(), str(PATCH)]
    subprocess.run(command[:-1] + ['--check', command[-1]], cwd=tree, check=True)
    subprocess.run(command, cwd=tree, check=True)
    result = dict(source=str(source), revision=revision, source_sha256=SOURCE_SHA256,
                  patch_sha256=patch_hash, inputs=inputs, staged=str(staged),
                  staged_sha256=hashlib.sha256((staged / 'NativeRamBlock.cpp').read_bytes()).hexdigest())
    # Only unlink the build-tree link after the copy and patch have succeeded.
    link.unlink()
    link.symlink_to(staged, target_is_directory=True)
    report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'RamBlock: staged publication fix from {revision}; {report}')
    return staged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    stage(args.tree)


if __name__ == '__main__':
    main()
