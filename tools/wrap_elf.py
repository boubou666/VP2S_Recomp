"""Wrap an extracted code module into a minimal MIPS R5900 ELF for Ghidra and PS2Recomp.

The module file (as written by extract.py) is mapped at its load address with its
0x80-byte header in memory, exactly as the game loads it. Sections come from the header:
  .modhdr load .. load+0x80                (WA; PS2Recomp reads the resident guard here)
  .text   load+0x80 .. load+text_size      (AX)
  .data   load+text_size .. +data_size     (WA)
  .bss    after the file image, bss_size   (WA, NOBITS)

Usage: python3 -I tools/wrap_elf.py <module.bin> <out.elf> [--flags-from <game ELF>]
"""
import argparse
import struct

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<8I")
SHDR = struct.Struct("<10I")
R5900_EABI64_FLAGS = 0x20924000  # e_flags of SLES_546.45 (5900, eabi64, mips3)


def wrap(module: bytes, flags: int) -> bytes:
    load, text, dsize, bss = struct.unpack_from("<4I", module, 8)
    text_lo, text_hi = 0x80, text
    data_lo, data_hi = text, min(text + dsize, len(module))
    shstr = b"\0.modhdr\0.text\0.data\0.bss\0.shstrtab\0"
    names = {n: shstr.index(n.encode() + b"\0") for n in (".modhdr", ".text", ".data", ".bss", ".shstrtab")}

    image_off = 0x1000  # page-aligned file offset of the module image
    shstr_off = image_off + len(module)
    shoff = (shstr_off + len(shstr) + 3) & ~3

    eh = EHDR.pack(b"\x7fELF\x01\x01\x01" + bytes(9), 2, 8, 1, load + 0x80, EHDR.size, shoff, flags,
                   EHDR.size, PHDR.size, 1, SHDR.size, 6, 5)
    ph = PHDR.pack(1, image_off, load, load, len(module), len(module) + bss, 7, 0x80)
    sections = [
        SHDR.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        SHDR.pack(names[".modhdr"], 1, 3, load, image_off, text_lo, 0, 0, 0x10, 0),
        SHDR.pack(names[".text"], 1, 6, load + text_lo, image_off + text_lo, text_hi - text_lo, 0, 0, 0x80, 0),
        SHDR.pack(names[".data"], 1, 3, load + data_lo, image_off + data_lo, data_hi - data_lo, 0, 0, 0x10, 0),
        SHDR.pack(names[".bss"], 8, 3, load + len(module), image_off + len(module), bss, 0, 0, 0x10, 0),
        SHDR.pack(names[".shstrtab"], 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
    ]
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
