#!/usr/bin/env python3
"""Derive XnuPrelink reviewed pins for the iPhone17,2 kernelcache from the iPad17,3 pairing of the same build.

Kexts are the same binaries in both 24A446 collections, linked at different addresses, so a kext pointer slot at a given
(entry, segment, offset) is the same import in both. For every kernel target the iPhone17,2 prelink leaves unmapped
(kernel.json "missing") or binds to the unmapped-call stub ("stubbed"), the kext slots that name it are read at the same
place in the iPad17,3 collection, and the iPad target is looked up in an iPad17,3 pairing of the same native kernel. A
target is pinned only when all its slots in kexts present in both collections agree on one paired native symbol. Function
pins carry the original size and sha256 from the iPhone kernel's function starts, which XnuPrelink checks.

  scripts/derive-fileset-pins.py --xnuprelink UniXNU/kits/XnuPrelink --iphone kernelcache.iphone17 --ipad kernelcache.ipad17 \\
      --ipad-map ipad/kernel.map.json --report iphone/kernel.json --pins scripts/fileset-24A446-d94ap-symbols.json

The pairing and report come from emit_prelink.py runs on the CI native kernel (the prelink-inputs artifact of a failed
build); an iPad run only has to get as far as writing its map.
"""
import argparse
import bisect
import hashlib
import json
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--xnuprelink', required=True, help='kits/XnuPrelink of the UniXNU checkout (kc_image.py)')
    parser.add_argument('--iphone', required=True, help='iPhone17,2 kernelcache (decompressed)')
    parser.add_argument('--ipad', required=True, help='iPad17,3 kernelcache of the same build (decompressed)')
    parser.add_argument('--ipad-map', required=True, help='emit_prelink.py pairing (kernel.map.json) against the iPad17,3 cache')
    parser.add_argument('--report', required=True, help='emit_prelink.py report (kernel.json) against the iPhone17,2 cache')
    parser.add_argument('--pins', required=True, help='fileset-*-symbols.json to extend in place')
    args = parser.parse_args()
    sys.path.insert(0, args.xnuprelink)
    from kc_image import Image

    report = json.load(open(args.report))
    missing = {int(a, 16) for d in report['missing'].values() for a in d}
    stubbed = {int(a, 16) for d in report['stubbed'].values() for a in d}
    pairing = json.load(open(args.ipad_map))
    paired = {int(k, 16): v for kind in ('data', 'functions') for k, v in pairing[kind].items()}

    def load(path):
        cache = Image(open(path, 'rb').read())
        segments = []
        for name, (_, offset) in cache.entries.items():
            if name == 'com.apple.kernel':
                continue
            for s in Image(cache.data, offset).segments:
                if s['size']:
                    segments.append((s['va'], s['va'] + s['size'], name, s['name']))
        return cache, segments, cache.fixups()
    iphone, iphone_segments, iphone_fixups = load(args.iphone)
    ipad, ipad_segments, ipad_fixups = load(args.ipad)
    ipad_base = {(name, segment): begin for begin, _, name, segment in ipad_segments}
    sites = {}
    for site, (target, *_) in iphone_fixups.items():
        if target in missing or target in stubbed:
            sites.setdefault(target, []).append(site)

    def identify(target):
        """The paired iPad symbols the target's kext slots name, with the kexts naming each."""
        names = {}
        for site in sites.get(target, ()):
            where = next(((n, s, site - b) for b, e, n, s in iphone_segments if b <= site < e), None)
            if where is None or (where[0], where[1]) not in ipad_base:
                continue   # a kext the iPad collection does not carry
            found = ipad_fixups.get(ipad_base[where[:2]] + where[2])
            pair = paired.get(found[0]) if found else None
            key = (pair['name'], pair['how']) if pair else None
            names.setdefault(key, set()).add(where[0].rsplit('.', 1)[-1])
        return names

    kernel = Image(iphone.data, iphone.entries['com.apple.kernel'][1])
    starts = sorted(kernel.function_starts())

    def read(address, size):
        segment = next(s for s in iphone.segments if s['va'] <= address < s['va'] + s['size'])
        offset = segment['fileoff'] + address - segment['va']
        return iphone.data[offset:offset + size]

    pins = json.load(open(args.pins))
    if pins['inputs']['kernelcache_sha256'] != hashlib.sha256(iphone.data).hexdigest():
        sys.exit('the pins name another kernelcache')
    pinned = {p['original'] for kind in ('functions', 'data') for p in pins[kind]}
    added = 0
    for target in sorted(missing | stubbed):
        names = identify(target)
        if len(names) != 1 or None in names:
            print(f'{target:#x}: not pinned ({"no slots in shared kexts" if not names else ", ".join(str(k) for k in names)})')
            continue
        (symbol, how), kexts = next(iter(names.items()))
        original = f'{target:#x}'
        if original in pinned:
            continue
        evidence = (f"Kext pointers name this address ({', '.join(sorted(kexts)[:6])}); at the same entry, segment and offset the "
                    f"iPad17,3 38e6d9fa collection points at what XnuPrelink pairs with {symbol} ({how}). Kexts are the same binaries "
                    f"in both collections, so the slot is the same import. Derived by UniXNU-aston scripts/derive-fileset-pins.py.")
        if target in stubbed:
            index = bisect.bisect_right(starts, target)
            if not index or starts[index - 1] != target or index == len(starts):
                print(f'{original}: not a function start of the iPhone kernel')
                continue
            size = starts[index] - target
            pins['functions'].append({'original': original, 'original_size': size,
                                      'original_sha256': hashlib.sha256(read(target, size)).hexdigest(),
                                      'native_symbol': symbol, 'evidence': evidence + ' Size and sha256 from its function starts.'})
        else:
            pins['data'].append({'original': original, 'native_symbol': symbol, 'addend': 0, 'evidence': evidence})
        print(f'{original}: {symbol} ({len(kexts)} kexts)')
        added += 1
    with open(args.pins, 'w') as out:
        json.dump(pins, out, indent=2)
        out.write('\n')
    print(f'{added} pins added; {len(pins["functions"])} functions, {len(pins["data"])} data')


if __name__ == '__main__':
    main()
