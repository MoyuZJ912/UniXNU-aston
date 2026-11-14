#!/usr/bin/env python3
"""Compile the aston board sources with a kernel tree's recorded compiler flags.

A syntax, type and warning check of the board code, run where the full kernel of the
selected OS cannot be built (the 24A446 kernel needs Xcode's clang; a Linux host builds
the iOS 17 kernel, whose compile records carry the same IOKit and arm64 kernel ABI for
these sources). The flags are those of scripts/build-gop-legacy-display-27.py:
compile_cpp for C++ and compile_c for C. The objects are discarded; nothing is linked.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

CPP = ['TouchProbe', 'ButtonProbe', 'BatteryProbe', 'ShengBattery', 'AstonDisplayWindow', 'AstonStorageTraits', 'AstonPanel', 'AstonDisplaySurvey', 'AppleARMBacklight', 'BacklightProbe', 'CpuFreqProbe']
C = ['S3910', 'AstonGaugeBus', 'AstonGauge', 'BootLog', 'AstonHaltRestart', 'AstonSocBackup']
# With BSD flags, as build-sheng-source-storage.py compiles them (the VFS readers).
BSD = ['WifiFirmware']


def recipe(tree, name):
    path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / name
    return json.loads(path.read_text().strip().rstrip(','))


def command(record, source, output, extra):
    cmd, args, i = [], record['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == record['file']:
            cmd.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ', '-MQ'):
            if value == '-o':
                cmd += ['-o', str(output)]
            i += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    return cmd + extra


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', required=True, type=Path, help='kernel tree with compile records (build/xnu/ios-obj)')
    parser.add_argument('--device', required=True, type=Path, help='UniXNU-aston checkout')
    parser.add_argument('--unixnu', type=Path, help='UniXNU checkout (default: the one holding --tree)')
    parser.add_argument('--out', type=Path, default=Path('/tmp/aston-board-objects'))
    parser.add_argument('sources', nargs='*', help='stems to compile (default: all board sources this check knows)')
    args = parser.parse_args()
    tree, device = args.tree.resolve(), args.device.resolve()
    unixnu = (args.unixnu or tree.parents[4]).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    cpp_record = recipe(tree, 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    c_record = recipe(tree, 'osfmk/DEVELOPMENT/startup.o.json')
    bsd_record = recipe(tree, 'bsd/DEVELOPMENT/kern_exec.o.json')
    common = ['-I', str(tree / '.cache/xnu-work/osfmk'), '-I', str(device / 'package'), '-I', str(unixnu / 'drivers/QcomGeniSpi')]
    soc = sorted((unixnu / 'out/feeds').glob('UniXNU-sm8550-common-*/kits/Sm8550Platform'))
    if soc:
        common += ['-I', str(soc[-1])]
    # Upstream clang (the Linux toolchain) adds -Wmissing-designated-field-initializers to -Wextra; XNU's own kalloc type macros
    # trip it in every IOKit header. Apple clang, which builds the kernel these objects go into, has no such warning in -Wextra.
    host = ['-Wno-missing-designated-field-initializers']
    cpp_extra = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
                 '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', *host, *common]
    c_extra = ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', *host, *common]
    wanted = set(args.sources)
    failures = 0
    for stems, record, extra, suffix in ((CPP, cpp_record, cpp_extra, '.cpp'), (C, c_record, c_extra, '.c'),
                                         (BSD, bsd_record, c_extra, '.c')):
        for stem in stems:
            if wanted and stem not in wanted:
                continue
            source = device / 'package' / (stem + suffix)
            output = args.out / (stem + '.o')
            result = subprocess.run(command(record, source, output, extra), cwd=record['directory'], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                failures += 1
                print(f'FAIL {stem}{suffix}\n{result.stdout[-6000:]}')
            else:
                print(f'PASS {stem}{suffix}')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
