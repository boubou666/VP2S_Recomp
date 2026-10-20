"""Hexdump bytes of the main ELF at virtual addresses. Usage: read_vaddr.py <addr> <len> [<addr> <len> ...]"""
import sys

ELF = "/work/extracted/SLES_546.45"
BASE, OFF = 0x100000, 0x2E00  # main PT_LOAD vaddr / file offset

data = open(ELF, "rb").read()
args = sys.argv[1:]
for a, n in zip(args[::2], args[1::2]):
    va, n = int(a, 16), int(n, 0)
    buf = data[va - BASE + OFF:va - BASE + OFF + n]
    print(f"--- 0x{va:x}")
    for i in range(0, len(buf), 16):
        c = buf[i:i + 16]
        print(f"{va + i:08x}  " + " ".join(f"{b:02x}" for b in c).ljust(48)
              + "".join(chr(b) if 32 <= b < 127 else "." for b in c))
