"""Report chunk chains that fail to decompress, with per-part header details."""
import csv
import glob
import os
import sys
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]


def work(row):
    off, comp, chain = int(row["offset"]), int(row["comp"]), int(row["chain"])
    with open(ISO, "rb") as f:
        f.seek(off)
        buf = f.read(comp * chain + 4096 * chain + 65536)
    pos = 0
    for n in range(chain):
        try:
            slz.decompress_chunk(buf, pos, slz.SLE_KEY)
        except Exception as e:
            magic, typ, c, d, nxt = slz.parse_header(buf, pos)
            return (off, chain, n, magic, typ, c, d, nxt, repr(e)[:90], buf[pos + 16:pos + 32].hex())
        nxt = slz.parse_header(buf, pos)[4]
        if not nxt:
            break
        pos += nxt
    return None


if __name__ == "__main__":
    rows = list(csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t"))
    rows.sort(key=lambda r: -int(r["comp"]))
    with Pool(os.cpu_count()) as p:
        bad = [r for r in p.imap_unordered(work, rows, chunksize=16) if r]
    for b in bad:
        print(b)
