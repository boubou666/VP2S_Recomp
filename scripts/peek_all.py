"""Peek the first 32 decompressed bytes of every SLZ/SLE header on the disc (loose filters)
and report those whose first 8 words contain a mode-B slot address (0x35ec80, 0x36d800, small slots)."""
import glob
import os
import struct
import sys
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
SIZE = os.path.getsize(ISO)
SPAN = 64 << 20
ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
WANT = set(SLOT_TABLE[6:21])


def scan(start):
    out = []
    total = 0
    with open(ISO, "rb") as f:
        f.seek(start)
        buf = f.read(SPAN + (2 << 20))
    for magic in (b"SLZ", b"SLE"):
        p = buf.find(magic)
        while 0 <= p < SPAN:
            typ = buf[p + 3]
            comp, decomp = struct.unpack_from("<II", buf, p + 4)
            if typ <= 3 and 0 < comp and 0x20 <= decomp:
                total += 1
                try:
                    head = slz.decompress_chunk(buf, p, slz.SLE_KEY, peek=32)
                    w = struct.unpack_from("<8I", head)
                    hit = WANT.intersection(w)
                    if hit:
                        out.append((start + p, magic.decode(), typ, comp, decomp, head.hex()))
                except Exception:
                    pass
            p = buf.find(magic, p + 1)
    return out, total


if __name__ == "__main__":
    with Pool(os.cpu_count()) as pool:
        res = pool.map(scan, range(0, SIZE, SPAN))
    hits = sorted(h for r, _ in res for h in r)
    print(f"headers peeked: {sum(t for _, t in res)}, hits: {len(hits)}")
    for h in hits[:40]:
        print("  ", h)
