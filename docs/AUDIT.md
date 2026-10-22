# Feasibility audit — native PC port of Valkyrie Profile 2: Silmeria

Version analyzed: **SLES-54645** (PAL, France), 4.35 GB ISO. Audit dated 2026-10-20 (phases 1 and 2).

## Verdict

**GO for static recompilation, with conditions** (end-of-phase-2 decision, see section 8).

Phase 1 found nothing blocking. Phase 2 resolved the main unknowns:
- **SLZ** compression (4 types) is reimplemented and validated;
- **SLE** encryption is broken: the key is recovered and validated;
- the **`MWo3`** overlay format (Metrowerks CodeWarrior) is decoded;
- **≈ 3.3 MB of code** (≈ 7,600 functions) is extracted and disassembles cleanly.

The "mode B" slots (combat?) declared in the ELF **cannot be found on the disc**, whatever static method is used. They are probably linker leftovers, but only a memory capture during a battle can confirm this (section 7.6).

## 1. Disc layout

| Area (LBA) | Size | Contents |
|---|---|---|
| 0–820 | 1.6 MB | ISO9660 file system: `SLES_546.45` (executable), `IOPRP300.IMG` (IOP modules), `SYSTEM.CNF` |
| 1024–1041 | 36 KB | High-entropy data: **probably the file table** (encrypted or compressed) |
| 1042–254993 | ≈ 500 MB | Zeros (padding to place data toward the outer edge of the DVD) |
| 254994–2279407 | ≈ 4.1 GB | **Hidden archive**, invisible to ISO9660, read as raw sectors |

Contents identified in the archive:
- **tri-Ace SLZ compression** (`SLZ` + type 0 to 3 + compressed size + decompressed size + next). The first block is the compressed `sio2man` IOP module. Only 82 SLZ blocks start on a sector boundary; the other files are inside unaligned **containers**.
- `ANIM` / `CHAR` block tables (little-endian FourCC): animations and characters.
- `SEQW` (1,825 occurrences) and `spcm` (100): probably streamed voices and music.
- Two large areas with entropy 8.0 (≈ 1.3 GB at the start, and the end of the disc): probably the **FMVs**, in a non-standard container (no MPEG-PS header found).

## 2. Main executable (`SLES_546.45`)

- MIPS R5900 ELF (Emotion Engine), entry point `0x100008`, 822 KB main segment (`0x100000`–`0x1e7d80`).
- **No symbols**: empty symbol table and section names stripped.
- Ghidra 12.1.4 + EE plugin: **972 functions**, ≈ 67,000 instructions.
- Few PS2-specific instructions: **24 VU0 macro**, **110 MMI** (128-bit), 1,947 `lq`/`sq`, 1,253 FPU.
- **Sony SDK 3.0** libraries identified: libcdvd, libdma, libgraph (3.0.1), libkernl, libmc, libpad2, libdbc, sifrpc, stdio. They are standard, so they can be replaced by a PC implementation (HLE) instead of being recompiled.

Key functions identified:

| Address | Role |
|---|---|
| `FUN_00119a60` (8 KB, the largest) | **Hard disk installer** (corrected in phase 2): creates the `PP.SLES-54645` partition and writes `vp2.bin` to it via fileXio. It is not the read layer. |
| `FUN_00100230` | IOP boot (`cdrom0:\IOPRP300.IMG;1`) |
| `FUN_001025d0` | SLZ entry point: `SLZ`/`SLE` signature, decryption, type selection, block chaining |
| `FUN_00101ea0` / `FUN_001018d0` / `FUN_00101370` | SLZ type 1 / 2 / 3 decompressors |
| `FUN_00102360`, `FUN_001059b0` | SLE decryption, SLZ stream opening |

The game **can read all of its data from a single `vp2.bin` file** (PS2 hard disk installation). The hidden archive on the DVD is therefore, in practice, that same `vp2.bin`. This is very favorable for the port.

## 3. Overlays

365 zero-size segments in the file, spread over 20 memory slots (≈ 7 MB in total). Their contents are loaded from the archive at run time.

| Slot | Count | Max size | Note |
|---|---|---|---|
| `0x1e7d80`, `0x35ec80`, `0x36d800` | 1 + 3 + 1 | 1.2–1.7 MB | Probably buffers or heaps, not code |
| `0x48ea00`, `0x495000` | 10 + 12 | 26–105 KB | Game modules (scenes, systems)? |
| `0x514100` | 21 | 60 KB | same |
| `0x523280` … `0x52e680` | **4 × 15** | 15 KB | Identical parallel slots |
| `0x532280` | 57 | 33 KB | |
| `0x53a400`, `0x53d880` | **2 × 45** | 13 KB | Identical parallel slots |
| `0x540d00` | 12 | 7.5 KB | |
| `0x542b00` … `0x547780` | **4 × 24** | 6.5 KB | Identical parallel slots |

**Hypothesis to verify**: the groups of 4 identical slots correspond to the 4 party members in battle, and the groups of 2 to enemies (action and AI modules loaded in parallel).

Consequence for static recompilation: each overlay must be extracted, decompressed, then recompiled **for its load address**. The runtime must also know which one is resident in each slot in order to dispatch calls.

## 4. VU microcode

In the main executable:
- **one main VU1 program of about 1,150 instructions** (≈ 9 KB), sent in 5 VIF `MPG` commands (from `0x14a294` to `0x14c6a8`) to VU addresses `0x0dc`–`0x55a`. This is probably the 3D rendering engine;
- an 11-instruction mini-routine (`0x145d14`).

This size remains **manageable**: it can first be interpreted, then hand-translated into a shader or C. The overlays may contain other VU programs (battle effects, for example); to be checked.

## 5. Strengths and weaknesses

**Strengths**
- Standard Sony SDK (3.0), recognizable by signature and replaceable with HLE.
- **Compressed** data (SLZ is the format of the tri-Ace engine shared with Star Ocean 3). *Phase 2: only the code is encrypted (SLE), and the encryption is broken.*
- Centralized file access layer (`vp2.bin`).
- Reasonably sized VU1 microcode in the executable.
- Little MMI and VU0 in the main executable.
- Ghidra and the EE plugin recognize the binary automatically.

**Weaknesses**
- No symbols.
- Most of the game code is in ≈ 365 overlays **not yet extracted**: the real code size remains unknown.
- File table (LBA 1024) not decoded, possibly encrypted.
- Proprietary formats: containers, `SEQW`, `spcm`, FMV.
- Custom IOP modules (sound driver…) to reimplement with HLE.
- No existing community project (decompilation or recomp) to build on.

## 6. Recommended strategy

1. **Static recompilation of the EE**: main executable + each overlay at its address, translated from MIPS R5900 to C.
2. **SDK HLE**: libcdvd, libpad2, libmc and libgraph reimplemented for PC; the `FUN_00119a60` layer is redirected to `vp2.bin` extracted from the ISO.
3. **Graphics**: a GS emulation layer first (to get an image), then a native renderer.
4. **VU1**: VU interpreter at first, translation later.
5. **IOP / audio**: HLE of the sound modules based on their RPC commands.

First target milestone: **the title screen displayed natively**.

Order of magnitude, pending phase 2: **6 to 18+ months** of iterative part-time work, with testing at every step.

## 7. Phase 2 — results

### 7.1 SLZ compression

Reimplemented in `tools/slz.py` from the game's routines. These are LZSS variants: a flag byte or word, references with a 12-bit distance and a 4-bit length.

| Type | Algorithm | End of stream |
|---|---|---|
| 0 | stored | size |
| 1 | byte LZSS | distance 0 |
| 2 | byte LZSS + RLE (length 0xF = fill) | decompressed size |
| 3 | 16-bit word LZSS (length + 2 words) | distance 0 |

Validation: the 82 sector-aligned blocks decompress to the exact size in their header. Across the whole disc, **25,558 files** (1.6 GB compressed, **2.72 GiB decompressed**) pass, with only 2 failures.

### 7.2 SLE encryption

`SLE` is an SLZ block whose payload is encrypted: `p = (c - k) XOR key[i % 16]`, where `k = 3, 6, 9…` (modulo 256).

The key (16 bytes, read by the game at `0x1d5930`, in BSS, set at run time) was **recovered by frequency analysis** on the two aligned SLE blocks. It was validated by decompressing to the exact size of these two blocks, then of the 125 others. The key is in `tools/slz.py`.

**Only the code is encrypted**: all 127 SLE blocks are modules or overlays.

The LBA 1024 area (36 KB) does not use this encryption. *Update (first boot of the port): it is indeed the **file table**, encrypted differently.* At startup, `FUN_0011ebc0` reads it (LBA `0x400`, 0x9000 bytes) through the CD manager (`0x1ca440`), then decrypts it by XOR with an xorshift-style pseudo-random stream, seeded with the constant `0x49287491`, over three 0x3000-byte bands. This encryption still has to be reimplemented to read the table outside the game.

### 7.3 Modules and overlays

Common header (0x80 bytes), with the address and sizes at the same positions:
`[0]` 0 or `'MWo3'` · `[1]` identifier · `[2]` load address · `[3]` code size (header included) · `[4]` data size · `[5]` BSS size · `[6..7]` start and end of the C++ constructor table · `[0x20..]` **original file name** (`MWo3` only).

**The header stays in memory at the slot address, and the code starts at `slot + 0x80`.** This was verified with the internal `jal` targets of module 3: 2,261 out of 3,523 land on a prologue with this base, versus 138 without the header. `FUN_001210a0` then reads the header in memory: it clears the BSS (`+0x14`) and calls the constructors (`+0x18`..`+0x1c`).

`MWo3` very likely corresponds to the **Metrowerks CodeWarrior** overlay format: the game would be compiled with CodeWarrior, not with GCC.

The modules are **pieces of SLZ chains**: `FUN_00102ac0` returns the n-th block of a chain. The **slot table** is at `0x1c8a30` in the ELF: 21 addresses, indexed by slot number. It is used by the virtual methods `FUN_00100e70` / `FUN_00100f60` ("loader" class, vtable at `0x1a4a1c`) and by `FUN_00410cc0` (module 3, slots 3 and 4).

The small scene overlays are **`GRP`** entries of **directory containers**. Container format (read by `FUN_00422c80`): `+0x4` = number of entries, then 16-byte entries starting at `+0x10` (tag, parameter, size, offset). Tags seen: `ANIM`, `CHAR`, `SIG`, `ATRC`, `GRP`, `FACE`, `SYS`, `DCMS`, `ECS`, `ESM`, `FISP`, `AAM`, `APAM`, `PAMM`, `MRTA`, `PAM`.

| No. | Slot | Declared (ELF) | Found | Code | Contents |
|---|---|---|---|---|---|
| 1 | `0x1e7d80` | 1 | **1** | 1,200 KB | **module 1: resident core** (loaded with the title; contains libmpeg/libipu, the video decoder) |
| 2 / 6 | `0x35ec80` | 3 | 2 | 1,254 KB | module 2 (title / memory card); **module 3, exploration engine** (doors, cameras, physics) |
| 3 | `0x48ea00` | 10 | **10** | 78 KB | scenes: `SPWorldMap.bin`, `SPStaffRoll.bin`, `SPDragonHall.bin`… |
| 4 | `0x495000` | 12 | **12** | 550 KB | camp: `CampEquip.ovl`, `CampParty.ovl`, `CampSaveLoad.ovl`… |
| 5, 7–20 | `0x4aea80`, `0x36d800`, `0x514100`…`0x547780` | 1 + 1 + 336 | **0** | — | "mode B", see 7.6 |

The sizes found match the segments declared in the ELF **exactly**: decompressed size + BSS = segment size. The modules are duplicated on the disc (131 occurrences for 25 unique ones), probably to reduce read-head seeks.

### 7.4 Code measurements

| | Code | Functions | VU0 macro | MMI | VU1 microcode |
|---|---|---|---|---|---|
| Main ELF | ≈ 270 KB | 972 (Ghidra) | 24 | 110 | 1 program, 1,161 instr. |
| Module 1 (resident core) | 1,200 KB | ≈ 2,000 (estimate) | **9,863** | 1,027 | 3 uploads, 709 instr. |
| Module 3 (exploration) | 1,165 KB | **3,214 (Ghidra)**, 0 decompilation errors | 4,433 | 192 | none |
| Module 2 + scenes + camp | ≈ 720 KB | ≈ 1,300 (estimate) | ≈ 900 | ≈ 40 | none |
| **Total measured** | **≈ 3.3 MB** | **≈ 7,600** | **≈ 15,200** | **≈ 1,400** | **≈ 1,870 instr.** |

The estimates count `addiu sp,sp,-N` prologues × 1.25 (calibrated on the ELF: 780 prologues for 972 Ghidra functions). Module 3 also contains **1,782 indirect calls** (`jalr`): the recompiler will have to dispatch function pointers at run time.

### 7.5 IOP modules

| Sony (standard, replaceable) | tri-Ace (to reimplement) |
|---|---|
| `sio2man`, `sio2d`, `Dbc_Manager`, `ds2o_d` (DualShock 2), `Sound_Device_Library` (libsd), `mcman`, `mcserv` | `Csm_`, `Csd_`, `Csi_` (≈ 50 KB in total): very likely the **sound driver** |

### 7.6 The missing "mode B"

The slot table describes two memory configurations:
- **Mode A (exploration)**: module 3 at `0x35ec80`, plus the scenes (3) and the camp (4). Everything is found.
- **Mode B**: a `0xe580` module at `0x35ec80` (6), then a 1.7 MB module at `0x36d800` (7, right after: `0x35ec80 + 0xeb80`), then 13 small slots (8 to 20), including the parallel groups 4×15, 2×45 and 4×24. Plus slot 5 (`0x4aea80`, 0x100 bytes). **Nothing is found.**

Methods used, all negative:
1. module header at the top level of each chain piece (25,558 chains);
2. raw code without a header (`jal` targets on prologues, at each candidate slot);
3. headers and SLZ blocks nested inside the decompressed data;
4. relaxed headers: mode B address in any of the first 8 words, across the disc's 25,648 SLZ/SLE headers;
5. data overlays (internal pointers into their own range);
6. raw headers in the uncompressed sectors.

In addition, **no extracted code calls these slots** (zero `jal`), and all 128 `SLE` blocks on the disc decrypt with the known key. The only generic loader goes through a vtable, with a number computed at run time.

**Most likely hypothesis**: these are overlay declarations left over in the linker script (development, or an earlier organization of the battle system), and battles are integrated into the 3D engine of modules 1 and 3. **To confirm**: take a PCSX2 save state **during a battle** and read `eeMemory.bin` at addresses `0x35ec80`, `0x36d800` and `0x514100`+. This will show which modules are resident.

### 7.7 Videos

Module 1 contains Sony's **libmpeg/libipu** (`sceMpegGetPicture`, macroblock decoding): the FMVs are **MPEG-2** in a custom container. On PC, a standard library (FFmpeg) will be able to decode them once the container is understood.

### 7.8 Tooling

REA 6.7.0 **does not open** this binary on Windows: its Ghidra connector there only accepts PE and 6502, and IDA, Kuna and Hopper have no MIPS path. The work was done by driving Ghidra headless directly. REA might be usable on Linux (WSL), which remains to be tested.

## 8. Go/no-go decision

**GO for static recompilation**, for these reasons:
- **manageable** code volume (≈ 3.3 MB measured, ≈ 7,600 functions);
- **clean** disassembly and decompilation with a single compiler (CodeWarrior);
- **all the code storage layers are broken** (SLZ, SLE, MWo3);
- **little MMI** (≈ 1,400), **modestly sized VU1 microcode** (≈ 1,870 instructions in total);
- standard Sony SDK and IOP modules, replaceable with HLE.

Conditions and risks to address early:
1. **VU0 in macro mode** (≈ 15,200 instructions, mostly in module 1): the recompiler must translate them (to SSE). This is a central work item, not a detail.
2. **Indirect calls** (≈ 1,800 in module 3, plus the vtables): a dispatch table is needed covering the ELF and the resident module of each slot.
3. **Missing mode B** (section 7.6): to be confirmed by a memory capture during a battle.
4. **Custom sound driver** (`Csm_`/`Csd_`/`Csi_`) to reimplement from its RPC commands.
5. **GS rendering** and unknown **FMV** container.

## 9. Next steps

1. **Memory capture in battle** (PCSX2, save state) to settle the mode B question and check which modules are resident.
2. ~~**Full extractor**~~ — **done**: `tools/extract.py` (see `tools/README.md`). In 30 s, it outputs the 25 recompiler-ready modules (all matching their ELF segment), the IOP modules, the index of the 25,558 chains and, optionally, all the data. The exact layout of `vp2.bin` (hard disk installation) is not reproduced: the port will read the ISO directly.
3. ~~**Choose the recompiler base**~~ — **done**: PS2Recomp, in a fork with per-slot overlay support. See `docs/RECOMPILER.md` (the ELF and module 3 already recompile without errors).
4. **Minimal HLE**: EE kernel, libcdvd redirected to `vp2.bin`, libpad2, and a "software" GS to see an image.
5. **Milestone: the title screen** displayed natively.

## Appendix — environment and reproduction

| Tool | Location |
|---|---|
| Docker image `vp2s-toolchain` (7-Zip, MIPS binutils, Python + pyelftools/capstone) | `toolchain/Dockerfile` |
| Ghidra 12.1.4 | `D:\Tools\ghidra_12.1.4_PUBLIC` (JDK 21 Temurin via `JAVA_HOME_OVERRIDE`) |
| Emotion Engine plugin v2.1.38 | `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions` |
| REA 6.7.0 (MCP) | registered in `~/.claude.json` (backup: `.claude.json.rea.backup`) |
| Ghidra project | `ghidra/VP2S.gpr` |

Scripts (run with `docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain ...`):
- `sh /work/scripts/extract_iso.sh`: lists and extracts the ISO file system into `extracted/`
- `sh /work/scripts/elf_overview.sh`, `sh /work/scripts/elf_strings.sh`: ELF headers and strings
- `python3 -I /work/scripts/scan_blob.py`: entropy map of the hidden archive
- `python3 -I /work/scripts/extents.py`: data and zero areas
- `python3 -I /work/scripts/count_slz.py`: SLZ blocks and FourCCs
- `python3 -I /work/scripts/elf_layout.py`: overlay slots and VU microcode

- `python3 -I /work/scripts/test_slz.py`: decompresses all aligned blocks (SLZ validation)
- `python3 -I /work/scripts/recover_key.py`: recovers and validates the SLE key
- `python3 -I /work/scripts/scan_chunks.py`: inventory of all SLZ/SLE blocks → `work/chunks.tsv`, modules → `work/modules/`
- `python3 -I /work/scripts/scan_inner.py`: full decompression, `MWo3` overlays → `work/overlays/`
- `python3 -I /work/scripts/scan_parts.py`: **reference** — all unique modules, piece by piece → `work/parts/`
- `python3 -I /work/scripts/check_base.py <module>`: checks the load base using `jal` targets
- `python3 -I /work/scripts/scan_grp.py`: directory containers and `GRP` entries
- `scan_raw_overlays.py`, `scan_nested.py`, `peek_all.py`, `scan_data_overlays.py`, `sle_failures.py`, `calls_into_slots.py`: mode B searches (section 7.6)
- `python3 -I /work/scripts/module_stats.py`: code statistics per slot
- `python3 -I /work/scripts/list_irx.py`: IOP modules

Library: `tools/slz.py` (SLZ types 0 to 3, SLE decryption, block chains, key).

**Reference tool: `tools/extract.py`** (see `tools/README.md`). In a single pass, it replaces the exploration scripts above for extraction. `scripts/check_extract.py` cross-checks it against `scan_parts.py`: 25 out of 25 modules identical.

Ghidra scripts (`ghidra_scripts/`, via `analyzeHeadless`): `AuditStats.java`, `FindKeyFunctions.java`, `FindSlzCandidates.java`, `FindScalar.java`, `Decompile.java`.

> Never commit or redistribute the ISO, `extracted/`, or any data derived from the game.
