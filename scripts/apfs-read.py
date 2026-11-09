#!/usr/bin/env python3
"""Read-only browsing of an unencrypted APFS container image (the Ace 3 Data disk from build-aston-data-image.py).

Stdlib only. Finds the newest valid container superblock in the checkpoint area, resolves the volume through the container's object
map, walks the volume's file-system tree once and then lists or extracts files. Never writes the image. Encrypted volumes, compressed
(decmpfs) files and snapshots are not handled: compressed files are reported, encrypted volumes refused.

usage: apfs-read.py IMAGE [--volume N] ls PATH            list a directory (size, modification time, name)
       apfs-read.py IMAGE [--volume N] find SUBSTRING     every path containing SUBSTRING
       apfs-read.py IMAGE [--volume N] get PATH OUTFILE   copy one file out
       apfs-read.py IMAGE info                            volumes and their counters
"""
import argparse
import datetime
import struct
import sys

OBJ = struct.Struct('<QQQII')
J_INODE, J_XATTR, J_EXTENT, J_DREC = 3, 4, 8, 9
ROOT_DIR = 2


def fletcher64(block):
    words = struct.unpack_from(f'<{len(block) // 4}I', block)
    s1 = s2 = 0
    for w in words[2:]:
        s1 = (s1 + w) % 0xFFFFFFFF
        s2 = (s2 + s1) % 0xFFFFFFFF
    c1 = 0xFFFFFFFF - (s1 + s2) % 0xFFFFFFFF
    c2 = 0xFFFFFFFF - (s1 + c1) % 0xFFFFFFFF
    return c2 << 32 | c1


class Container:
    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.read(0, 4096)
        if head[32:36] != b'NXSB':
            raise SystemExit('no APFS container superblock at block 0')
        self.bs = struct.unpack_from('<I', head, 36)[0]
        base, blocks = struct.unpack_from('<Q', head, 112)[0], struct.unpack_from('<I', head, 104)[0] & 0x7FFFFFFF
        best = None
        for b in range(base, base + blocks):
            blk = self.block(b)
            cksum, oid, xid, otype, _ = OBJ.unpack_from(blk)
            if blk[32:36] == b'NXSB' and otype & 0xFFFF == 1 and cksum == fletcher64(blk) and (best is None or xid > best[0]):
                best = (xid, blk)
        if best is None:
            raise SystemExit('no valid container superblock in the checkpoint area')
        self.xid, self.nx = best
        self.omap = self.omap_root(struct.unpack_from('<Q', self.nx, 160)[0])
        self.fs_oids = [o for o in struct.unpack_from('<100Q', self.nx, 184) if o]

    def read(self, offset, length):
        self.f.seek(offset)
        return self.f.read(length)

    def block(self, n):
        return self.read(n * (self.bs if hasattr(self, 'bs') else 4096), self.bs if hasattr(self, 'bs') else 4096)

    def omap_root(self, omap_paddr):
        return struct.unpack_from('<Q', self.block(omap_paddr), 48)[0]

    # ---- B-trees ----
    def node(self, paddr):
        blk = self.block(paddr)
        flags, level, nkeys = struct.unpack_from('<HHI', blk, 32)
        toff, tlen = struct.unpack_from('<HH', blk, 40)
        keys_at = 56 + toff + tlen
        vals_end = self.bs - (40 if flags & 1 else 0)
        entries = []
        for i in range(nkeys):
            if flags & 4:
                koff, voff = struct.unpack_from('<HH', blk, 56 + toff + 4 * i)
                klen, vlen = 16, (16 if flags & 2 else 8)
            else:
                koff, klen, voff, vlen = struct.unpack_from('<HHHH', blk, 56 + toff + 8 * i)
            key = blk[keys_at + koff:keys_at + koff + klen]
            val = blk[vals_end - voff:vals_end - voff + vlen] if voff != 0xFFFF else b''
            entries.append((key, val))
        return flags, level, entries

    def omap_lookup(self, root, oid, xid):
        paddr = root
        while True:
            flags, level, entries = self.node(paddr)
            pick = None
            for key, val in entries:
                k_oid, k_xid = struct.unpack_from('<QQ', key)
                if (k_oid, k_xid) <= (oid, xid):
                    pick = (k_oid, k_xid, val)
                else:
                    break
            if pick is None:
                return None
            if level == 0:
                return struct.unpack_from('<IIQ', pick[2])[2] if pick[0] == oid else None
            paddr = struct.unpack_from('<Q', pick[2])[0]

    def walk(self, root, resolve):
        """Every (key, value) of a tree whose child pointers are virtual (resolve maps them to blocks)."""
        stack = [root]
        while stack:
            flags, level, entries = self.node(stack.pop())
            if level == 0:
                yield from entries
            else:
                stack.extend(resolve(struct.unpack_from('<Q', val)[0]) for _, val in reversed(entries))


class Volume:
    def __init__(self, nx, index):
        self.nx = nx
        oid = nx.fs_oids[index]
        paddr = nx.omap_lookup(nx.omap, oid, nx.xid)
        self.sb = nx.block(paddr)
        if self.sb[32:36] != b'APSB':
            raise SystemExit(f'volume {index}: no APSB at block {paddr}')
        self.name = self.sb[704:960].split(b'\0')[0].decode(errors='replace')
        self.flags = struct.unpack_from('<Q', self.sb, 264)[0]
        # case- or normalization-insensitive volumes key directory entries by a name hash (j_drec_hashed_key_t)
        self.hashed = struct.unpack_from('<Q', self.sb, 56)[0] & 0x9 != 0
        self.role = struct.unpack_from('<H', self.sb, 964)[0]
        self.vomap = nx.omap_root(struct.unpack_from('<Q', self.sb, 128)[0])
        self.root_tree = struct.unpack_from('<Q', self.sb, 136)[0]
        self.counts = struct.unpack_from('<4Q', self.sb, 184)
        self.loaded = False

    def resolve(self, oid):
        # a sealed volume's file-system tree is physical (root_tree_type OBJ_PHYSICAL): its pointers are block numbers
        if struct.unpack_from('<I', self.sb, 116)[0] & 0x40000000:
            return oid
        paddr = self.nx.omap_lookup(self.vomap, oid, self.nx.xid)
        if paddr is None:
            raise SystemExit(f'object {oid:#x} not in the volume object map')
        return paddr

    def load(self):
        if self.flags & 1 == 0:
            raise SystemExit(f'volume "{self.name}" is encrypted; this reader handles only unencrypted volumes')
        self.children, self.inodes, self.extents, self.xattrs = {}, {}, {}, {}
        for key, val in self.nx.walk(self.resolve(self.root_tree), self.resolve):
            hdr = struct.unpack_from('<Q', key)[0]
            oid, kind = hdr & 0x0FFFFFFFFFFFFFFF, hdr >> 60
            if kind == J_DREC:
                if self.hashed:
                    n = struct.unpack_from('<I', key, 8)[0] & 0x3FF
                    name = key[12:12 + n].rstrip(b'\0').decode(errors='replace')
                else:
                    n = struct.unpack_from('<H', key, 8)[0]
                    name = key[10:10 + n].rstrip(b'\0').decode(errors='replace')
                self.children.setdefault(oid, {})[name] = struct.unpack_from('<Q', val)[0]
            elif kind == J_INODE:
                private, = struct.unpack_from('<Q', val, 8)
                mtime, = struct.unpack_from('<Q', val, 24)
                mode, = struct.unpack_from('<H', val, 80)
                size = self.dstream_size(val[92:])
                self.inodes[oid] = (private, mode, size, mtime, struct.unpack_from('<Q', val, 56)[0])
            elif kind == J_EXTENT:
                logical, = struct.unpack_from('<Q', key, 8)
                length, phys = struct.unpack_from('<QQ', val)
                self.extents.setdefault(oid, []).append((logical, length & 0x00FFFFFFFFFFFFFF, phys))
            elif kind == J_XATTR:
                n, = struct.unpack_from('<H', key, 8)
                self.xattrs.setdefault(oid, set()).add(key[10:10 + n].rstrip(b'\0').decode(errors='replace'))
        self.loaded = True

    @staticmethod
    def dstream_size(xf):
        if len(xf) < 4:
            return 0
        count, _used = struct.unpack_from('<HH', xf)
        heads = [struct.unpack_from('<BBH', xf, 4 + 4 * i) for i in range(count)]
        at = 4 + 4 * count
        for kind, _flags, size in heads:
            if kind == 8:  # INO_EXT_TYPE_DSTREAM
                return struct.unpack_from('<Q', xf, at)[0]
            at += (size + 7) & ~7
        return 0

    def lookup(self, path):
        oid = ROOT_DIR
        for part in [p for p in path.split('/') if p]:
            entries = self.children.get(oid, {})
            if part not in entries:
                match = [n for n in entries if n.lower() == part.lower()]
                if not match:
                    raise SystemExit(f'{path}: "{part}" not found')
                part = match[0]
            oid = entries[part]
        return oid

    def paths(self, oid=ROOT_DIR, prefix=''):
        for name, child in sorted(self.children.get(oid, {}).items()):
            path = f'{prefix}/{name}'
            yield path, child
            if child in self.children:
                yield from self.paths(child, path)

    def data(self, oid):
        private, mode, size, _, _ = self.inodes[oid]
        if 'com.apple.decmpfs' in self.xattrs.get(oid, ()):
            raise SystemExit('compressed (decmpfs) file: not handled')
        out = bytearray(size)
        bs = self.nx.bs
        for logical, length, phys in sorted(self.extents.get(private, [])):
            if logical >= size:
                continue
            take = min(length, size - logical)
            if phys:
                out[logical:logical + take] = self.nx.read(phys * bs, take)
        return bytes(out)


def stamp(ns):
    return datetime.datetime.fromtimestamp(ns / 1e9, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image')
    parser.add_argument('--volume', type=int, default=0)
    parser.add_argument('command', choices=('info', 'ls', 'find', 'get'))
    parser.add_argument('args', nargs='*')
    a = parser.parse_args()
    nx = Container(a.image)
    if a.command == 'info':
        print(f'container xid {nx.xid}, block size {nx.bs}, {len(nx.fs_oids)} volumes')
        for i in range(len(nx.fs_oids)):
            v = Volume(nx, i)
            print(f'  [{i}] "{v.name}" role {v.role:#x} {"unencrypted" if v.flags & 1 else "ENCRYPTED"} '
                  f'files {v.counts[0]} dirs {v.counts[1]} symlinks {v.counts[2]} other {v.counts[3]}')
        return
    v = Volume(nx, a.volume)
    v.load()
    if a.command == 'ls':
        oid = v.lookup(a.args[0] if a.args else '/')
        for name, child in sorted(v.children.get(oid, {}).items()):
            _, mode, size, mtime, _ = v.inodes.get(child, (0, 0, 0, 0, 0))
            kind = 'd' if mode & 0o170000 == 0o040000 else 'l' if mode & 0o170000 == 0o120000 else '-'
            print(f'{kind} {size:>12} {stamp(mtime)}  {name}')
    elif a.command == 'find':
        needle = a.args[0].lower()
        for path, child in v.paths():
            if needle in path.lower():
                _, _, size, mtime, _ = v.inodes.get(child, (0, 0, 0, 0, 0))
                print(f'{size:>12} {stamp(mtime)}  {path}')
    elif a.command == 'get':
        blob = v.data(v.lookup(a.args[0]))
        with open(a.args[1], 'wb') as out:
            out.write(blob)
        print(f'{a.args[0]}: {len(blob)} bytes -> {a.args[1]}')


if __name__ == '__main__':
    sys.exit(main())
