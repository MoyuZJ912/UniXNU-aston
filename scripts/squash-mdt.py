#!/usr/bin/env python3
"""Join a Qualcomm split firmware image (name.mdt plus name.bNN segment files) into one ELF (.mbn).

The .mdt holds the ELF header, the program headers and usually the hash segment; segment N's bytes are in name.bNN
and belong at that program header's p_offset. A segment the .mdt already holds in full is taken from it. Only reads
the inputs and writes the output; the image is never executed or modified otherwise.
"""
import struct
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: squash-mdt.py name.mdt out.mbn')
    mdt, out = Path(sys.argv[1]), Path(sys.argv[2])
    head = mdt.read_bytes()
    if head[:4] != b'\x7fELF' or head[4] != 1:
        sys.exit(f'{mdt}: not a 32-bit ELF')
    phoff, = struct.unpack_from('<I', head, 28)
    phentsize, phnum = struct.unpack_from('<HH', head, 42)
    if phentsize != 32 or phoff + phnum * 32 > len(head):
        sys.exit(f'{mdt}: program headers outside the file')
    segments = [struct.unpack_from('<8I', head, phoff + i * 32) for i in range(phnum)]
    size = max([len(head)] + [offset + filesz for _, offset, _, _, filesz, _, _, _ in segments])
    image = bytearray(size)
    image[:len(head)] = head
    for i, (_, offset, _, _, filesz, _, _, _) in enumerate(segments):
        if not filesz:
            continue
        part = mdt.with_suffix(f'.b{i:02d}')
        if part.is_file():
            data = part.read_bytes()
            if len(data) != filesz:
                sys.exit(f'{part}: {len(data)} bytes, program header {i} says {filesz}')
            image[offset:offset + filesz] = data
        elif offset + filesz > len(head):
            sys.exit(f'{part} missing and segment {i} is not inside {mdt.name}')
    out.write_bytes(bytes(image))
    print(f'{out}: {len(image)} bytes from {mdt.name} and {phnum} program headers')


if __name__ == '__main__':
    main()
