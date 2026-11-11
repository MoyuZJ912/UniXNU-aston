#!/usr/bin/env python3
"""Owner overlays for sealed system files, built from the mounted OS image ([payload.runtime] overlay_tool).

configd's InterfaceNamer publishes Plugin:InterfaceNamer once IOKit is quiet with IONetworkStack open, once the interfaces of the
previous boot are named, or when its timer runs out: WaitStackTimeout (300 s) while IONetworkStack is missing, WaitQuietTimeout
(240 s) after it appears. SCNetworkInterfaceCopyAll() waits for that key without a timeout of its own, and Settings calls it on its
main thread at launch. This kernel has no IONetworkingFamily, so Settings hung until 300 s after configd started (iOS boot 7: configd
at 55 s, three Settings launches blocked in SCDynamicStoreNotifyWait and killed by the app watchdog).

The overlay is the plugin's own Info.plist with both timeouts shortened; the kernel mounts it over
/System/Library/SystemConfiguration/InterfaceNamer.bundle (patches/xnu-12377/aston-interface-namer-overlay.patch). configd reads the
plugin's Info.plist through CFBundle; the plugin code is built into configd.

usage: aston-runtime-overlays.py OUT --os-root ROOT
Writes OUT/ifnamer/Info.plist and OUT/report.json ({"directories": {addon prefix: directory below OUT}}).
"""
import argparse
import json
import plistlib
from pathlib import Path

PLUGIN = 'System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist'
TIMEOUTS = {'WaitStackTimeout': 5.0, 'WaitQuietTimeout': 10.0}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out', type=Path)
    parser.add_argument('--os-root', required=True, type=Path)
    args = parser.parse_args()
    source = args.os_root / PLUGIN
    raw = source.read_bytes()
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    info = plistlib.loads(raw)
    if info.get('CFBundleIdentifier') != 'com.apple.SystemConfiguration.InterfaceNamer':
        raise SystemExit(f'{source}: not the InterfaceNamer plugin ({info.get("CFBundleIdentifier")!r})')
    before = {key: info.get(key) for key in TIMEOUTS}
    info.update(TIMEOUTS)
    target = args.out / 'ifnamer/Info.plist'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(plistlib.dumps(info, fmt=fmt, sort_keys=False))
    if plistlib.loads(target.read_bytes()) != info:
        raise SystemExit('rewritten Info.plist does not read back')
    (args.out / 'report.json').write_text(json.dumps({'directories': {'ifnamer': 'ifnamer'}}, indent=1) + '\n')
    print(f'InterfaceNamer timeouts {before} -> {TIMEOUTS} ({"binary" if fmt == plistlib.FMT_BINARY else "XML"} plist)')


if __name__ == '__main__':
    main()
