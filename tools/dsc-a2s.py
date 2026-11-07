#!/usr/bin/env python3
"""Resolve unslid dyld shared cache addresses to image + nearest symbol.

  dsc-a2s.py CACHE_DIR ADDR [ADDR ...]

CACHE_DIR holds dyld_shared_cache_arm64 and its .01.. subcaches (e.g. the
mounted iOS root's System/Library/Caches/com.apple.dyld). ADDR = sampled
address minus the slide printed by the SWEET sampler/crash report.
Symbols come from each image's export trie and LC_SYMTAB (locals that the
cache keeps); the nearest one at or below ADDR is printed with its offset.
No ipsw needed.
"""
import mmap, os, struct, sys


class Cache:
    def __init__(self, d):
        self.maps = []  # (addr, size, mm, fileoff)
        main = os.path.join(d, 'dyld_shared_cache_arm64')
        files = [main] + sorted(os.path.join(d, f) for f in os.listdir(d)
                                if f.startswith('dyld_shared_cache_arm64.')
                                and f.split('.')[-1].isdigit())
        for f in files:
            fd = open(f, 'rb')
            mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
            moff, mcnt = struct.unpack_from('<II', mm, 0x10)
            for i in range(mcnt):
                a, s, fo = struct.unpack_from('<QQQ', mm, moff + 32 * i)
                self.maps.append((a, s, mm, fo))
            if f == main:
                self.mm = mm
        hdr = self.mm
        moff = struct.unpack_from('<I', hdr, 0x10)[0]
        io, ic = struct.unpack_from('<II', hdr, 0x1c0) if moff > 0x1c4 else (0, 0)
        if not ic:
            io, ic = struct.unpack_from('<II', hdr, 0x18)
        self.images = []
        for i in range(ic):
            a, _, _, po = struct.unpack_from('<QQQI', hdr, io + 32 * i)
            path = hdr[po:hdr.find(b'\0', po)].decode()
            self.images.append((a, path))

    def read(self, addr, n):
        for a, s, mm, fo in self.maps:
            if a <= addr < a + s:
                return mm[fo + addr - a: fo + addr - a + n]
        return None


def uleb(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return v, i


def trie(b, base, out):
    stack = [(0, '')]
    while stack:
        off, pre = stack.pop()
        if off >= len(b):
            continue
        size, i = uleb(b, off)
        if size:
            flags, j = uleb(b, i)
            if not flags & 0x8:  # not re-export
                addr, _ = uleb(b, j)
                out.append((base + addr, pre))
        i += size
        n = b[i]; i += 1
        for _ in range(n):
            e = b.index(b'\0', i)
            name = b[i:e].decode('latin1'); i = e + 1
            child, i = uleb(b, i)
            stack.append((child, pre + name))


def image_syms(c, base):
    h = c.read(base, 32)
    ncmds, sz = struct.unpack_from('<II', h, 16)
    lc = c.read(base + 32, sz)
    segs, syms, text = {}, [], None
    i = 0
    cmds = []
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', lc, i)
        cmds.append((cmd, lc[i:i + cs]))
        if cmd == 0x19:
            name = lc[i + 8:i + 24].split(b'\0')[0].decode()
            va, vs, fo, fs = struct.unpack_from('<QQQQ', lc, i + 24)
            segs[name] = (va, vs, fo)
        i += cs
    le = segs.get('__LINKEDIT')
    lea = lambda off: le[0] + off - le[2]
    for cmd, b in cmds:
        if cmd == 0x80000033 or (cmd == 0x80000022 and struct.unpack_from('<I', b, 44)[0]):
            off, size = struct.unpack_from('<II', b, 8) if cmd == 0x80000033 else struct.unpack_from('<II', b, 40)
            t = c.read(lea(off), size)
            if t:
                trie(t, base, syms)
        if cmd == 0x2:
            so, sn, stro, strs = struct.unpack_from('<IIII', b, 8)
            st = c.read(lea(so), sn * 16) or b''
            ss = c.read(lea(stro), strs) or b''
            for k in range(len(st) // 16):
                sx, ty, _, _, v = struct.unpack_from('<IBBHQ', st, 16 * k)
                if ty & 0xe0 or not v or sx >= len(ss):
                    continue
                syms.append((v, ss[sx:ss.find(b'\0', sx)].decode('latin1')))
    return segs.get('__TEXT'), sorted(syms)


def main():
    c = Cache(sys.argv[1])
    for a in sys.argv[2:]:
        addr = int(a, 16)
        hit = None
        for base, path in c.images:
            h = c.read(base, 4)
            if h != b'\xcf\xfa\xed\xfe':
                continue
            nc, sz = struct.unpack_from('<II', c.read(base + 16, 8))
            lc = c.read(base + 32, sz)
            j = 0
            for _ in range(nc):
                cmd, cs = struct.unpack_from('<II', lc, j)
                if cmd == 0x19 and lc[j + 8:j + 14] == b'__TEXT':
                    va, vs = struct.unpack_from('<QQ', lc, j + 24)
                    if va <= addr < va + vs:
                        hit = (base, path)
                    break
                j += cs
            if hit:
                break
        if not hit:
            print(f'{a}: not in any image')
            continue
        _, syms = image_syms(c, hit[0])
        best = None
        for v, n in syms:
            if v <= addr:
                best = (v, n)
            else:
                break
        name = f'{best[1]}+0x{addr - best[0]:x}' if best else '?'
        print(f'{a}: {os.path.basename(hit[1])} {name}')


if __name__ == '__main__':
    main()
