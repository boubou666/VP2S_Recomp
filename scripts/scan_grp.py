"""Find tagged containers (raw on disc) and extract their GRP entries (small overlays).

Container (from FUN_00422c80): +0x4 entry count; entries of 16 bytes from +0x10:
tag (u32, ASCII), param, size, offset from container start. GRP: overlay loaded by
FUN_00410cc0 into slot table entry (param + 3); its payload is an SLZ chunk.
"""
import glob
import hashlib
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

ISO = glob.glob("/work/*.iso")[0]
OUT = "/work/work/grp"
ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
SPAN = 256 << 20
TAIL = 16 << 20


def tag_ok(t: bytes):
    return all(48 <= b < 91 or b == 0 for b in t) and t[0] != 0


def find_base(buf, pos):
    """Walk back from an entry tag to the container header."""
    for j in range(0, 512):
        base = pos - 0x10 - 16 * j
        if base < 0:
            return None
        if not tag_ok(buf[base + 0x10 + 16 * j:base + 0x14 + 16 * j]):
            return None
        count = struct.unpack_from("<I", buf, base + 4)[0]
        if struct.unpack_from("<I", buf, base)[0] == 0 and j < count <= 4096:
            if all(tag_ok(buf[base + 0x10 + 16 * k:base + 0x14 + 16 * k]) for k in range(min(count, 64))):
                return base, count
    return None


containers = {}
with open(ISO, "rb") as f:
    size = os.path.getsize(ISO)
    for start in range(0, size, SPAN):
        f.seek(start)
        buf = f.read(SPAN + TAIL)
        pos = buf.find(b"GRP\0")
        while 0 <= pos < SPAN:
            r = find_base(buf, pos)
            if r and start + r[0] not in containers:
                base, count = r
                entries = [struct.unpack_from("<4sIII", buf, base + 0x10 + 16 * k) for k in range(count)]
                containers[start + base] = (count, entries, buf[base:base + max(e[3] + e[2] for e in entries)])
            pos = buf.find(b"GRP\0", pos + 4)

os.makedirs(OUT, exist_ok=True)
tags = Counter()
uniq = {}
by_slot = defaultdict(list)
bad = 0
for cbase, (count, entries, blob) in sorted(containers.items()):
    for tag, param, esize, eoff in entries:
        tags[tag.rstrip(b"\0").decode()] += 1
        if tag != b"GRP\0":
            continue
        try:
            data = slz.decompress_chunk(blob, eoff, slz.SLE_KEY)
        except Exception:
            data = blob[eoff:eoff + esize]
            if slz.HEADER.size and blob[eoff:eoff + 3] in (b"SLZ", b"SLE"):
                bad += 1
                continue
        slot = SLOT_TABLE[param + 3] if param + 3 < len(SLOT_TABLE) else None
        h = hashlib.sha1(data).hexdigest()[:10]
        if h not in uniq:
            uniq[h] = (param, slot, len(data))
            by_slot[param].append(len(data))
            open(f"{OUT}/{param:02d}_{(slot or 0):08x}_{h}.bin", "wb").write(data)

print(f"containers with GRP: {len(containers)}")
print("entry tags:", tags.most_common(25))
print(f"GRP payloads: {sum(1 for c in containers.values() for e in c[1] if e[0] == b'GRP' + bytes(1))}, unique {len(uniq)}, decode errors {bad}")
for param in sorted(by_slot):
    slot = SLOT_TABLE[param + 3] if param + 3 < len(SLOT_TABLE) else 0
    sizes = by_slot[param]
    print(f"  param {param:2} -> slot [{param + 3}] 0x{slot:08x}: {len(sizes):>3} unique, {min(sizes):#x}..{max(sizes):#x}, {sum(sizes) / 1024:.0f} KiB")
