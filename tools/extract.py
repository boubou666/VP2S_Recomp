"""Valkyrie Profile 2: Silmeria (SLES-54645) disc extractor.

Reads the ISO directly (no 7-Zip, no third-party Python packages) and writes:

  <out>/iso/            files of the ISO9660 filesystem (SLES_546.45, IOPRP300.IMG, SYSTEM.CNF)
  <out>/modules/        every unique code module / overlay, decrypted and decompressed,
                        exactly as the game places it in memory (0x80-byte header first)
  <out>/modules.json    module metadata: slot, id, name, sizes, constructors, occurrences
  <out>/iop/            IOP modules (IRX) shipped in the archive
  <out>/chunks.tsv      index of every SLZ/SLE chain on the disc and what each part holds
  <out>/assets/         (with --assets) every decompressed part, ~2.7 GiB
  <out>/manifest.json   summary and validation results

Usage:
  python3 -I tools/extract.py <game.iso> <out dir> [--assets] [--jobs N]

Formats are documented in docs/AUDIT.md (sections 7.1 to 7.3).
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slz  # noqa: E402

VERSION = 1
SECTOR = 2048
SPAN = 64 << 20               # scan window per worker task
MAX_COMP = 16 << 20           # sanity bounds for chunk headers
MAX_DECOMP = 64 << 20
MAX_LINK_GAP = 4096           # 'next' may skip at most this much padding after a payload
MWO3 = 0x336F574D             # 'MWo3' (Metrowerks overlay header)
SLOT_TABLE_VADDR = 0x1C8A30   # 21 slot addresses in the main ELF
SLOT_COUNT = 21
SHT_SCE_IOPMOD = 0x70000080
ET_SCE_IOPRELEXEC = 0xFF80


# ---------------------------------------------------------------- ISO9660

def iso_root_files(f):
    """List (name, lba, size) of the files in the ISO9660 root directory."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b"CD001":
        raise SystemExit("not an ISO9660 image")
    root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
    f.seek(root_lba * SECTOR)
    data = f.read(root_size)
    files = []
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:  # records never cross sectors: skip padding
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        lba, size = struct.unpack_from("<I", data, pos + 2)[0], struct.unpack_from("<I", data, pos + 10)[0]
        flags, name_len = data[pos + 25], data[pos + 32]
        name = data[pos + 33:pos + 33 + name_len]
        if name not in (b"\0", b"\1") and not flags & 2:
            files.append((name.decode("ascii").split(";")[0], lba, size))
        pos += length
    return files


# ---------------------------------------------------------------- ELF

def elf_segments(elf: bytes):
    """PT_LOAD program headers as (offset, vaddr, filesz, memsz)."""
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz = struct.unpack_from("<6I", elf, phoff + i * phentsize)
        if p_type == 1:
            segs.append((off, vaddr, filesz, memsz))
    return segs


def irx_info(elf: bytes):
    """(name, version) from an IRX .iopmod section, or None if not an IRX."""
    if elf[:4] != b"\x7fELF" or struct.unpack_from("<H", elf, 0x10)[0] != ET_SCE_IOPRELEXEC:
        return None
    shoff = struct.unpack_from("<I", elf, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x2E)
    for i in range(shnum):
        sh = struct.unpack_from("<10I", elf, shoff + i * shentsize)
        if sh[1] == SHT_SCE_IOPMOD:
            mod = elf[sh[4]:sh[4] + sh[5]]
            ver = struct.unpack_from("<H", mod, 0x18)[0]
            return mod[0x1A:].split(b"\0")[0].decode("ascii", "replace"), f"{ver >> 8}.{ver & 0xFF:02d}"
    return "unknown", "?"


def game_layout(elf: bytes):
    """Slot table and per-slot segment sizes declared by the main ELF."""
    segs = elf_segments(elf)
    off, vaddr = segs[0][0], segs[0][1]
    table = list(struct.unpack_from(f"<{SLOT_COUNT}I", elf, SLOT_TABLE_VADDR - vaddr + off))
    declared = defaultdict(list)
    for _, va, _, memsz in segs[1:]:
        declared[va].append(memsz)
    return table, declared


# ---------------------------------------------------------------- chunk discovery

def plausible_header(buf, pos):
    if pos + 16 > len(buf):
        return None
    magic, typ, comp, decomp, nxt = slz.HEADER.unpack_from(buf, pos)
    if magic not in (b"SLZ", b"SLE") or typ > 3:
        return None
    if not (0 < comp <= MAX_COMP and 0 < decomp <= MAX_DECOMP) or comp > decomp + 64:
        return None
    if nxt and not (16 + comp <= nxt <= 16 + comp + MAX_LINK_GAP):
        return None
    return magic, typ, comp, decomp, nxt


def scan_window(args):
    """Find plausible chunk headers in one window; keep those whose start decodes."""
    iso, start = args
    with open(iso, "rb") as f:
        f.seek(start)
        buf = f.read(SPAN + MAX_COMP + 16)
    found = []
    for magic in (b"SLZ", b"SLE"):
        pos = buf.find(magic)
        while 0 <= pos < SPAN:
            h = plausible_header(buf, pos)
            if h:
                try:
                    slz.decompress_chunk(buf, pos, slz.SLE_KEY, peek=64)
                    found.append((start + pos,) + h)
                except Exception:
                    pass
            pos = buf.find(magic, pos + 1)
    return found


def build_chains(headers):
    """Group headers into chains using validated 'next' links; returns list of member offset lists."""
    by_off = {h[0]: h for h in headers}
    linked = {h[0] + h[5] for h in headers if h[5] and h[0] + h[5] in by_off}
    chains = []
    for h in sorted(headers):
        if h[0] in linked:
            continue
        members = [h[0]]
        while by_off[members[-1]][5] and members[-1] + by_off[members[-1]][5] in by_off:
            members.append(members[-1] + by_off[members[-1]][5])
        chains.append(members)
    return chains


# ---------------------------------------------------------------- part classification

def classify(part: bytes, slots: set):
    if len(part) >= 0x80:
        w = struct.unpack_from("<8I", part)
        if w[0] in (0, MWO3) and w[2] in slots:
            return "module", w
    if part[:4] == b"\x7fELF":
        return ("irx" if irx_info(part) else "elf"), None
    if len(part) >= 0x20:
        count = struct.unpack_from("<I", part, 4)[0]
        if struct.unpack_from("<I", part, 0)[0] == 0 and 0 < count <= 4096 and len(part) >= 0x10 + 16 * count:
            tags = [part[0x10 + 16 * k:0x14 + 16 * k] for k in range(min(count, 8))]
            if all(t[0] and all(48 <= b < 91 or b == 0 for b in t) for t in tags):
                return "container", None
    tag = part[:4]
    if len(tag) == 4 and all(32 <= b < 127 for b in tag):
        return "tag:" + tag.decode(), None
    return "data", None


EXT = {"module": "mod", "irx": "irx", "elf": "elf", "container": "ctr", "data": "bin"}


def process_chain(args):
    """Decompress one chain; return per-part metadata plus module/IRX payloads."""
    iso, members, slots, assets_dir = args
    with open(iso, "rb") as f:
        f.seek(members[0])
        head_last = members[-1] - members[0]
        f.seek(members[-1])
        last = slz.parse_header(f.read(16))
        f.seek(members[0])
        buf = f.read(head_last + 16 + last[2])
    parts, blobs = [], []
    for n, m in enumerate(members):
        rel = m - members[0]
        try:
            data = slz.decompress_chunk(buf, rel, slz.SLE_KEY)
        except Exception as e:
            parts.append((n, "error", 0, repr(e)[:60]))
            continue
        kind, w = classify(data, slots)
        parts.append((n, kind, len(data), ""))
        if kind in ("module", "irx"):
            blobs.append((n, kind, data))
        if assets_dir:
            ext = EXT.get(kind, kind[4:].strip().lower() or "bin")
            sub = os.path.join(assets_dir, f"{members[0] >> 28:x}")
            os.makedirs(sub, exist_ok=True)
            with open(os.path.join(sub, f"{members[0]:010x}_{n:03d}.{ext}"), "wb") as o:
                o.write(data)
    return members, parts, blobs


# ---------------------------------------------------------------- main

def safe_name(s):
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in s)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("iso")
    ap.add_argument("out")
    ap.add_argument("--assets", action="store_true", help="also dump every decompressed part (~2.7 GiB)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()

    t0 = time.time()
    out = args.out
    for d in ("iso", "modules", "iop"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    iso_size = os.path.getsize(args.iso)

    # 1. ISO9660 files
    with open(args.iso, "rb") as f:
        files = iso_root_files(f)
        for name, lba, size in files:
            f.seek(lba * SECTOR)
            with open(os.path.join(out, "iso", name), "wb") as o:
                o.write(f.read(size))
    boot = next((n for n, _, _ in files if n.startswith(("SLES", "SLUS", "SLPM", "SLPS"))), None)
    if not boot:
        raise SystemExit("boot executable not found in the ISO root")
    elf = open(os.path.join(out, "iso", boot), "rb").read()
    table, declared = game_layout(elf)
    slots = {a for a in table if a and a != 0x100000}
    print(f"[1/4] ISO filesystem: {len(files)} files, boot {boot}")

    # 2. Chunk headers and chains
    with Pool(args.jobs) as pool:
        headers = [h for part in pool.map(scan_window, [(args.iso, s) for s in range(0, iso_size, SPAN)])
                   for h in part]
    chains = build_chains(headers)
    print(f"[2/4] chunk headers: {len(headers)}, chains: {len(chains)}  ({time.time() - t0:.0f}s)")

    # 3. Decompress every chain, classify parts, collect modules and IRX
    assets_dir = os.path.join(out, "assets") if args.assets else None
    jobs = [(args.iso, c, slots, assets_dir) for c in chains]
    jobs.sort(key=lambda j: j[1][0])
    modules = {}
    irx = {}
    kinds = Counter()
    errors = []
    decompressed = 0
    with open(os.path.join(out, "chunks.tsv"), "w") as idx:
        idx.write("offset\tlba\tparts\tkinds\n")
        with Pool(args.jobs) as pool:
            for members, parts, blobs in pool.imap(process_chain, jobs, chunksize=8):
                for n, kind, size, err in parts:
                    kinds[kind.split(":")[0]] += 1
                    decompressed += size
                    if kind == "error":
                        errors.append((members[n], err))
                idx.write(f"{members[0]}\t{members[0] // SECTOR}\t{len(members)}\t"
                          + ",".join(k for _, k, _, _ in parts) + "\n")
                for n, kind, data in blobs:
                    h = hashlib.sha1(data).hexdigest()
                    where = {"offset": members[n], "chain": members[0], "part": n}
                    store = modules if kind == "module" else irx
                    if h not in store:
                        store[h] = {"data": data, "occurrences": []}
                    store[h]["occurrences"].append(where)
    print(f"[3/4] decompressed {decompressed / 2**30:.2f} GiB, parts by kind {dict(kinds)}, "
          f"errors {len(errors)}  ({time.time() - t0:.0f}s)")

    # 4. Export modules and IRX, validate against the ELF
    mod_meta = []
    valid = 0
    for h, m in sorted(modules.items(), key=lambda kv: struct.unpack_from("<II", kv[1]["data"], 4)[::-1]):
        d = m["data"]
        w = struct.unpack_from("<8I", d)
        name = d[0x20:0x60].split(b"\0")[0].decode("ascii", "replace") if w[0] == MWO3 else ""
        mem_size = len(d) + w[5]
        ok = any((mem_size + 0x7F) & ~0x7F == s or mem_size == s for s in declared.get(w[2], []))
        valid += ok
        fname = f"{w[2]:08x}_{w[1]:03d}" + (f"_{safe_name(os.path.splitext(name)[0])}" if name else "") + ".bin"
        with open(os.path.join(out, "modules", fname), "wb") as o:
            o.write(d)
        mod_meta.append({
            "file": fname, "id": w[1], "name": name or None, "format": "MWo3" if w[0] == MWO3 else "module",
            "load": f"0x{w[2]:08x}", "slots": [i for i, a in enumerate(table) if a == w[2]],
            "code_start": f"0x{w[2] + 0x80:08x}", "text_size": w[3], "data_size": w[4], "bss_size": w[5],
            "ctors": [f"0x{w[6]:08x}", f"0x{w[7]:08x}"], "size": len(d), "sha1": h,
            "matches_elf_segment": ok, "occurrences": m["occurrences"],
        })
    irx_meta = []
    for h, m in irx.items():
        name, ver = irx_info(m["data"])
        fname = f"{safe_name(name)}.irx"
        with open(os.path.join(out, "iop", fname), "wb") as o:
            o.write(m["data"])
        irx_meta.append({"file": fname, "name": name, "version": ver, "size": len(m["data"]), "sha1": h,
                         "occurrences": m["occurrences"]})

    with open(os.path.join(out, "modules.json"), "w") as o:
        json.dump(mod_meta, o, indent=1)
    found_slots = Counter(m["load"] for m in mod_meta)
    slot_report = []
    for i, a in enumerate(table):
        if a and a != 0x100000:
            slot_report.append({"slot": i, "address": f"0x{a:08x}", "declared": len(declared.get(a, [])),
                                "found": found_slots.get(f"0x{a:08x}", 0)})
    manifest = {
        "tool": "VP2S_Recomp tools/extract.py", "version": VERSION,
        "iso": {"path": os.path.basename(args.iso), "size": iso_size, "boot": boot,
                "boot_sha1": hashlib.sha1(elf).hexdigest()},
        "chunk_headers": len(headers), "chains": len(chains), "parts_by_kind": dict(kinds),
        "decompressed_bytes": decompressed, "errors": [{"offset": o, "error": e} for o, e in errors],
        "modules": {"unique": len(mod_meta), "match_elf_segments": valid},
        "iop_modules": sorted(m["name"] for m in irx_meta),
        "slots": slot_report, "assets_dumped": bool(args.assets),
        "seconds": round(time.time() - t0),
    }
    with open(os.path.join(out, "manifest.json"), "w") as o:
        json.dump(manifest, o, indent=1)
    print(f"[4/4] modules: {len(mod_meta)} unique ({valid} match their ELF segment), IOP modules: {len(irx_meta)}")
    for s in slot_report:
        if s["found"] or s["declared"]:
            print(f"      slot {s['slot']:2} {s['address']}: {s['found']:3} found / {s['declared']:3} declared")
    print(f"done in {time.time() - t0:.0f}s -> {out}")


if __name__ == "__main__":
    main()
