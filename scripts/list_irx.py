"""List IOP modules (IRX) among decompressed samples: name and version from the .iopmod section."""
import glob
import struct

from elftools.elf.elffile import ELFFile

for path in sorted(glob.glob("/work/work/slz_samples/*.bin")):
    with open(path, "rb") as f:
        if f.read(4) != b"\x7fELF":
            continue
        f.seek(0)
        elf = ELFFile(f)
        name, ver = "?", 0
        for sec in elf.iter_sections():
            if sec["sh_type"] == 0x70000080:  # SHT_SCE_IOPMOD
                d = sec.data()
                ver = struct.unpack_from("<H", d, 0x18)[0]
                name = d[0x1A:].split(b"\0")[0].decode("ascii", "replace")
        size = sum(s["p_filesz"] for s in elf.iter_segments())
        print(f"{path.split('/')[-1]:<20} {name:<14} v{ver >> 8}.{ver & 0xff:02d}  {size:>7} bytes")
