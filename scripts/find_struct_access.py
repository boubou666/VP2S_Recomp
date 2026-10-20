"""Find accesses to the global system struct (FUN_0010d070 returns 0x1c8c80) near a field offset.

Looks after every `jal 0x10d070` for a `lui at,0x1` + immediate pattern landing in
[lo, hi) of the struct, i.e. base + 0x10000 + simm16. Usage: find_struct_access.py <lo> <hi>
"""
import re
import subprocess
import sys

lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
dis = subprocess.run(
    ["mips-linux-gnu-objdump", "-d", "-m", "mips:5900", "/work/extracted/SLES_546.45"],
    capture_output=True, text=True).stdout.splitlines()
ins = [l for l in dis if re.match(r"^\s+[0-9a-f]+:\t", l)]

imm_re = re.compile(r"(-?\d+)\((\w+)\)|,(-?\d+)$")
for i, line in enumerate(ins):
    if "jal\t0x10d070" not in line:
        continue
    window = ins[i:i + 25]
    has_lui = any(re.search(r"lui\t\w+,0x1$", w) for w in window)
    for w in window:
        m = imm_re.search(w)
        if not m:
            continue
        v = int(m.group(1) or m.group(3))
        for off in ((0x10000 + v) if has_lui else None, v):
            if off is not None and lo <= off < hi:
                print(f"{line.split(':')[0].strip()}  ->  {w.strip()}   (struct+0x{off:x})")
