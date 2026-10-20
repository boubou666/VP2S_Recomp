"""Find references to given absolute addresses in code (lui+imm pairs) and data (raw words).

Scans the main ELF code segment plus every extracted module/overlay.
Usage: find_addr_refs.py <addr> [addr...]   (hex)
"""
import glob
import re
import struct
import subprocess
import sys

targets = {int(a, 16) for a in sys.argv[1:]}


def images():
    elf = open("/work/extracted/SLES_546.45", "rb").read()
    yield "main", 0x100000, elf[0x2E00:0x2E00 + 0xc8b00]
    # Module headers stay in memory: file offset 0 sits at the slot address (code at +0x80)
    for p in sorted(glob.glob("/work/work/parts/*.bin")):
        d = open(p, "rb").read()
        w = struct.unpack_from("<8I", d)
        yield p.split("/")[-1], w[2], d


lui_re = re.compile(r"lui\t(\w+),0x([0-9a-f]+)$")
imm_re = re.compile(r"\t(addiu|ori|lw|sw|lq|sq|ld|sd|lbu|sb|lhu|sh)\t\w+,(?:(-?\d+)\((\w+)\)|(\w+),(-?\w+))$")

for name, base, img in images():
    open("/tmp/img.bin", "wb").write(img)
    dis = subprocess.run(["mips-linux-gnu-objdump", "-D", "-b", "binary", "-m", "mips:5900", "-EL",
                          f"--adjust-vma=0x{base:x}", "/tmp/img.bin"], capture_output=True, text=True).stdout
    lines = [l for l in dis.splitlines() if re.match(r"^\s+[0-9a-f]+:\t", l)]
    hits = []
    for i, l in enumerate(lines):
        m = lui_re.search(l)
        if not m:
            continue
        reg, hi = m.group(1), int(m.group(2), 16)
        for l2 in lines[i + 1:i + 10]:
            m2 = imm_re.search(l2)
            if not m2:
                continue
            if m2.group(2) is not None and m2.group(3) == reg:
                lo = int(m2.group(2))
            elif m2.group(4) == reg and m2.group(5) is not None:
                v = m2.group(5)
                lo = int(v, 16) if v.startswith("0x") else int(v)
                if m2.group(1) == "ori":
                    lo &= 0xFFFF
            else:
                continue
            addr = ((hi << 16) + lo) & 0xFFFFFFFF
            if addr in targets:
                hits.append(f"code {l.split(':')[0].strip()} -> 0x{addr:x}")
            break
    words = struct.unpack_from(f"<{len(img) // 4}I", img)
    for i, w in enumerate(words):
        if w in targets:
            hits.append(f"data 0x{base + i * 4:x} = 0x{w:x}")
    if hits:
        print(f"== {name} ({len(hits)})")
        for h in hits[:40]:
            print("  ", h)
