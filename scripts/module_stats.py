"""Code statistics for extracted overlay modules (and the main ELF, for calibration).

Per module: text size, function count (stack-frame prologues), VU0 macro / MMI / quadword
instruction counts, and VU1 microcode uploads (VIF MPG with VU-looking payload).
Writes /work/work/module_stats.tsv and prints totals per slot.
"""
import glob
import os
import re
import struct
import subprocess
from collections import defaultdict

VU_UPPER_NOP = 0x000002FF
VU0 = re.compile(r"^(v[a-z]+|qmfc2|qmtc2|lqc2|sqc2|cfc2|ctc2|vcallms[r]?)(\.|\t|$)")
MMI = re.compile(r"^(p(add|sub|ext|pac|cpy|madd|msub|mult|div|mfh|mth|max|min|ceq|cgt|and|or|xor|nor|sll|srl|sra|abs|inth|intel|exe|rev|rot|lzcw)|qfsrv|m[ft](hi|lo)1|mult1|multu1|div1|divu1|madd1)")


def stats(code: bytes, base: int):
    tmp = "/tmp/code.bin"
    open(tmp, "wb").write(code)
    dis = subprocess.run(["mips-linux-gnu-objdump", "-D", "-b", "binary", "-m", "mips:5900", "-EL",
                          f"--adjust-vma=0x{base:x}", tmp], capture_output=True, text=True).stdout
    funcs = vu0 = mmi = quad = total = 0
    for line in dis.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        m = parts[2].strip()
        total += 1
        if m == "addiu" and parts[3].startswith("sp,sp,-"):
            funcs += 1
        elif VU0.match(m):
            vu0 += 1
        elif m in ("lq", "sq"):
            quad += 1
        elif MMI.match(m):
            mmi += 1
    words = struct.unpack_from(f"<{len(code) // 4}I", code)
    mpg = vu1 = 0
    for i, w in enumerate(words):
        if (w >> 24) & 0x7F == 0x4A:
            num = (w >> 16) & 0xFF or 256
            nxt = words[i + 1:i + 1 + num * 2]
            if len(nxt) == num * 2 and VU_UPPER_NOP in nxt[1::2]:
                mpg += 1
                vu1 += num
    return dict(funcs=funcs, insns=total, vu0=vu0, mmi=mmi, quad=quad, mpg=mpg, vu_insns=vu1)


rows = []
# Calibration: main ELF code segment
elf = open("/work/extracted/SLES_546.45", "rb").read()
rows.append(("main", "SLES_546.45", 0xc8b00, stats(elf[0x2E00:0x2E00 + 0xc8b00], 0x100000)))

# Unique modules from scan_parts.py. The 0x80-byte header stays in memory at the slot
# address, so code starts at slot + 0x80; word3 (text size) counts from the header start.
for path in sorted(glob.glob("/work/work/parts/*.bin")):
    data = open(path, "rb").read()
    w = struct.unpack_from("<8I", data)
    slot, text = w[2], w[3]
    code = data[0x80:text]
    rows.append((f"0x{slot:08x}", os.path.basename(path), len(code), stats(code, slot + 0x80)))

keys = ["funcs", "insns", "vu0", "mmi", "quad", "mpg", "vu_insns"]
with open("/work/work/module_stats.tsv", "w") as o:
    o.write("slot\tfile\ttext\t" + "\t".join(keys) + "\n")
    for slot, name, text, s in rows:
        o.write(f"{slot}\t{name}\t{text}\t" + "\t".join(str(s[k]) for k in keys) + "\n")

per_slot = defaultdict(lambda: defaultdict(int))
for slot, name, text, s in rows:
    per_slot[slot]["modules"] += 1
    per_slot[slot]["text"] += text
    for k in keys:
        per_slot[slot][k] += s[k]

print(f"{'slot':<12}{'mods':>5}{'text KiB':>10}{'funcs':>8}{'insns':>9}{'vu0':>7}{'mmi':>7}{'lq/sq':>7}{'mpg':>5}{'vu1 ins':>9}")
tot = defaultdict(int)
for slot in sorted(per_slot):
    d = per_slot[slot]
    print(f"{slot:<12}{d['modules']:>5}{d['text'] / 1024:>10.0f}{d['funcs']:>8}{d['insns']:>9}{d['vu0']:>7}{d['mmi']:>7}{d['quad']:>7}{d['mpg']:>5}{d['vu_insns']:>9}")
    if slot != "main":
        for k, v in d.items():
            tot[k] += v
print(f"{'overlays':<12}{tot['modules']:>5}{tot['text'] / 1024:>10.0f}{tot['funcs']:>8}{tot['insns']:>9}{tot['vu0']:>7}{tot['mmi']:>7}{tot['quad']:>7}{tot['mpg']:>5}{tot['vu_insns']:>9}")
