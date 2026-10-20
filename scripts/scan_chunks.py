"""Find every SLZ/SLE chunk chain on the disc (aligned or not), classify, and extract modules.

Output:
  /work/work/chunks.tsv          one line per chain head: offset, magic, type, comp, decomp, chain len, kind, info
  /work/work/modules/<slot>_<id>_<offset>.bin   fully decompressed overlay modules
"""
import glob
import os
import struct
import sys
from collections import Counter
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

from elftools.elf.elffile import ELFFile

ISO = glob.glob("/work/*.iso")[0]
OUT_DIR = "/work/work/modules"
SIZE = os.path.getsize(ISO)
SPAN = 64 * 1024 * 1024
MAX_CHUNK = 8 * 1024 * 1024

with open("/work/extracted/SLES_546.45", "rb") as f:
    SLOTS = sorted({s["p_vaddr"] for s in ELFFile(f).iter_segments() if s["p_type"] == "PT_LOAD"} - {0x100000})


def plausible(buf, off):
    try:
        magic, typ, comp, decomp, nxt = slz.parse_header(buf, off)
    except Exception:
        return None
    if typ > 3 or not (0 < comp <= MAX_CHUNK) or not (0 < decomp <= 4 * MAX_CHUNK) or comp > decomp + 64:
        return None
    if nxt and (nxt < 16 + comp or nxt > comp + 4096):
        return None
    return magic, typ, comp, decomp, nxt


def classify(head: bytes):
    if len(head) >= 16:
        w = struct.unpack_from("<4I", head)
        if w[0] == 0 and w[2] in SLOTS:
            return "module", f"id={w[1]} slot=0x{w[2]:x} text=0x{w[3]:x}"
    if head[:4] == b"\x7fELF":
        return "elf", ""
    tag = head[:4]
    if all(32 <= b < 127 for b in tag):
        return "tagged", tag.decode()
    return "data", head[:8].hex()


def scan(start):
    results = []
    with open(ISO, "rb") as f:
        f.seek(start)
        buf = f.read(SPAN + MAX_CHUNK + 32)
    for magic in (b"SLZ", b"SLE"):
        pos = buf.find(magic)
        while 0 <= pos < SPAN:
            h = plausible(buf, pos)
            if h:
                try:
                    head = slz.decompress_chunk(buf, pos, slz.SLE_KEY, peek=64)
                    n, off = 1, pos
                    while slz.parse_header(buf, off)[4] and n < 4096:
                        off += slz.parse_header(buf, off)[4]
                        if not plausible(buf, off):
                            break
                        n += 1
                    kind, info = classify(head)
                    results.append((start + pos, h[0].decode(), h[1], h[2], h[3], n, kind, info))
                except Exception:
                    pass
            pos = buf.find(magic, pos + 1)
    return results


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    with Pool(os.cpu_count()) as p:
        rows = sorted(r for part in p.map(scan, range(0, SIZE, SPAN)) for r in part)

    # Drop chunks that are links inside an already-found chain
    inside = set()
    with open(ISO, "rb") as f:
        for r in rows:
            if r[5] > 1:
                f.seek(r[0])
                buf = f.read(MAX_CHUNK * 2)
                off = 0
                for _ in range(r[5] - 1):
                    off += slz.parse_header(buf, off)[4]
                    inside.add(r[0] + off)
    heads = [r for r in rows if r[0] not in inside]

    with open("/work/work/chunks.tsv", "w") as o:
        o.write("offset\tmagic\ttype\tcomp\tdecomp\tchain\tkind\tinfo\n")
        for r in heads:
            o.write("\t".join(str(x) for x in r) + "\n")

    kinds = Counter(r[6] for r in heads)
    print(f"chunk headers found: {len(rows)}, chain heads: {len(heads)}")
    print("kinds:", dict(kinds))
    print("magics:", dict(Counter(r[1] for r in heads)))
    print("total compressed: %.0f MiB" % (sum(r[3] for r in rows) / 2**20))
    tags = Counter(r[7] for r in heads if r[6] == "tagged")
    print("top tags:", tags.most_common(20))

    # Extract modules completely
    mods = [r for r in heads if r[6] == "module"]
    slot_ids = Counter()
    with open(ISO, "rb") as f:
        for r in mods:
            f.seek(r[0])
            buf = f.read(MAX_CHUNK * 4)
            data = b"".join(slz.decompress_chain(buf, 0, slz.SLE_KEY))
            w = struct.unpack_from("<4I", data)
            slot_ids[w[2]] += 1
            with open(f"{OUT_DIR}/{w[2]:08x}_{w[1]:04x}_{r[0]:010x}.bin", "wb") as o:
                o.write(data)
    print(f"modules extracted: {len(mods)}")
    for s in SLOTS:
        print(f"  slot 0x{s:08x}: {slot_ids.get(s, 0)} module(s)")
