#!/usr/bin/env python3
"""Lays the installation seed (DBRDISK1) and the pages pull-aston-data.py copied into a sparse Data disk image (Linux).

usage: build-aston-data-image.py --seed data.dbr --pulled DIR --out data.img
The image is the whole Data disk (the seed's logical size); mount its APFS container read only (apfs-fuse).
"""
import argparse
import json
import struct
from pathlib import Path

BLOCK = 4096


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--seed', required=True, type=Path)
    parser.add_argument('--pulled', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    blob = args.seed.read_bytes()
    magic, version, page, logical, count = struct.unpack_from('<8sIIQQ', blob)
    if magic != b'DBRDISK1' or version != 1 or page != BLOCK:
        raise SystemExit('not a DBRDISK1 seed')
    runs = json.loads((args.pulled / 'runs.json').read_text())
    store = json.loads((args.pulled / 'store.json').read_text())
    if store['pages'] != logical // BLOCK:
        raise SystemExit('the pulled store belongs to another seed size')
    with open(args.out, 'wb') as f:
        f.truncate(logical)
        cursor = 32 + 16 * count
        for index in range(count):
            offset, length = struct.unpack_from('<QQ', blob, 32 + 16 * index)
            f.seek(offset)
            f.write(blob[cursor:cursor + length])
            cursor += length
        with open(args.pulled / 'pages.bin', 'rb') as pages:
            for index, n in runs:
                f.seek(index * BLOCK)
                for _ in range(n):
                    f.write(pages.read(BLOCK))
            if pages.read(1):
                raise SystemExit('pages.bin is longer than its runs')
    print(f'{args.out}: {logical} bytes, seed {count} extents, {sum(n for _, n in runs)} changed pages')


if __name__ == '__main__':
    main()
