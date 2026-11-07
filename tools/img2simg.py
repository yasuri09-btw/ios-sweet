#!/usr/bin/env python3
"""Minimal img2simg: raw image -> Android sparse image (format v1.0).

  img2simg.py RAW OUT.simg [--block 4096]

All-zero blocks become DONT_CARE chunks, so fastboot/ABL skips them instead of
writing zeros (Qualcomm ABL writes FILL chunks block by block, which made a
16 GiB userdata flash crawl). Everything else is RAW. No CRC chunks.
"""
import struct, sys

MAGIC = 0xed26ff3a
RAW, FILL, DONT_CARE = 0xcac1, 0xcac2, 0xcac3
HDR = struct.Struct('<IHHHHIIII')      # magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks, crc
CHUNK = struct.Struct('<HHII')         # type, reserved, chunk_sz (blocks), total_sz (bytes incl. header)
MAX_RAW_BLOCKS = 4096                  # 16 MiB raw chunks keep fastboot's splitting cheap


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    bs = int(sys.argv[sys.argv.index('--block') + 1]) if '--block' in sys.argv else 4096
    zero = bytes(bs)
    chunks = []                        # (type, nblocks, data_or_None)
    total = 0
    with open(src, 'rb') as f, open(dst + '.tmp', 'wb') as out:
        out.write(bytes(HDR.size))     # placeholder, rewritten at the end

        def flush(kind, blocks):
            if not blocks:
                return
            if kind == RAW:
                data = b''.join(blocks)
                out.write(CHUNK.pack(RAW, 0, len(blocks), CHUNK.size + len(data)))
                out.write(data)
            else:
                out.write(CHUNK.pack(DONT_CARE, 0, blocks, CHUNK.size))
            chunks.append(kind)

        raw, skip = [], 0
        while True:
            b = f.read(bs)
            if not b:
                break
            if len(b) < bs:
                b += bytes(bs - len(b))
            total += 1
            if b == zero:
                if raw:
                    flush(RAW, raw)
                    raw = []
                skip += 1
            else:
                if skip:
                    flush(DONT_CARE, skip)
                    skip = 0
                raw.append(b)
                if len(raw) == MAX_RAW_BLOCKS:
                    flush(RAW, raw)
                    raw = []
        flush(RAW, raw)
        flush(DONT_CARE, skip)
        out.seek(0)
        out.write(HDR.pack(MAGIC, 1, 0, HDR.size, CHUNK.size, bs, total, len(chunks), 0))
    import os
    os.replace(dst + '.tmp', dst)
    print(f'{dst}: {total} blocks of {bs}, {len(chunks)} chunks')


if __name__ == '__main__':
    main()
