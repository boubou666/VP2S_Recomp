"""Decompress each chunk of every chain separately and extract module-headed parts.

A part is a module when its header (0x80 bytes, kept in memory at the slot address) has
word0 in {0, 'MWo3'} and word2 equal to a slot address from the slot table at 0x1c8a30.
Large parts that do not match are listed too, to spot headerless modules.
Unique modules go to /work/work/parts/<slot>_<id>_<sha1>.bin; a TSV lists every occurrence.
"""
import csv
import glob
import hashlib
import os
import struct
import sys
from collections import defaultdict
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
OUT = "/work/work/parts"
ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
SLOTS = {a for a in SLOT_TABLE if a and a != 0x100000}


def work(row):
    off, chain = int(row["offset"]), int(row["chain"])
    found = []
    try:
        with open(ISO, "rb") as f:
            buf = slz.read_chain(f, off)
        pos = 0
        for n in range(chain):
            part = slz.decompress_chunk(buf, pos, slz.SLE_KEY)
            if len(part) >= 0x80:
                w = struct.unpack_from("<8I", part)
                if w[0] in (0, 0x336F574D) and w[2] in SLOTS:
                    found.append((off, n, w[1], w[2], len(part), part))
                elif len(part) >= 0x40000:
                    print(f"large non-module part: chain {off} part {n} size {len(part):#x} head {part[:16].hex()}", flush=True)
            nxt = slz.parse_header(buf, pos)[4]
            if not nxt:
                break
            pos += nxt
    except Exception:
        pass
    return found


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    rows = list(csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t"))
    rows.sort(key=lambda r: -int(r["comp"]))
    occ = []
    with Pool(os.cpu_count()) as p:
        for found in p.imap_unordered(work, rows, chunksize=16):
            occ += found

    uniq = {}
    with open("/work/work/parts.tsv", "w") as o:
        o.write("chain_offset\tpart\tid\tslot\tsize\tsha1\n")
        for off, n, mid, slot, size, part in sorted(occ, key=lambda x: (x[0], x[1])):
            h = hashlib.sha1(part).hexdigest()[:10]
            o.write(f"{off}\t{n}\t{mid}\t0x{slot:x}\t{size}\t{h}\n")
            if h not in uniq:
                uniq[h] = (slot, mid, size)
                open(f"{OUT}/{slot:08x}_{mid:04x}_{h}.bin", "wb").write(part)

    by_slot = defaultdict(list)
    for slot, mid, size in uniq.values():
        by_slot[slot].append((mid, size))
    print(f"module parts: {len(occ)} occurrences, {len(uniq)} unique")
    for idx, slot in enumerate(SLOT_TABLE):
        if slot in by_slot:
            items = by_slot[slot]
            ids = sorted({m for m, _ in items})
            print(f"  [{idx:2}] slot 0x{slot:08x}: {len(items):>3} unique, ids {ids[:12]}{'...' if len(ids) > 12 else ''}, max {max(s for _, s in items):#x}")
