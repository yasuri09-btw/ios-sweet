#!/usr/bin/env python3
"""List ARMv8.2 SHA3-extension instructions (EOR3, BCAX, RAX1, XAR) in a kernel.

  scan-sha3.py KERNEL

The phones' Cortex-A55/A75/A76 cores lack FEAT_SHA3, but the CI kernel is
built for armv8.2-a+crypto with an Apple toolchain, so vectorized xor chains
can become EOR3 (0091: undefined instruction panic in a6_blit_init_ib).
Exit status 1 if any are found.
"""
import bisect, struct, subprocess, sys

K = sys.argv[1]
b = open(K, 'rb').read()
out = subprocess.run(['llvm-objdump', '--macho', '--private-headers', K], capture_output=True, text=True).stdout
segs, cur = [], {}
for l in out.splitlines():
    l = l.strip().split()
    if l and l[0] in ('segname', 'vmaddr', 'fileoff', 'filesize'):
        cur[l[0]] = l[1]
        if l[0] == 'filesize' and 'segname' in cur:
            segs.append(cur)
            cur = {}
nm = subprocess.run(['llvm-nm', '-n', K], capture_output=True, text=True).stdout.split('\n')
syms = [(int(p[0], 16), p[2]) for p in (x.split() for x in nm) if len(p) == 3 and p[1] in 'tTsS']
ks = [a for a, _ in syms]
found = 0
for s in segs:
    if s['segname'] != '__TEXT_EXEC':
        continue
    fo, fs, va0 = int(s['fileoff']), int(s['filesize']), int(s['vmaddr'], 16)
    for o in range(fo, fo + fs, 4):
        w = struct.unpack_from('<I', b, o)[0]
        kind = ('eor3' if w & 0xffe08000 == 0xce000000 else 'bcax' if w & 0xffe08000 == 0xce200000 else
                'rax1' if w & 0xffe0fc00 == 0xce608c00 else 'xar' if w & 0xffe00000 == 0xce800000 else None)
        if kind:
            va = va0 + o - fo
            j = bisect.bisect_right(ks, va) - 1
            print(f'{va:#x} {kind} {w:08x} {syms[j][1]}+{va - syms[j][0]:#x}')
            found += 1
print(f'{found} SHA3 instructions')
sys.exit(1 if found else 0)
