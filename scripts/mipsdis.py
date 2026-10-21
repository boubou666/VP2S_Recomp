"""Minimal MIPS I (R3000, IOP) disassembler for quick reads of IRX code without binutils.

Usage: python scripts/mipsdis.py <file> <file offset of vaddr 0> <start vaddr> <end vaddr>
       e.g. python scripts/mipsdis.py build/extract/iop/Csi_.irx 0xa0 0x878 0x900
Covers the common integer, branch, load/store and COP0 forms; anything else prints as .word.
"""
import struct
import sys

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr",
           0x09: "jalr", 0x0C: "syscall", 0x0D: "break", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
           0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub",
           0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu"}
IMM = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori"}
MEM = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
       0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr"}
BRANCH = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz"}


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def disasm(word: int, pc: int) -> str:
    op, rs, rt, rd = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    sa, fn, imm = (word >> 6) & 31, word & 63, word & 0xFFFF
    if word == 0:
        return "nop"
    if op == 0 and fn in SPECIAL:
        name = SPECIAL[fn]
        if fn in (0x00, 0x02, 0x03):
            return f"{name} {R[rd]},{R[rt]},{sa}"
        if fn in (0x04, 0x06, 0x07):
            return f"{name} {R[rd]},{R[rt]},{R[rs]}"
        if fn == 0x08:
            return f"jr {R[rs]}"
        if fn == 0x09:
            return f"jalr {R[rd]},{R[rs]}"
        if fn in (0x0C, 0x0D):
            return name
        if fn in (0x10, 0x12):
            return f"{name} {R[rd]}"
        if fn in (0x11, 0x13):
            return f"{name} {R[rs]}"
        if fn in (0x18, 0x19, 0x1A, 0x1B):
            return f"{name} {R[rs]},{R[rt]}"
        return f"{name} {R[rd]},{R[rs]},{R[rt]}"
    if op == 1:
        name = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt, f"regimm{rt}")
        return f"{name} {R[rs]},{pc + 4 + s16(imm) * 4:#x}"
    if op in (2, 3):
        return f"{'j' if op == 2 else 'jal'} {((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2):#x}"
    if op in BRANCH:
        target = pc + 4 + s16(imm) * 4
        return f"{BRANCH[op]} {R[rs]},{R[rt]},{target:#x}" if op < 6 else f"{BRANCH[op]} {R[rs]},{target:#x}"
    if op in IMM:
        value = s16(imm) if op in (0x08, 0x09, 0x0A, 0x0B) else imm
        return f"{IMM[op]} {R[rt]},{R[rs]},{value:#x}" if value >= 0 else f"{IMM[op]} {R[rt]},{R[rs]},-{-value:#x}"
    if op == 0x0F:
        return f"lui {R[rt]},{imm:#x}"
    if op in MEM:
        off = s16(imm)
        return f"{MEM[op]} {R[rt]},{'-' if off < 0 else ''}{abs(off):#x}({R[rs]})"
    if op == 0x10:
        return {0: f"mfc0 {R[rt]},${rd}", 4: f"mtc0 {R[rt]},${rd}"}.get(rs, f"cop0 {word:#010x}")
    return f".word {word:#010x}"


def main() -> None:
    path, base, start, end = sys.argv[1], *(int(a, 0) for a in sys.argv[2:5])
    data = open(path, "rb").read()
    for pc in range(start, end, 4):
        word = struct.unpack_from("<I", data, base + pc)[0]
        print(f"{pc:6x}: {word:08x}  {disasm(word, pc)}")


if __name__ == "__main__":
    main()
