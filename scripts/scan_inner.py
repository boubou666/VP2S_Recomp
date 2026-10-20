"""Decompress every chunk chain and extract MWo3 overlays found anywhere inside (and in raw sectors).

MWo3 header (0x80 bytes): 'MWo3', id, load address, text size, data size, bss size,
entry, entry2, name[0x20..]. Unique overlays are written to /work/work/overlays/.
"""
import csv
import glob
import hashlib
import os
import struct
import sys
from multiprocessing import Pool

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
OUT = "/work/work/overlays"
MW = struct.Struct("<4s7I")


def overlays_in(data: bytes, where: str):
    found = []
    pos = data.find(b"MWo3")
    while pos >= 0:
        if pos + 0x80 <= len(data):
            _, ov_id, load, text, dsize, bss, entry, _ = MW.unpack_from(data, pos)
            if 0x100000 <= load < 0x2000000 and text < 0x400000 and dsize < 0x400000:
                name = data[pos + 0x20:pos + 0x60].split(b"\0")[0].decode("ascii", "replace")
                blob = data[pos:pos + 0x80 + text + dsize]
                found.append((where, ov_id, load, text, dsize, bss, name, blob))
        pos = data.find(b"MWo3", pos + 4)
    return found


def work(row):
    off, comp, chain = int(row["offset"]), int(row["comp"]), int(row["chain"])
    try:
        with open(ISO, "rb") as f:
            f.seek(off)
            buf = f.read(comp * chain + 4096 * chain + 65536)
        data = b"".join(slz.decompress_chain(buf, 0, slz.SLE_KEY))
    except Exception:
        return [], 0, 1
    return overlays_in(data, f"chunk@{off}"), len(data), 0


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    rows = list(csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t"))
    rows.sort(key=lambda r: -int(r["comp"]))  # big ones first for load balance
    total_out = errors = 0
    hits = []
    with Pool(os.cpu_count()) as p:
        for found, n, err in p.imap_unordered(work, rows, chunksize=16):
            hits += found
            total_out += n
            errors += err

    # Raw (uncompressed) occurrences straight on the disc
    size = os.path.getsize(ISO)
    with open(ISO, "rb") as f:
        for start in range(0, size, 256 << 20):
            f.seek(start)
            hits += overlays_in(f.read((256 << 20) + 0x400000), f"raw@{start}")

    uniq = {}
    for where, ov_id, load, text, dsize, bss, name, blob in hits:
        h = hashlib.sha1(blob).hexdigest()[:10]
        if h not in uniq:
            uniq[h] = (load, ov_id, name, text, dsize, bss, where)
            safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in name) or "noname"
            open(f"{OUT}/{load:08x}_{ov_id:04x}_{safe}_{h}.bin", "wb").write(blob)

    print(f"decompressed {total_out / 2**30:.2f} GiB from {len(rows)} chains ({errors} errors)")
    print(f"MWo3 occurrences: {len(hits)}, unique overlays: {len(uniq)}")
    per_slot = {}
    for load, ov_id, name, text, dsize, bss, where in uniq.values():
        per_slot.setdefault(load, []).append((ov_id, name, text))
    for load in sorted(per_slot):
        items = sorted(per_slot[load])
        names = ", ".join(n for _, n, _ in items[:6])
        print(f"  slot 0x{load:08x}: {len(items):>3} unique, text {sum(t for *_, t in items) / 1024:.0f} KiB  e.g. {names}")
