"""Find functions reached only through pointers (thread entries, callbacks, vtables).

Ghidra creates functions from direct calls; a routine only ever referenced by a pointer
can be missed, and the recompiled program then has nothing to dispatch to. This scans
each input image for code pointers:
  - aligned words anywhere in the loaded image (data, tables, vtables);
  - lui + addiu/ori pairs in code (e.g. a thread entry passed to CreateThread);
and keeps targets that look like a function start: an `addiu sp,sp,-N` prologue, or the
instruction after the previous function's end (`jr ra`, `j` or `b`, its delay slot and
any zero alignment padding). Every target of a module's constructor table (header
+0x18..+0x1c) is kept as is, and so is every jal from one input into the code of another
(a module into the main ELF or another module, the main ELF into the resident core module):
Ghidra analyzes each input alone and never sees those calls.
The jal of every new entry are then followed, up to its first jr ra, until nothing new
appears. The boot ELF's single section also holds data, so its guessed pointers must land in
the blocks where Ghidra found functions. Pointers into the middle of a function (switch
tables, labels) fail both tests and are ignored, as are addresses Ghidra already knows.

Targets in the input's own code are attributed to it; a target in another input's code goes
to every input loaded at another address that has a function start there (a vtable in the
main ELF can point into the resident core module).
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
        self.sections = list(self.code)  # executable sections; self.code may be narrowed

    def in_section(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in self.sections)

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
        if self.word(addr - 8) == JR_RA:
            return True
        # Functions are often 16-byte aligned: skip the zero padding (and a nop delay slot)
        # back to the previous function's jr ra.
        back = addr - 4
        while back > addr - 20 and self.word(back) == 0:
            back -= 4
        # The previous function ends with jr ra or with an unconditional jump (j, e.g. a C++
        # this-adjusting thunk, or b = beq zero,zero), followed by its delay slot.
        ends = lambda w: w is not None and (w == JR_RA or w >> 26 == 0x02 or w >> 16 == 0x1000)
        if back == addr - 4 or not (ends(self.word(back)) or ends(self.word(back - 4))):
            return False
        if not first:  # still inside the zero padding
            return False
        # The same layout ends the static initializers before a constructor table: a first
        # word that is an address in the image is a pointer table, not code.
        return not self.load <= (first or 0) < self.load + len(self.bytes)

    def call_targets(self) -> set[int]:
        """Targets of the jal instructions in this image's code (the narrowed code blocks for the
        main ELF, whose single section also holds data)."""
        targets = set()
        for lo, hi in self.code:
            for addr in range(lo, hi, 4):
                w = self.word(addr)
                if w is not None and w >> 26 == 0x03:  # jal
                    targets.add(((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
        return targets

    def calls_from(self, start: int, limit: int = 0x4000) -> set[int]:
        """jal targets from start up to the first jr ra (a linear scan of one function)."""
        targets = set()
        for addr in range(start, start + limit, 4):
            w = self.word(addr)
            if w is None or not self.in_section(addr):
                break
            if w >> 26 == 0x03:
                targets.add(((addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
            if w == JR_RA:
                break
        return targets

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


def code_blocks(csv_path: Path, max_gap: int = 0x400) -> list[tuple[int, int]]:
    """Ranges covered by the functions Ghidra found, merging gaps up to max_gap bytes."""
    if not csv_path.exists():
        return []
    with open(csv_path, newline="") as f:
        ranges = sorted((int(row["Start"], 16), int(row["End"], 16)) for row in csv.DictReader(f))
    blocks: list[list[int]] = []
    for start, end in ranges:
        if blocks and start - blocks[-1][1] <= max_gap:
            blocks[-1][1] = max(blocks[-1][1], end)
        else:
            blocks.append([start, end])
    return [(start, end + max_gap) for start, end in blocks]


def find_entry_points(images: dict[str, Image], csvs: dict[str, Path]) -> dict[str, list[int]]:
    """Entry points per input name; 'main' must be present."""
    main = images["main"]
    known = {name: known_starts(path) for name, path in csvs.items()}
    # The boot ELF has a single section covering code and data (including data between code
    # blocks, and static initializers after the data): its code is where Ghidra found
    # functions, or data words that happen to follow a jr ra or a j would pass for functions.
    blocks = code_blocks(csvs.get("main", Path()))
    if blocks:
        main.code = blocks
    result = {name: set() for name in images}
    for name, image in images.items():
        if name != "main":
            # Static initializers listed by the module header's constructor table (+0x18..+0x1c)
            # are functions whatever their first instruction looks like.
            table_lo, table_hi = image.word(image.load + 0x18) or 0, image.word(image.load + 0x1C) or 0
            for slot in range(table_lo, table_hi if table_hi - table_lo <= 0x1000 else table_lo, 4):
                target = image.word(slot)
                if target and image.in_code(target) and target not in known.get(name, set()):
                    result[name].add(target)
        # Routines of another input called only from this one: an overlay calling the main ELF
        # or another module, or the main ELF calling into the resident core module (VP2's main
        # ELF calls 0x248dc0 in module 1). Ghidra analyzes each input alone and never sees those
        # calls. A jal target is a function entry whatever its shape.
        for target in image.call_targets():
            if image.in_section(target):
                continue
            for owner, candidate in images.items():
                if candidate.load != image.load and candidate.in_section(target) and target not in known.get(owner, set()):
                    result[owner].add(target)
        for target in image.pointers():
            if image.looks_like_function_start(target):
                owners = [name]
            else:
                # A pointer into another input's code (e.g. a vtable in the main ELF pointing
                # into the resident core module) goes to every input loaded elsewhere that has
                # a function start there; an extra entry in a module sharing the slot is harmless.
                owners = [other for other, candidate in images.items()
                          if candidate.load != image.load and candidate.looks_like_function_start(target)]
            for owner in owners:
                if target not in known.get(owner, set()):
                    result[owner].add(target)

    # Routines Ghidra missed call others it missed too: follow the jal of every new entry, up
    # to its first jr ra, until no new target appears.
    pending = [(name, target) for name, targets in result.items() for target in targets]
    while pending:
        name, start = pending.pop()
        image = images[name]
        for target in image.calls_from(start):
            for owner, candidate in images.items():
                in_owner = candidate.in_section(target) and (owner == name or candidate.load != image.load)
                if in_owner and target not in known.get(owner, set()) and target not in result[owner]:
                    result[owner].add(target)
                    pending.append((owner, target))
    return {name: sorted(targets) for name, targets in result.items()}
