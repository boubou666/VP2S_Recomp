"""Scan the hidden data area of the ISO (everything after the ISO9660 filesystem).

Prints the first sectors as hex, an entropy map, and offsets of known magics
(ELF/IRX modules, TIM2 textures, etc.) to help understand the archive layout.
"""
import math
import glob
import struct
import sys

SECTOR = 2048
ISO = glob.glob("/work/*.iso")[0]
FS_END = int(sys.argv[1]) if len(sys.argv) > 1 else 1681408  # from 7z "Physical Size"

MAGICS = {
    b"\x7fELF": "ELF",
    b"TIM2": "TIM2",
    b"VAGp": "VAG",
    b"IECS": "IECS(snd)",
    b"pBAV": "VAB",
    b"\x00\x00\x01\xba": "MPEG-PS",
    b"PSMF": "PSMF",
    b"RIFF": "RIFF",
}


def entropy(buf):
    if not buf:
        return 0.0
    counts = [0] * 256
    for b in buf:
        counts[b] += 1
    n = len(buf)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def hexdump(buf, base):
    for i in range(0, len(buf), 16):
        chunk = buf[i:i + 16]
        hx = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{base + i:010x}  {hx:<47}  {asc}")


with open(ISO, "rb") as f:
    f.seek(0, 2)
    size = f.tell()
    print(f"ISO size {size} bytes, {size // SECTOR} sectors; hidden area starts at sector {FS_END // SECTOR}")

    f.seek(FS_END)
    print("=== first 512 bytes of hidden area")
    hexdump(f.read(512), FS_END)

    # Magic scan at sector starts (cheap) + entropy per 64 MiB block
    print("=== magics at sector boundaries (first 40 per type)")
    found = {}
    block = 64 * 1024 * 1024
    ent_map = []
    pos = FS_END - (FS_END % SECTOR)
    f.seek(pos)
    while True:
        data = f.read(block)
        if not data:
            break
        ent_map.append((pos, entropy(data[:1 << 20])))
        for off in range(0, len(data), SECTOR):
            head = data[off:off + 4]
            name = MAGICS.get(head)
            if name:
                lst = found.setdefault(name, [])
                if len(lst) < 40:
                    lst.append(pos + off)
                else:
                    found[name + "_more"] = found.get(name + "_more", 0) + 1
        pos += len(data)
    for k, v in found.items():
        if isinstance(v, list):
            extra = found.get(k + "_more", 0)
            print(f"{k}: {len(v) + extra} hits; first: " + ", ".join(f"0x{o:x}" for o in v[:12]))
    print("=== entropy (first MiB of each 64 MiB block, 8.0 = compressed/encrypted)")
    for p, e in ent_map:
        print(f"0x{p:09x} ({p // SECTOR:>8} lba)  {e:.2f}  " + "#" * int(e * 4))
