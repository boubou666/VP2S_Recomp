"""Find headerless code overlays for the small slots (slot table entries 8..20).

The module-3 loader (FUN_00410c40) decompresses a chunk straight to the slot address with no
header handling, so these overlays are raw code. A decompressed part of size <= slot capacity
is attributed to slot S when its jal targets land on function prologues, either inside the
part placed at S or inside the main ELF.
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
OUT = "/work/work/raw_overlays"
ELF = open("/work/extracted/SLES_546.45", "rb").read()
MAIN = ELF[0x2E00:0x2E00 + 0xc8b00]
MAIN_W = struct.unpack_from(f"<{len(MAIN) // 4}I", MAIN)
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
SMALL = sorted(set(SLOT_TABLE[8:21]))
CAP = {s: (sorted(SMALL + [0x549100])[sorted(SMALL + [0x549100]).index(s) + 1] - s) for s in SMALL}


def is_prologue(w):
    return (w >> 16) == 0x27BD and (w & 0x8000)


def score(part: bytes, slot: int):
    words = struct.unpack_from(f"<{len(part) // 4}I", part)
    inner = inner_ok = main_ok = jals = 0
    for w in words:
        if w >> 26 != 3:
            continue
        jals += 1
        t = (w & 0x3FFFFFF) << 2
        if slot <= t < slot + len(part):
            inner += 1
            inner_ok += bool(is_prologue(words[(t - slot) // 4]))
        elif 0x100000 <= t < 0x100000 + len(MAIN):
            main_ok += bool(is_prologue(MAIN_W[(t - 0x100000) // 4]))
    return jals, inner, inner_ok, main_ok


def work(row):
    off, comp, chain = int(row["offset"]), int(row["comp"]), int(row["chain"])
    found = []
    try:
        with open(ISO, "rb") as f:
            f.seek(off)
            buf = f.read(comp * chain + 4096 * chain + 65536)
        pos = 0
        for n in range(chain):
            decomp = slz.parse_header(buf, pos)[3]
            if 0x100 <= decomp <= 0x10000:
                part = slz.decompress_chunk(buf, pos, slz.SLE_KEY)
                best = None
                for s in SMALL:
                    if len(part) > CAP[s]:
                        continue
                    jals, inner, inner_ok, main_ok = score(part, s)
                    good = inner_ok + main_ok
                    if jals >= 4 and good >= 0.6 * jals and inner_ok >= 1:
                        if best is None or inner_ok > best[1]:
                            best = (s, inner_ok, main_ok, jals)
                if best:
                    found.append((off, n, best, part))
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
    with open("/work/work/raw_overlays.tsv", "w") as o:
        o.write("chain_offset\tpart\tslot\tsize\tinner_ok\tmain_ok\tjals\tsha1\n")
        for off, n, (s, inner_ok, main_ok, jals), part in sorted(occ, key=lambda x: (x[0], x[1])):
            h = hashlib.sha1(part).hexdigest()[:10]
            o.write(f"{off}\t{n}\t0x{s:x}\t{len(part)}\t{inner_ok}\t{main_ok}\t{jals}\t{h}\n")
            if h not in uniq:
                uniq[h] = (s, len(part))
                open(f"{OUT}/{s:08x}_{h}.bin", "wb").write(part)

    by_slot = defaultdict(list)
    for s, size in uniq.values():
        by_slot[s].append(size)
    print(f"raw overlay candidates: {len(occ)} occurrences, {len(uniq)} unique")
    for idx in range(8, 21):
        s = SLOT_TABLE[idx]
        sizes = by_slot.get(s, [])
        print(f"  [{idx:2}] slot 0x{s:08x} cap {CAP[s]:#7x}: {len(sizes):>3} unique"
              + (f", sizes {min(sizes):#x}..{max(sizes):#x}, code {sum(sizes) / 1024:.0f} KiB" if sizes else ""))
