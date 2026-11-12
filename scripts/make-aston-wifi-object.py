#!/usr/bin/env python3
"""make-aston-wifi-object.py OUT --linux-firmware DIR --board FILE: the root image object "Wifi" that package/WifiFirmware.c reads.

The WCN7850 hw2.0 firmware is linux-firmware's (ath12k/WCN7850/hw2.0: amss.bin, m3.bin and the regulatory database of board-2.bin),
the files the reference board runs mainline ath12k with. The board data is this phone's own, /odm/etc/wifi/kiwi/bdwlan.elf (the file
OnePlus' cnss2 picks for a CN unit, qmi.c cnss_get_oplus_bdf_file_name). It is served as board.bin: ath12k looks a WCN7850 up in
board-2.bin by bus, PCI IDs, subsystem IDs, chip and board ID (board ID 0xff here), and only when no entry matches takes board.bin.
The board-2.bin written here therefore holds linux-firmware's regulatory element alone ("bus=pci"), which ath12k requires before the
board data, and no board entries, so a laptop card's calibration can never be chosen.
Every input is checked against scripts/data/aston-wifi-inputs.json. Nothing here or in OUT may be committed: pack OUT into the unixnu
image with `unixnu build userdata ... --object Wifi=OUT` (after the existing objects, so their offsets stay).

Object layout: block 0 "AWF1", u32 count, from byte 16 one 64-byte entry per file (name[24] NUL padded, u32 offset, u32 size,
sha256[32]), the SHA-256 of bytes 0..4063 in bytes 4064..4095; files at 4096-byte aligned offsets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b'QCA-ATH12K-BOARD'
IE_REGDB = 1


def board2_regdb_only(data):
    """linux-firmware board-2.bin with only its regulatory database elements."""
    if not data.startswith(MAGIC + b'\0'):
        raise SystemExit('board-2.bin: not an ath12k board file')
    start = (len(MAGIC) + 1 + 3) & ~3
    out, at, kept = bytearray(data[:start]), start, 0
    while at + 8 <= len(data):
        ie, length = struct.unpack_from('<II', data, at)
        end = at + 8 + ((length + 3) & ~3)
        if end > len(data):
            raise SystemExit('board-2.bin: truncated element')
        if ie == IE_REGDB:
            out += data[at:end]
            kept += 1
        at = end
    if kept != 1:
        raise SystemExit(f'board-2.bin: expected one regulatory element, found {kept}')
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--linux-firmware', type=Path, required=True, help='directory with amss.bin, m3.bin, board-2.bin')
    parser.add_argument('--board', type=Path, required=True, help="the phone's /odm/etc/wifi/kiwi/bdwlan.elf")
    args = parser.parse_args()
    pins = json.loads((ROOT / 'scripts/data/aston-wifi-inputs.json').read_text())

    def pinned(path, pin, what):
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if len(data) != pin['bytes'] or digest != pin['sha256']:
            raise SystemExit(f'{what} {path}: {len(data)} bytes sha256 {digest}, expected {pin["bytes"]} {pin["sha256"]}')
        return data

    lf = pins['linux_firmware']['files']
    files = [
        ('amss.bin', pinned(args.linux_firmware / 'amss.bin', lf['amss.bin'], 'linux-firmware')),
        ('m3.bin', pinned(args.linux_firmware / 'm3.bin', lf['m3.bin'], 'linux-firmware')),
        ('board-2.bin', board2_regdb_only(pinned(args.linux_firmware / 'board-2.bin', lf['board-2.bin'], 'linux-firmware'))),
        ('board.bin', pinned(args.board, pins['board'], 'board data')),
    ]
    if not files[3][1].startswith(b'\x7fELF'):
        raise SystemExit('board data is not an ELF board file')
    header = bytearray(4096)
    header[0:4] = b'AWF1'
    struct.pack_into('<I', header, 4, len(files))
    body, at = bytearray(), 4096
    for index, (name, data) in enumerate(files):
        struct.pack_into('<24sII32s', header, 16 + 64 * index, name.encode(), at, len(data), hashlib.sha256(data).digest())
        body += data + bytes((-len(data)) % 4096)
        at += len(data) + (-len(data)) % 4096
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
    image = bytes(header) + bytes(body)
    args.output.write_bytes(image)
    for name, data in files:
        print(f'{name:12s} {len(data):8d} {hashlib.sha256(data).hexdigest()}')
    print(f'{args.output}: {len(image)} bytes sha256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
