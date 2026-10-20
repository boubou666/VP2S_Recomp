"""Look for headerless data overlays of the mode-B slots (slot table entries 5..20).

A decompressed part of size <= slot capacity is attributed to slot S when many aligned
words are pointers into [S, S + len(part)). Also checks raw disc sectors for module headers
(word0 == 0, word2 == mode-B slot address).
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
OUT = "/work/work/data_overlays"
ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
MODE_B = sorted(set(SLOT_TABLE[5:21]) - {0x35ec80})
bounds = sorted(set(a for a in SLOT_TABLE if a)) + [0x549100]
CAP = {s: bounds[bounds.index(s) + 1] - s for s in MODE_B}
CAP[0x36d800] = 0x514100 - 0x36d800


def best_slot(part):
    n = len(part) // 4
    words = struct.unpack_from(f"<{n}I", part)
    best = None
    for s in MODE_B:
        if len(part) > CAP[s]:
            continue
        inside = sum(1 for w in words if s <= w < s + len(part))
        if inside >= 8 and inside >= n * 0.02 and (best is None or inside > best[1]):
            best = (s, inside)
    return best


def work(row):
    off, chain = int(row["offset"]), int(row["chain"])
    found = []
    try:
        with open(ISO, "rb") as f:
            buf = slz.read_chain(f, off)
        pos = 0
        for n in range(chain):
            if slz.parse_header(buf, pos)[3] <= 0x1a6900:
                part = slz.decompress_chunk(buf, pos, slz.SLE_KEY)
                b = best_slot(part)
                if b:
                    found.append((off, n, b[0], b[1], part))
            nxt = slz.parse_header(buf, pos)[4]
            if not nxt:
                break
            pos += nxt
    except Exception:
        pass
    return found


def raw_headers():
    hits = []
    with open(ISO, "rb") as f:
        size = os.path.getsize(ISO)
        for start in range(0, size, 256 << 20):
            f.seek(start)
            buf = f.read((256 << 20) + 16)
            for s in MODE_B + [0x35ec80]:
                pat = struct.pack("<I", s)
                p = buf.find(pat)
                while p >= 0:
                    if p >= 8 and (p - 8) % 16 == 0 and buf[p - 8:p - 4] == bytes(4):
                        w = struct.unpack_from("<8I", buf, p - 8)
                        if 0x80 < w[3] < 0x200000 and w[4] < 0x200000:
                            hits.append((start + p - 8, w[1], hex(s), hex(w[3])))
                    p = buf.find(pat, p + 4)
    return hits


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    rows = list(csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t"))
    rows.sort(key=lambda r: -int(r["comp"]))
    occ = []
    with Pool(os.cpu_count()) as p:
        for found in p.imap_unordered(work, rows, chunksize=16):
            occ += found
    uniq = {}
    by_slot = defaultdict(list)
    for off, n, s, inside, part in occ:
        h = hashlib.sha1(part).hexdigest()[:10]
        if h not in uniq:
            uniq[h] = (s, len(part), inside, off, n)
            by_slot[s].append((len(part), inside))
            open(f"{OUT}/{s:08x}_{h}.bin", "wb").write(part)
    print(f"data overlay candidates: {len(occ)} occurrences, {len(uniq)} unique")
    for s in MODE_B:
        items = by_slot.get(s, [])
        print(f"  slot 0x{s:08x} cap {CAP[s]:#8x}: {len(items):>3} unique"
              + (f", sizes {min(i[0] for i in items):#x}..{max(i[0] for i in items):#x}" if items else ""))
    for h, (s, size, inside, off, n) in list(uniq.items())[:8]:
        print(f"   e.g. slot 0x{s:x} size {size:#x} pointers {inside} from chain {off} part {n}")
    rh = raw_headers()
    print(f"raw module headers on disc for mode-B slots: {len(rh)}", rh[:10])
