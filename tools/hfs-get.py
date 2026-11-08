#!/usr/bin/env python3
"""Read files from the phone's iOS root (HFS+ on userdata) through TWRP's adb, block by block.

  hfs-get.py ls  PATH            list a directory
  hfs-get.py get PATH [OUT]      copy a file (default: stdout)

  env HFS_DEV  (default /dev/block/mmcblk0p72, bonito userdata)
      HFS_IMG  read a local image instead of the phone (for testing)
      ADB      (default: doas .../platform-tools/adb)

Only reads. Walks the catalog B-tree, fetching just the nodes it needs; forks with more than
8 extents get the rest from the extents overflow file. Changes still in the HFS+ journal only
(not yet written in place) are not seen.
"""
import os, shlex, struct, subprocess, sys

DEV = os.environ.get('HFS_DEV', '/dev/block/mmcblk0p72')
IMG = os.environ.get('HFS_IMG')
ADB = shlex.split(os.environ.get('ADB', 'doas /home/ada/codestuff/tools/platform-tools/adb'))
BS = 4096
_cache = {}


def read(off, n):
    """n bytes at byte offset off of the device."""
    if IMG:
        with open(IMG, 'rb') as f:
            f.seek(off)
            return f.read(n)
    out = bytearray()
    first, last = off // BS, (off + n - 1) // BS
    blk = first
    while blk <= last:
        if blk in _cache:
            out += _cache[blk]
            blk += 1
            continue
        run = blk
        while run <= last and run not in _cache and run - blk < 4096:
            run += 1
        data = subprocess.run(ADB + ['exec-out', f'dd if={DEV} bs={BS} skip={blk} count={run - blk} 2>/dev/null'],
                              check=True, capture_output=True).stdout
        assert len(data) == (run - blk) * BS, f'short read at block {blk}'
        for i in range(run - blk):
            _cache[blk + i] = data[i * BS:(i + 1) * BS]
        out += data
        blk = run
    s = off - first * BS
    return bytes(out[s:s + n])


class Fork:
    def __init__(self, raw, bsize, overflow=None, cnid=None):
        self.size, _, self.blocks = struct.unpack_from('>QII', raw, 0)
        self.ext = [struct.unpack_from('>II', raw, 16 + 8 * i) for i in range(8)]
        self.bsize = bsize
        if overflow is not None and sum(c for _, c in self.ext) < self.blocks:
            self.ext += overflow(cnid)

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            b, inner = divmod(off, self.bsize)
            for start, count in self.ext:
                if b < count:
                    take = min(n, (count - b) * self.bsize - inner)
                    out += read((start + b) * self.bsize + inner, take)
                    off += take
                    n -= take
                    break
                b -= count
            else:
                raise IOError('offset beyond the first 8 extents (extents overflow not supported)')
        return bytes(out)


class HFS:
    def __init__(self):
        vh = read(1024, 512)
        sig = vh[:2]
        assert sig in (b'H+', b'HX'), f'not HFS+ ({sig!r})'
        self.bsize, = struct.unpack_from('>I', vh, 40)
        self.extf = Fork(vh[0xc0:0x110], self.bsize)
        self.cat = Fork(vh[0x110:0x160], self.bsize, self.overflow, 4)
        hdr = self.cat.read(0, 512)
        self.root, = struct.unpack_from('>I', hdr, 14 + 2)
        self.nsize, = struct.unpack_from('>H', hdr, 14 + 18)

    def overflow(self, cnid):
        """Extra data-fork extents of file cnid from the extents overflow B-tree (leaf scan)."""
        hdr = self.extf.read(0, 512)
        first, = struct.unpack_from('>I', hdr, 14 + 10)
        nsize, = struct.unpack_from('>H', hdr, 14 + 18)
        found, n = [], first
        while n:
            d = self.extf.read(n * nsize, nsize)
            flink, _, kind, _, nrec = struct.unpack_from('>IIbBH', d, 0)
            for k in range(nrec):
                o, = struct.unpack_from('>H', d, nsize - 2 * (k + 1))
                klen, fork, fid, start = struct.unpack_from('>HBxII', d, o)
                if fork == 0 and fid == cnid:
                    found.append((start, [struct.unpack_from('>II', d, o + 2 + klen + 8 * i) for i in range(8)]))
            n = flink
        return [e for _, ext in sorted(found) for e in ext if e[1]]

    def node(self, i):
        d = self.cat.read(i * self.nsize, self.nsize)
        flink, _, kind, height, nrec = struct.unpack_from('>IIbBH', d, 0)
        offs = [struct.unpack_from('>H', d, self.nsize - 2 * (k + 1))[0] for k in range(nrec + 1)]
        recs = []
        for k in range(nrec):
            r = d[offs[k]:offs[k + 1]]
            klen, = struct.unpack_from('>H', r, 0)
            parent, nlen = struct.unpack_from('>IH', r, 2)
            name = r[8:8 + 2 * nlen].decode('utf-16-be', 'replace')
            body = r[2 + klen:]
            recs.append((parent, name, body))
        return kind, flink, recs

    def children(self, parent):
        """{name: (type, id, body)} of a folder's entries."""
        n = self.root
        while True:
            kind, flink, recs = self.node(n)
            if kind != 0:   # leaf (-1)
                break
            child = None
            for p, _, body in recs:
                if p < parent or child is None:
                    child = struct.unpack_from('>I', body, 0)[0]
                if p >= parent:
                    break
            n = child
        out = {}
        while True:
            done = False
            for p, name, body in recs:
                if p > parent:
                    done = True
                    break
                if p == parent and name:
                    typ, = struct.unpack_from('>h', body, 0)
                    cnid, = struct.unpack_from('>I', body, 8)
                    out[name] = (typ, cnid, body)
            if done or not flink:
                return out
            n = flink
            kind, flink, recs = self.node(n)

    def lookup(self, path):
        cur, body, typ = 2, None, 1
        for part in [p for p in path.split('/') if p]:
            kids = self.children(cur)
            if part not in kids:
                raise FileNotFoundError(path)
            typ, cur, body = kids[part]
            if typ == 2 and struct.unpack_from('>H', body, 32 + 10)[0] & 0o170000 == 0o120000:
                target = Fork(body[88:168], self.bsize, self.overflow, struct.unpack_from('>I', body, 8)[0])
                raise IsADirectoryError(f'{part} is a symlink to {target.read(0, target.size).decode()}')
        return typ, cur, body


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if path == '/var' or path.startswith('/var/'):
        path = '/private' + path
    fs = HFS()
    typ, cnid, body = fs.lookup(path)
    if cmd == 'ls':
        for name, (t, c, b) in sorted(fs.children(cnid).items()):
            size = struct.unpack_from('>Q', b, 88)[0] if t == 2 else 0
            mode = struct.unpack_from('>H', b, 32 + 10)[0]
            print(f'{"d" if t == 1 else "-"} {mode:06o} {size:>12} {name}')
    elif cmd == 'get':
        assert typ == 2, 'not a file'
        f = Fork(body[88:168], fs.bsize, fs.overflow, cnid)
        data = f.read(0, f.size)
        if len(sys.argv) > 3:
            open(sys.argv[3], 'wb').write(data)
            print(f'{path}: {len(data)} bytes -> {sys.argv[3]}', file=sys.stderr)
        else:
            sys.stdout.buffer.write(data)


if __name__ == '__main__':
    main()
