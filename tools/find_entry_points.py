"""Find functions reached only through pointers (thread entries, callbacks, vtables).

Ghidra creates functions from direct calls; a routine only ever referenced by a pointer
can be missed, and the recompiled program then has nothing to dispatch to. This scans
each input image for code pointers:
  - aligned words anywhere in the loaded image (data, tables, vtables);
  - lui + addiu/ori pairs in code (e.g. a thread entry passed to CreateThread);
and keeps targets that look like a function start: an `addiu sp,sp,-N` prologue, or the
instruction after a `jr ra` + delay slot. Pointers into the middle of a function (switch
tables, labels) fail both tests and are ignored, as are addresses Ghidra already knows.

Targets in the input's own code are attributed to it; targets in the main ELF's code are
attributed to the main ELF; pointers into another overlay slot are ambiguous and dropped.
"""
import csv
import struct
from pathlib import Path

JR_RA = 0x03E00008


def load_image(elf_path: Path):
    """(load address, image bytes, executable ranges) of a single-image ELF."""
    data = elf_path.read_bytes()
    phoff = struct.unpack_from("<I", data, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    load = None
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, _ = struct.unpack_from("<6I", data, phoff + i * phentsize)
        if p_type == 1 and filesz:
            load, image = vaddr, data[off:off + filesz]
            break
    code = []
    for i in range(shnum):
        sh = struct.unpack_from("<10I", data, shoff + i * shentsize)
        if sh[2] & 0x4 and sh[3]:  # SHF_EXECINSTR
            code.append((sh[3], sh[3] + sh[5]))
    if not code:  # no section headers: whole image
        code = [(load, load + len(image))]
    return load, image, code


def known_starts(csv_path: Path) -> set[int]:
    if not csv_path.exists():
        return set()
    with open(csv_path, newline="") as f:
        return {int(row["Start"], 16) for row in csv.DictReader(f)}


class Image:
    def __init__(self, elf_path: Path):
        self.load, self.bytes, self.code = load_image(elf_path)

    def in_code(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in self.code)

    def word(self, addr: int) -> int | None:
        off = addr - self.load
        if 0 <= off <= len(self.bytes) - 4:
            return struct.unpack_from("<I", self.bytes, off)[0]
        return None

    def looks_like_function_start(self, addr: int) -> bool:
        if addr & 3 or not self.in_code(addr):
            return False
        first = self.word(addr)
        if first is not None and first >> 16 == 0x27BD and first & 0x8000:  # addiu sp,sp,-N
            return True
        return self.word(addr - 8) == JR_RA

    def pointers(self):
        """Candidate code addresses referenced from this image."""
        words = struct.unpack_from(f"<{len(self.bytes) // 4}I", self.bytes)
        found = set(words)
        for i, w in enumerate(words):
            if w >> 26 != 0x0F or not self.in_code(self.load + i * 4):  # lui in code
                continue
            rt, hi = (w >> 16) & 0x1F, (w & 0xFFFF) << 16
            for nxt in words[i + 1:i + 9]:
                op, rs = nxt >> 26, (nxt >> 21) & 0x1F
                if rs == rt and op in (0x09, 0x0D):  # addiu / ori from the lui register
                    imm = nxt & 0xFFFF
                    found.add((hi + (imm - 0x10000 if op == 0x09 and imm & 0x8000 else imm)) & 0xFFFFFFFF)
                    break
        return found


def find_entry_points(images: dict[str, Image], csvs: dict[str, Path]) -> dict[str, list[int]]:
    """Entry points per input name; 'main' must be present."""
    main = images["main"]
    known = {name: known_starts(path) for name, path in csvs.items()}
    result = {name: set() for name in images}
    for name, image in images.items():
        for target in image.pointers():
            if image.looks_like_function_start(target):
                owner = name
            elif name != "main" and main.looks_like_function_start(target):
                owner = "main"
            else:
                continue
            if target not in known.get(owner, set()):
                result[owner].add(target)
    return {name: sorted(targets) for name, targets in result.items()}
