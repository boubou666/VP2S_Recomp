"""Decompress every sector-aligned SLZ/SLE chain on the disc and report results.

Writes decompressed chains to /work/work/slz_samples/<lba>_<n>.bin for inspection.
"""
import glob
import os
import sys
from collections import Counter

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

SECTOR = 2048
ISO = glob.glob("/work/*.iso")[0]
OUT = "/work/work/slz_samples"
os.makedirs(OUT, exist_ok=True)

ok = Counter()
fail = []
magic_seen = Counter()
with open(ISO, "rb") as f:
    lba = 0
    while True:
        block = f.read(SECTOR * 8192)
        if not block:
            break
        for off in range(0, len(block), SECTOR):
            if block[off:off + 3] not in (b"SLZ", b"SLE"):
                continue
            here = lba + off // SECTOR
            magic_seen[block[off:off + 3]] += 1
            f_pos = f.tell()
            f.seek(here * SECTOR)
            buf = f.read(4 * 1024 * 1024)
            f.seek(f_pos)
            try:
                parts = slz.decompress_chain(buf)
                for n, p in enumerate(parts):
                    with open(f"{OUT}/{here}_{n}.bin", "wb") as o:
                        o.write(p)
                typ = buf[3]
                ok[typ] += 1
            except Exception as e:  # report and continue
                fail.append((here, buf[:4], str(e)[:80]))
        lba += len(block) // SECTOR

print("magics:", dict(magic_seen))
print("decompressed OK by first-chunk type:", dict(ok))
print(f"failures: {len(fail)}")
for x in fail[:15]:
    print("  ", x)
