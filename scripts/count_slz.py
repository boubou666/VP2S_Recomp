"""Count SLZ-compressed chunks starting on sector boundaries and summarize their headers.

Header guess (little endian): 'SLZ' + type(u8), comp_size(u32), decomp_size(u32), next(u32).
"""
import glob
import struct
from collections import Counter

SECTOR = 2048
ISO = glob.glob("/work/*.iso")[0]

types = Counter()
total_comp = total_decomp = 0
samples = []
fourcc = Counter()
with open(ISO, "rb") as f:
    lba = 0
    while True:
        data = f.read(SECTOR * 8192)
        if not data:
            break
        for off in range(0, len(data), SECTOR):
            head = data[off:off + 16]
            if head[:3] == b"SLZ":
                t = head[3]
                comp, decomp, nxt = struct.unpack_from("<III", head, 4)
                types[t] += 1
                total_comp += comp
                total_decomp += decomp
                if len(samples) < 15:
                    samples.append((lba + off // SECTOR, t, comp, decomp, nxt, data[off + 16:off + 24]))
            elif head[:4].isalpha() and head[4:8] != b"\0\0\0\0":
                fourcc[head[:4]] += 1
        lba += len(data) // SECTOR

print("SLZ chunks at sector starts by type:", dict(types))
print(f"total compressed {total_comp / 2**20:.0f} MiB -> decompressed {total_decomp / 2**20:.0f} MiB")
print("samples (lba, type, comp, decomp, next, first bytes):")
for s in samples:
    print("  ", s[:5], s[5].hex())
print("most common alphabetic 4CC at sector start:", fourcc.most_common(15))
