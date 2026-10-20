"""List every plausible SLE header on the disc and whether our key decrypts it."""
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


def scan(start):
    out = []
    with open(ISO, "rb") as f:
        f.seek(start)
        buf = f.read(SPAN + 16)
    p = buf.find(b"SLE")
    while 0 <= p < SPAN:
        magic, typ, comp, decomp, nxt = struct.unpack_from("<3sBIII", buf + bytes(16), p)
        if typ <= 3 and 0 < comp <= 16 << 20 and 0 < decomp <= 64 << 20:
            with open(ISO, "rb") as f:
                f.seek(start + p)
                chunk = f.read(16 + comp)
            try:
                slz.decompress_chunk(chunk, 0, slz.SLE_KEY)
                ok = True
            except Exception:
                ok = False
            out.append((start + p, typ, comp, decomp, nxt, ok))
        p = buf.find(b"SLE", p + 1)
    return out


if __name__ == "__main__":
    with Pool(os.cpu_count()) as pool:
        rows = sorted(r for part in pool.map(scan, range(0, SIZE, SPAN)) for r in part)
    good = [r for r in rows if r[5]]
    bad = [r for r in rows if not r[5]]
    print(f"plausible SLE headers: {len(rows)}, decrypt OK: {len(good)}, FAIL: {len(bad)}")
    for r in bad[:30]:
        print(f"  offset {r[0]} (lba {r[0] // 2048}, +{r[0] % 2048:#x}) type {r[1]} comp {r[2]:#x} decomp {r[3]:#x} next {r[4]:#x}")
