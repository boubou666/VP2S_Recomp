"""Look for small-slot modules nested inside decompressed parts.

For each decompressed part: (1) raw module headers (word0 == 0, word2 == small slot address,
word6/7 constructor table inside that slot) and (2) nested SLZ/SLE chunks whose decompressed
data starts with such a header. Unique hits go to /work/work/nested/.
"""
import csv
import glob
import hashlib
import os
import struct
import sys
from collections import Counter, defaultdict
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
OUT = "/work/work/nested"
ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
SMALL = sorted(set(SLOT_TABLE[7:21]))
LO, HI = min(SMALL), 0x549100


def module_at(data, p):
    if p < 0 or p + 0x80 > len(data):
        return None
    w = struct.unpack_from("<8I", data, p)
    if w[0] == 0 and w[2] in SMALL and 0x80 < w[3] < 0x200000 and w[4] < 0x200000 \
            and LO <= w[6] <= w[7] < HI:
        return w
    return None


def scan_part(data, where, found):
    for slot in SMALL:
        pat = struct.pack("<I", slot)
        p = data.find(pat)
        while p >= 0:
            w = module_at(data, p - 8)
            if w:
                found.append((where + f"+raw@{p - 8:#x}", w, data[p - 8:p - 8 + w[3] + w[4]]))
            p = data.find(pat, p + 4)
    for magic in (b"SLZ", b"SLE"):
        p = data.find(magic)
        while p >= 0:
            try:
                magic_, typ, comp, decomp, nxt = slz.parse_header(data, p)
                if typ <= 3 and 0 < comp <= len(data) - p and 0x80 <= decomp < 0x400000:
                    head = slz.decompress_chunk(data, p, slz.SLE_KEY, peek=0x80)
                    if module_at(head, 0):
                        full = slz.decompress_chunk(data, p, slz.SLE_KEY)
                        found.append((where + f"+slz@{p:#x}", module_at(full, 0), full))
            except Exception:
                pass
            p = data.find(magic, p + 1)


def work(row):
    off, chain = int(row["offset"]), int(row["chain"])
    found, tags = [], Counter()
    try:
        with open(ISO, "rb") as f:
            buf = slz.read_chain(f, off)
        pos = 0
        for n in range(chain):
            part = slz.decompress_chunk(buf, pos, slz.SLE_KEY)
            before = len(found)
            scan_part(part, f"{off}/{n}", found)
            if len(found) > before:
                tags[part[:4]] += 1
            nxt = slz.parse_header(buf, pos)[4]
            if not nxt:
                break
            pos += nxt
    except Exception:
        pass
    return found, tags


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    rows = list(csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t"))
    rows.sort(key=lambda r: -int(r["comp"]))
    occ, tags = [], Counter()
    with Pool(os.cpu_count()) as p:
        for found, t in p.imap_unordered(work, rows, chunksize=16):
            occ += found
            tags += t

    uniq = {}
    with open("/work/work/nested.tsv", "w") as o:
        o.write("where\tid\tslot\ttext\tdata\tbss\tsha1\n")
        for where, w, blob in occ:
            h = hashlib.sha1(blob).hexdigest()[:10]
            o.write(f"{where}\t{w[1]}\t0x{w[2]:x}\t{w[3]:#x}\t{w[4]:#x}\t{w[5]:#x}\t{h}\n")
            if h not in uniq:
                uniq[h] = w
                open(f"{OUT}/{w[2]:08x}_{w[1]:04x}_{h}.bin", "wb").write(blob)

    by_slot = defaultdict(list)
    for w in uniq.values():
        by_slot[w[2]].append(w)
    print(f"nested module hits: {len(occ)}, unique {len(uniq)}")
    print("container tags holding them:", [(t.hex(), t, n) for t, n in tags.most_common(10)])
    for idx in range(7, 21):
        s = SLOT_TABLE[idx]
        ws = by_slot.get(s, [])
        ids = sorted({w[1] for w in ws})
        print(f"  [{idx:2}] slot 0x{s:08x}: {len(ws):>3} unique"
              + (f", ids {ids[0]}..{ids[-1]}, text {sum(w[3] for w in ws) / 1024:.0f} KiB" if ws else ""))
