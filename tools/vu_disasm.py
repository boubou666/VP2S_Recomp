"""VU1 microcode disassembler (PS2 VU instruction set), for reading a game's microprograms.

    python tools/vu_disasm.py <code.bin> [start_pc_hex] [count]

<code.bin> is a VU1 code image as ps2_vu1_replay dumpcode writes it (16 KiB, little endian). Each line is one
instruction pair: address, upper (FMAC) and lower instruction, with the E/I/M/D/T bits of the upper word. When the
upper I bit is set the lower word is the float loaded into I.
"""
import struct
import sys

BC = 'xyzw'


def dest(d):
    return ''.join(c for c, bit in zip('xyzw', (8, 4, 2, 1)) if d & bit) or '-'


def upper(word):
    d = (word >> 21) & 0xF
    ft = (word >> 16) & 0x1F
    fs = (word >> 11) & 0x1F
    fd = (word >> 6) & 0x1F
    op = word & 0x3F
    x = '.' + dest(d)
    simple = {
        0x1C: ('mulq', 'fd fs q'), 0x1D: ('maxi', 'fd fs i'), 0x1E: ('muli', 'fd fs i'), 0x1F: ('minii', 'fd fs i'),
        0x20: ('addq', 'fd fs q'), 0x21: ('maddq', 'fd fs q'), 0x22: ('addi', 'fd fs i'), 0x23: ('maddi', 'fd fs i'),
        0x24: ('subq', 'fd fs q'), 0x25: ('msubq', 'fd fs q'), 0x26: ('subi', 'fd fs i'), 0x27: ('msubi', 'fd fs i'),
        0x28: ('add', 'fd fs ft'), 0x29: ('madd', 'fd fs ft'), 0x2A: ('mul', 'fd fs ft'), 0x2B: ('max', 'fd fs ft'),
        0x2C: ('sub', 'fd fs ft'), 0x2D: ('msub', 'fd fs ft'), 0x2E: ('opmsub', 'fd fs ft'), 0x2F: ('mini', 'fd fs ft'),
    }
    regs = {'fd': 'vf%02d' % fd, 'fs': 'vf%02d' % fs, 'ft': 'vf%02d' % ft, 'q': 'Q', 'i': 'I'}
    if op < 0x1C:
        names = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul']
        return '%s%s%s vf%02d, vf%02d, vf%02d%s' % (names[op >> 2], BC[op & 3], x, fd, fs, ft, BC[op & 3])
    if op in simple:
        name, args = simple[op]
        return '%s%s %s' % (name, x, ', '.join(regs[a] for a in args.split()))
    if op >= 0x3C:
        sp = (((word >> 6) & 0x1F) << 2) | (op & 3)
        if sp < 0x10:
            names = ['adda', 'suba', 'madda', 'msuba']
            return '%s%s%s ACC, vf%02d, vf%02d%s' % (names[sp >> 2], BC[sp & 3], x, fs, ft, BC[sp & 3])
        if 0x18 <= sp <= 0x1B:
            return 'mula%s%s ACC, vf%02d, vf%02d%s' % (BC[sp & 3], x, fs, ft, BC[sp & 3])
        table = {
            0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
            0x16: 'ftoi12', 0x17: 'ftoi15', 0x1D: 'abs',
        }
        if sp in table:
            return '%s%s vf%02d, vf%02d' % (table[sp], x, ft, fs)
        acc = {
            0x1C: ('mulaq', 'fs q'), 0x1E: ('mulai', 'fs i'), 0x20: ('addaq', 'fs q'), 0x21: ('maddaq', 'fs q'),
            0x22: ('addai', 'fs i'), 0x23: ('maddai', 'fs i'), 0x24: ('subaq', 'fs q'), 0x25: ('msubaq', 'fs q'),
            0x26: ('subai', 'fs i'), 0x27: ('msubai', 'fs i'), 0x28: ('adda', 'fs ft'), 0x29: ('madda', 'fs ft'),
            0x2A: ('mula', 'fs ft'), 0x2C: ('suba', 'fs ft'), 0x2D: ('msuba', 'fs ft'), 0x2E: ('opmula', 'fs ft'),
        }
        if sp in acc:
            name, args = acc[sp]
            return '%s%s ACC, %s' % (name, x, ', '.join(regs[a] for a in args.split()))
        if sp == 0x1F:
            return 'clipw.xyz vf%02d, vf%02d' % (fs, ft)
        if sp == 0x2F:
            return 'nop'
    return 'upper?%08x' % word


def simm(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def lower(word, pc):
    op = (word >> 25) & 0x7F
    d = (word >> 21) & 0xF
    it = (word >> 16) & 0x1F
    is_ = (word >> 11) & 0x1F
    imm11 = simm(word & 0x7FF, 11)
    x = '.' + dest(d)
    target = lambda: '0x%04x' % ((pc + 8 + imm11 * 8) & 0x3FFF)
    if op == 0x00:
        return 'lq%s vf%02d, %d(vi%02d)' % (x, it, imm11, is_)
    if op == 0x01:
        return 'sq%s vf%02d, %d(vi%02d)' % (x, is_, imm11, it)
    if op == 0x04:
        return 'ilw%s vi%02d, %d(vi%02d)' % (x, it, imm11, is_)
    if op == 0x05:
        return 'isw%s vi%02d, %d(vi%02d)' % (x, it, imm11, is_)
    if op in (0x08, 0x09):
        imm15 = (((word >> 21) & 0xF) << 11) | (word & 0x7FF)
        return '%s vi%02d, vi%02d, 0x%x' % ('iaddiu' if op == 0x08 else 'isubiu', it, is_, imm15)
    flags = {0x10: 'fceq', 0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand',
             0x17: 'fsor', 0x18: 'fmeq', 0x1A: 'fmand', 0x1B: 'fmor', 0x1C: 'fcget'}
    if op in flags:
        return '%s vi%02d, 0x%x' % (flags[op], it, word & 0xFFFFFF)
    branches = {0x20: 'b', 0x21: 'bal', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez',
                0x2F: 'ibgez'}
    if op in branches:
        name = branches[op]
        if op == 0x20:
            return 'b %s' % target()
        if op == 0x21:
            return 'bal vi%02d, %s' % (it, target())
        if op in (0x28, 0x29):
            return '%s vi%02d, vi%02d, %s' % (name, it, is_, target())
        return '%s vi%02d, %s' % (name, is_, target())
    if op == 0x24:
        return 'jr vi%02d' % is_
    if op == 0x25:
        return 'jalr vi%02d, vi%02d' % (it, is_)
    if op == 0x40:
        funct = word & 0x3F
        fd = (word >> 6) & 0x1F
        if funct == 0x30:
            return 'iadd vi%02d, vi%02d, vi%02d' % (fd, is_, it)
        if funct == 0x31:
            return 'isub vi%02d, vi%02d, vi%02d' % (fd, is_, it)
        if funct == 0x32:
            return 'iaddi vi%02d, vi%02d, %d' % (it, is_, simm(fd, 5))
        if funct == 0x34:
            return 'iand vi%02d, vi%02d, vi%02d' % (fd, is_, it)
        if funct == 0x35:
            return 'ior vi%02d, vi%02d, vi%02d' % (fd, is_, it)
        if funct >= 0x3C:
            sp = (((word >> 6) & 0x1F) << 2) | (funct & 3)
            table = {
                0x30: 'move%s vf%02d, vf%02d' % (x, it, is_), 0x31: 'mr32%s vf%02d, vf%02d' % (x, it, is_),
                0x34: 'lqi%s vf%02d, (vi%02d++)' % (x, it, is_), 0x35: 'sqi%s vf%02d, (vi%02d++)' % (x, is_, it),
                0x36: 'lqd%s vf%02d, (--vi%02d)' % (x, it, is_), 0x37: 'sqd%s vf%02d, (--vi%02d)' % (x, is_, it),
                0x38: 'div Q, vf%02d.%s, vf%02d.%s' % (is_, BC[(word >> 21) & 3], it, BC[(word >> 23) & 3]),
                0x39: 'sqrt Q, vf%02d.%s' % (it, BC[(word >> 23) & 3]),
                0x3A: 'rsqrt Q, vf%02d.%s, vf%02d.%s' % (is_, BC[(word >> 21) & 3], it, BC[(word >> 23) & 3]),
                0x3B: 'waitq', 0x3C: 'mtir vi%02d, vf%02d.%s' % (it, is_, BC[(word >> 21) & 3]),
                0x3D: 'mfir%s vf%02d, vi%02d' % (x, it, is_), 0x3E: 'ilwr%s vi%02d, (vi%02d)' % (x, it, is_),
                0x3F: 'iswr%s vi%02d, (vi%02d)' % (x, it, is_), 0x40: 'rnext%s vf%02d, R' % (x, it),
                0x41: 'rget%s vf%02d, R' % (x, it), 0x42: 'rinit R, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
                0x43: 'rxor R, vf%02d.%s' % (is_, BC[(word >> 21) & 3]), 0x64: 'mfp%s vf%02d, P' % (x, it),
                0x68: 'xtop vi%02d' % it, 0x69: 'xitop vi%02d' % it, 0x6C: 'xgkick vi%02d' % is_,
                0x70: 'esadd P, vf%02d' % is_, 0x71: 'ersadd P, vf%02d' % is_, 0x72: 'eleng P, vf%02d' % is_,
                0x73: 'erleng P, vf%02d' % is_, 0x74: 'eatanxy P, vf%02d' % is_, 0x75: 'eatanxz P, vf%02d' % is_,
                0x76: 'esum P, vf%02d' % is_, 0x78: 'esqrt P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
                0x79: 'ersqrt P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
                0x7A: 'ercpr P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]), 0x7B: 'waitp',
                0x7C: 'esin P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
                0x7D: 'eatan P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
                0x7E: 'eexp P, vf%02d.%s' % (is_, BC[(word >> 21) & 3]),
            }
            if sp in table:
                return table[sp]
            if word == 0x8000033C:
                return 'nop'
    return 'lower?%08x' % word


def main():
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else len(data) // 8
    for pc in range(start, min(len(data), start + count * 8), 8):
        lo, up = struct.unpack_from('<II', data, pc)
        bits = ''.join(c if up & b else '.' for c, b in zip('IEMDT', (1 << 31, 1 << 30, 1 << 29, 1 << 28, 1 << 27)))
        u = upper(up & 0x07FFFFFF)
        if up & (1 << 31):
            low = 'loi %g' % struct.unpack('<f', struct.pack('<I', lo))[0]
        else:
            low = lower(lo, pc)
        print('%04x %s  %-40s %s' % (pc, bits, u, low))
        if up & (1 << 30):
            print('     ---- (end after the next pair)')


if __name__ == '__main__':
    main()
