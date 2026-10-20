"""Count direct calls (jal) and lui-based accesses from known code into each overlay slot range."""
import glob
import struct
from collections import Counter

ELF = open("/work/extracted/SLES_546.45", "rb").read()
SLOT_TABLE = struct.unpack_from("<21I", ELF, 0x1C8A30 - 0x100000 + 0x2E00)
bounds = sorted(set(a for a in SLOT_TABLE if a)) + [0x549100]
ranges = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]


def images():
    yield "main", 0x100000, ELF[0x2E00:0x2E00 + 0xc8b00]
    for p in sorted(glob.glob("/work/work/parts/*.bin")):
        d = open(p, "rb").read()
        yield p.split("/")[-1][:14], struct.unpack_from("<I", d, 8)[0], d


def slot_of(a):
    for lo, hi in ranges:
        if lo <= a < hi:
            return lo
    return None


jal = Counter()
lui = Counter()
for name, base, img in images():
    words = struct.unpack_from(f"<{len(img) // 4}I", img)
    own = slot_of(base)
    for w in words:
        if w >> 26 == 3:
            s = slot_of((w & 0x3FFFFFF) << 2)
            if s is not None and s != own:
                jal[(s, name)] += 1
        elif w >> 26 == 0xF:  # lui
            hi = (w & 0xFFFF) << 16
            s = slot_of(hi + 0x8000)
            if s is not None and s != own and hi >= 0x500000:
                lui[s] += 1

print("cross-module jal into slots (target slot <- caller: count):")
for (s, name), n in sorted(jal.items()):
    print(f"  0x{s:08x} <- {name}: {n}")
print("lui with high half inside small slots (any caller):")
for s, n in sorted(lui.items()):
    print(f"  0x{s:08x}: {n}")
