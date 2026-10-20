"""Decompress MWo3-tagged chunks, dump a few headers, and group identical payloads."""
import csv
import glob
import hashlib
import sys
from collections import defaultdict

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
rows = [r for r in csv.DictReader(open("/work/work/chunks.tsv"), delimiter="\t") if r["info"] == "MWo3"]
print("MWo3 chunks:", len(rows))

uniq = defaultdict(list)
with open(ISO, "rb") as f:
    for n, r in enumerate(rows):
        off = int(r["offset"])
        f.seek(off)
        buf = f.read(int(r["comp"]) * int(r["chain"]) + 65536)
        data = b"".join(slz.decompress_chain(buf, 0, slz.SLE_KEY))
        h = hashlib.sha1(data).hexdigest()[:12]
        uniq[h].append(off)
        if len(uniq[h]) == 1:
            open(f"/work/work/mwo3_{h}.bin", "wb").write(data)
        if n < 4:
            print(f"--- {r['magic']} @ {off} ({len(data)} bytes)")
            for i in range(0, 0x60, 16):
                c = data[i:i + 16]
                print(f"  {i:04x}  " + " ".join(f"{b:02x}" for b in c).ljust(48)
                      + "".join(chr(b) if 32 <= b < 127 else "." for b in c))
print("unique MWo3 payloads:", len(uniq))
