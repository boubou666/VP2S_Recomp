"""Wrap an extracted code module into a minimal MIPS R5900 ELF for Ghidra and PS2Recomp.

The module file (as written by extract.py) is mapped at its load address with its
0x80-byte header in memory, exactly as the game loads it. Sections come from the header:
  .modhdr load .. load+0x80                (WA; PS2Recomp reads the resident guard here)
  .text   load+0x80 .. load+text_size      (AX; extended to the end of a function that
          runs past text_size, as in modules 2, 16, 19 and 22)
  .data   load+text_size .. +data_size     (WA)
  .bss    after the file image, bss_size   (WA, NOBITS)
CodeWarrior puts the static initializers (called through the constructor table at header
+0x18..+0x1c) at the end of the data, just before that table. When a module has some, that
part of .data becomes an executable .init section and the table a .ctors section.

Usage: python3 -I tools/wrap_elf.py <module.bin> <out.elf> [--flags-from <game ELF>]
"""
import argparse
import struct

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<8I")
SHDR = struct.Struct("<10I")
R5900_EABI64_FLAGS = 0x20924000  # e_flags of SLES_546.45 (5900, eabi64, mips3)
JR_RA = 0x03E00008


def ctor_code_range(module: bytes) -> tuple[int, int, int] | None:
    """Image offsets (code start, table start, table end) of the static initializers, if any."""
    load, text, dsize, _ = struct.unpack_from("<4I", module, 8)
    table_lo, table_hi = struct.unpack_from("<2I", module, 0x18)
    if not table_lo or table_hi <= table_lo or table_hi - load > len(module):
        return None
    pointers = [struct.unpack_from("<I", module, a - load)[0] for a in range(table_lo, table_hi, 4)]
    if not all(load + text <= p < table_lo for p in pointers):
        return None
    return min(pointers) - load, table_lo - load, table_hi - load


def text_end(module: bytes, text: int) -> int:
    """Header text end, extended to the end of a function that runs past it (module 2's last
    function ends 0x28 bytes after it), and over the C++ this-adjusting thunks some modules keep
    in their data, before the vtables (`j method`, `addiu a0,a0,-N`, two nops; module 10's at
    +0x6240 are called when leaving Logun)."""
    word = lambda off: struct.unpack_from("<I", module, off)[0] if 0 <= off <= len(module) - 4 else None
    end = header_text_end(module, text, word)
    # Also one-instruction virtual methods (`jr ra` and its delay slot, e.g. `li v0,0`), padded to 16 bytes:
    # module 27 (the tutorial) keeps them there and calls them through a vtable in another module.
    thunk = lambda at: (((word(at) or 0) >> 26 == 0x02 and (word(at + 4) or 0) >> 16 == 0x2484
                         or word(at) == JR_RA and word(at + 4) is not None and word(at + 4) != JR_RA)
                        and word(at + 8) == 0 and word(at + 12) == 0)
    tail = end + 8 if word(end) == JR_RA else end  # module 15's last function returns just past it
    aligned = (tail + 15) & ~15  # past the zero padding of the last function
    if all(word(at) == 0 for at in range(tail, aligned, 4)):
        while thunk(aligned):
            aligned += 16
            end = aligned
    return end


def header_text_end(module: bytes, text: int, word) -> int:
    back = text - 4
    while back > text - 20 and word(back) == 0:
        back -= 4
    if word(back - 4) == JR_RA or word(back) == JR_RA:  # clean end: jr ra, delay slot, padding
        return text
    for off in range(text, min(text + 0x1000, len(module) - 4), 4):
        if word(off) == JR_RA:
            return off + 8
    return text


def wrap(module: bytes, flags: int) -> bytes:
    load, text, dsize, bss = struct.unpack_from("<4I", module, 8)
    text_lo, text_hi = 0x80, text_end(module, text)
    data_lo, data_hi = text_hi, min(text + dsize, len(module))
    image_off = 0x1000  # page-aligned file offset of the module image

    # (name, type, flags, image offset, size, alignment); offsets are relative to the image.
    parts = [(".modhdr", 1, 3, 0, text_lo, 0x10),
             (".text", 1, 6, text_lo, text_hi - text_lo, 0x80)]
    init = ctor_code_range(module)
    if init:
        init_lo, table_lo, table_hi = init
        # The table may lie just past the header's data size, still inside the image.
        parts += [(".data", 1, 3, data_lo, init_lo - data_lo, 0x10),
                  (".init", 1, 6, init_lo, table_lo - init_lo, 0x10),
                  (".ctors", 1, 3, table_lo, max(data_hi, table_hi) - table_lo, 0x4)]
    else:
        parts.append((".data", 1, 3, data_lo, data_hi - data_lo, 0x10))
    parts.append((".bss", 8, 3, len(module), bss, 0x10))

    names = [p[0] for p in parts] + [".shstrtab"]
    shstr = b"\0" + b"".join(n.encode() + b"\0" for n in names)
    index = {n: shstr.index(b"\0" + n.encode() + b"\0") + 1 for n in names}
    shstr_off = image_off + len(module)
    shoff = (shstr_off + len(shstr) + 3) & ~3

    sections = [SHDR.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, kind, sflags, off, size, align in parts:
        sections.append(SHDR.pack(index[name], kind, sflags, load + off, image_off + off, size, 0, 0, align, 0))
    sections.append(SHDR.pack(index[".shstrtab"], 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))

    eh = EHDR.pack(b"\x7fELF\x01\x01\x01" + bytes(9), 2, 8, 1, load + 0x80, EHDR.size, shoff, flags,
                   EHDR.size, PHDR.size, 1, SHDR.size, len(sections), len(sections) - 1)
    ph = PHDR.pack(1, image_off, load, load, len(module), len(module) + bss, 7, 0x80)
    out = bytearray(eh + ph)
    out += bytes(image_off - len(out)) + module + shstr
    out += bytes(shoff - len(out)) + b"".join(sections)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("module")
    ap.add_argument("out")
    ap.add_argument("--flags-from", help="copy e_flags from this ELF (default: R5900 eabi64)")
    args = ap.parse_args()
    flags = R5900_EABI64_FLAGS
    if args.flags_from:
        flags = struct.unpack_from("<I", open(args.flags_from, "rb").read(), 0x24)[0]
    module = open(args.module, "rb").read()
    open(args.out, "wb").write(wrap(module, flags))
    load, text, dsize, bss = struct.unpack_from("<4I", module, 8)
    print(f"{args.out}: load 0x{load:08x}, .text 0x{load + 0x80:08x}+0x{text - 0x80:x}, "
          f".data +0x{dsize:x}, .bss +0x{bss:x}")


if __name__ == "__main__":
    main()
