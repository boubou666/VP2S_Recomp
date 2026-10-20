"""Overlay layout and VU microcode heuristics for the main ELF.

- Groups the zero-filesize PT_LOAD segments (overlay slots) by load address.
- Scans the loaded image for VIF MPG commands followed by VU code and for
  the VU lower/upper NOP pair, which is dense inside VU microprograms.
"""
import struct
from collections import defaultdict
from elftools.elf.elffile import ELFFile

ELF = "/work/extracted/SLES_546.45"
VU_LOWER_NOP = 0x8000033C
VU_UPPER_NOP = 0x000002FF

with open(ELF, "rb") as f:
    elf = ELFFile(f)
    segs = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
    main = segs[0]
    image = main.data()
    base = main["p_vaddr"]

    slots = defaultdict(list)
    for s in segs[1:]:
        slots[s["p_vaddr"]].append(s["p_memsz"])
    print(f"main segment: 0x{base:x}, file {main['p_filesz']:#x}, mem {main['p_memsz']:#x}")
    print(f"overlay segments: {len(segs) - 1} in {len(slots)} slots")
    total = 0
    for addr in sorted(slots):
        sizes = slots[addr]
        total += sum(sizes)
        print(f"  slot 0x{addr:08x}: {len(sizes):>3} overlays, max {max(sizes):#8x}, sum {sum(sizes):#9x}")
    print(f"sum of overlay sizes: {total / 1024:.0f} KiB")

words = struct.unpack_from(f"<{len(image) // 4}I", image)

# VU NOP pairs (upper word at odd index in each 64-bit VU instruction: lower, upper)
pairs = [i for i in range(0, len(words) - 1, 2) if words[i] == VU_LOWER_NOP and words[i + 1] == VU_UPPER_NOP]
print(f"VU lower/upper NOP pairs: {len(pairs)}")
if pairs:
    # cluster into regions
    regions = []
    start = prev = pairs[0]
    for p in pairs[1:]:
        if p - prev > 512:
            regions.append((start, prev))
            start = p
        prev = p
    regions.append((start, prev))
    print(f"VU code regions (clustered): {len(regions)}")
    for s, e in regions[:30]:
        print(f"  0x{base + s * 4:08x}-0x{base + e * 4 + 8:08x}  ~{(e - s) * 4 + 8} bytes")

# VIF MPG: cmd byte 0x4A, NUM = number of 64-bit instructions to upload
mpg = []
for i, w in enumerate(words):
    if (w >> 24) & 0x7F == 0x4A:
        num = (w >> 16) & 0xFF or 256
        nxt = words[i + 1:i + 1 + num * 2]
        if len(nxt) == num * 2 and VU_UPPER_NOP in nxt[1::2]:
            mpg.append((base + i * 4, num, w & 0xFFFF))
print(f"VIF MPG uploads with VU-looking payload: {len(mpg)}")
for a, n, addr in mpg[:20]:
    print(f"  at 0x{a:08x}: {n} instr -> VU addr {addr:#x}")
