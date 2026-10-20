"""Check which load base makes a module's internal jal targets land on function prologues.

H1: header kept in memory (file offset 0 at the slot address, code at slot+0x80).
H0: header stripped (file offset 0x80 at the slot address).
"""
import glob
import struct
import sys

path = glob.glob(sys.argv[1])[0]
d = open(path, "rb").read()
slot, text = struct.unpack_from("<I", d, 8)[0], struct.unpack_from("<I", d, 12)[0]
words = struct.unpack_from(f"<{len(d) // 4}I", d)

for label, file0 in (("H1 header in memory", slot), ("H0 header stripped", slot - 0x80)):
    hits = total = 0
    for w in words[0x20:(0x80 + text) // 4]:
        if w >> 26 == 3:  # jal
            target = (w & 0x3FFFFFF) << 2
            off = target - file0
            if 0x80 <= off < len(d) - 4:
                total += 1
                hits += (words[off // 4] >> 16) == 0x27BD and (words[off // 4] & 0x8000) != 0
    print(f"{label}: {hits}/{total} internal jal targets are 'addiu sp,sp,-N' prologues")
