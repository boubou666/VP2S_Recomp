<div align="center">

# VP2S_Recomp

**A native Windows port of *Valkyrie Profile 2: Silmeria* (PAL, `SLES-54645`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Not an emulator: the game's own MIPS R5900 code, rebuilt as a Windows program on a reimplementation of the PS2 hardware.

![Platform](https://img.shields.io/badge/platform-Windows-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-MSVC-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-17a35b)
![Status](https://img.shields.io/badge/status-not%20playable%20yet-d01f3c)

</div>

> [!IMPORTANT]
> No game files are included: no ELF, overlay modules, IOP modules, decryption key, or code generated from them.
> You need **your own copy** of the PAL release, PS2 ID **`SLES-54645`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLES_546.45` | `541fa8d563b2834350fd0a635a4d3631574ad778` |
>
> `tools/extract.py` records the hash it finds in `build/extract/manifest.json` (`boot_sha1`).

## Status

**Work in progress, not playable yet.** The game boots to the title screen with music. **New Game** plays the opening movie, the chapter title card, then the first field cutscene with its subtitled dialogue. The field's 3D rendering is still wrong (flat green areas, magnified patterns), and the field runs at about 25 to 35 VSync/s.

| Milestone | |
|---|:---:|
| Extract, decrypt and decompress the 25 overlay modules from the disc | ✅ |
| Recompile the main ELF and all 25 modules (7,196 functions, about 9 million lines of C++) | ✅ |
| Boot: EE kernel, the game's own IOP modules, CD reads from the disc image | ✅ |
| Overlay decryption key computed by the game on VU0 | ✅ |
| Memory card detection and controller | ✅ |
| Logos and title screen | ✅ |
| Title and in-game music (VU1 synthesis, SPU2 AutoDMA) | ✅ |
| Opening movie (MPEG-2 decoded with FFmpeg) | ✅ |
| New Game: chapter title card, first field cutscene with subtitles | ✅ |
| Field 3D rendering | 🔧 |
| Field at full speed | 🔧 |
| Exploration, battles, menus, saving | ⏳ |
| Widescreen, higher resolution | ⏳ |

✅ works · 🔧 in progress · ⏳ not reached yet

[docs/RECOMPILER.md](docs/RECOMPILER.md) records what works, every fix and how it was found, and what remains open. [docs/AUDIT.md](docs/AUDIT.md) is the initial feasibility audit: disc layout, overlay modules, compression and encryption formats, VU microcode.

## Goal

A fully playable *Valkyrie Profile 2: Silmeria* as a native executable, from the opening movie to the credits. Accuracy first; once it is playable, widescreen and higher internal resolution.

## How it works

```
disc image ──► tools/extract.py ──► main ELF + 25 overlay modules (decrypted, decompressed)
                                           │
               Ghidra (headless, EE) + find_entry_points.py ──► function maps
                                           │
                                     ps2_recomp ──► C++ (one file per function)
                                           │
   PS2Recomp runtime: EE kernel HLE · DMAC · GIF/GS · VIF/VU0/VU1 · IPU ─┼─► vp2s.exe
   PS2Recomp IOP:     R3000A interpreter running the game's own IRX · SPU2 · SIO2
```

- The game loads most of its code at run time as overlay modules, stored encrypted (SLE) and compressed (SLZ) on the disc. `tools/extract.py` unpacks them; `tools/wrap_elf.py` wraps each one in an ELF at its load address.
- Ghidra (with the Emotion Engine extension) maps the functions; `tools/find_entry_points.py` adds those reached only through pointers or from another module.
- PS2Recomp turns every function into C++. Overlay calls are dispatched by slot, so a module's code runs only while it is resident.
- `port/` links the generated code with the PS2Recomp runtime into `vp2s.exe`, and adds the VP2-specific front end and diagnostics.

The PS2Recomp fork lives in `external/PS2Recomp` (git submodule, [boubou666/PS2Recomp](https://github.com/boubou666/PS2Recomp)). Most runtime fixes made for this port land there, for example scratchpad chain DMA, the VU FMAC addition rounding and VU1 data access from the EE.

## Requirements

- Windows 10 or 11, Python 3.10 or later
- Visual Studio 2022 Build Tools ("Desktop development with C++": MSVC, CMake, Ninja), found with `vswhere` or `VP2S_VS_DIR`
- Ghidra (`GHIDRA_INSTALL_DIR`) and the Emotion Engine extension zip for that Ghidra version (`VP2S_GHIDRA_EE_ZIP`)
- Your own disc image of `SLES-54645` (check the hash above)
- The 16-byte SLE key, in `VP2S_SLE_KEY` or `build/sle_key.txt` (32 hex digits). It is not distributed. The game computes it at boot and keeps it at `0x1d5930`; see `load_sle_key` in `tools/slz.py`.

## Build and run

```powershell
git clone --recurse-submodules https://github.com/boubou666/VP2S_Recomp.git
cd VP2S_Recomp
python tools/pipeline.py
```

The pipeline runs these stages: `extract`, `wrap`, `ghidra`, `tool`, `sdk`, `entries`, `recomp`, `report`, `port`, `test`. Each stage skips work whose outputs are up to date. Options: `--only <stage>`, `--from <stage>`, `--force`, `--inputs main,mod003`. The first full build takes a while: the recompiled code is about 360 MB of C++.

Run the game:

```powershell
build\port\vp2s.exe build\extract\iso\SLES_546.45
```

The disc image is found through `VP2S_ISO`, or by searching upward from the ELF's folder.

A controller is used when one is connected; otherwise the keyboard (US key positions):

| PS2 button | Keyboard |
|---|---|
| D-pad | Arrow keys or W A S D |
| Cross / Circle / Square / Triangle | X or Space / C or Esc / Z or Numpad 0 / V or Numpad 1 |
| L1 / R1 / L2 / R2 | Q / E / Left Shift / Right Shift |
| Start / Select | Enter / Tab |

| Key | What it does |
|---|---|
| F1 | Runtime debugger |
| F9 | Screenshot of the game screen (`vp2s_shot_N.png`) |

<details>
<summary><b>Runtime and diagnostic options</b></summary>

| Variable | Effect |
|---|---|
| `VP2S_ISO=<path>` | Disc image to read |
| `VP2S_SLE_KEY=<32 hex digits>` | SLE key for the extractor (or `build/sle_key.txt`) |
| `VP2S_DIAG=1` | Every 5 s: PC, resident modules, guest threads, IOP and GS state, and a screenshot (`vp2s_diag_N.png`) |
| `VP2S_PROFILE=<seconds>` | Sampling profiler: where the busiest host threads spend their time |
| `VP2S_HANG_DUMP=<seconds>` | If the display freezes that long, write every host thread's stack to `vp2s_hang.txt`; also on stack overflow |
| `VP2S_GS_SYNC=1` | Rasterize on the game thread instead of the GS worker threads |
| `VP2S_WALLCLOCK_CATCHUP=1` | Fast-forward emulated time to wall-clock time (off by default: it starves the music thread) |
| `VP2S_DUMP=<address>:<size>`, `VP2S_WATCH=<address>,…` | Dump a memory range, or print memory words on every diagnostic pass |
| `PS2X_PAD_SCRIPT=t:BUTTON[/ms],…` | Scripted button presses (seconds from start), for unattended runs |
| `PS2X_GS_DUMP=<dir>`, `PS2X_GS_DUMP_AFTER=<s>` | Log the next 4000 GS primitives with their state, textures and VRAM |
| `PS2X_VU1_RECORD=<file>` | Record VU1 microprogram runs for the `ps2_vu1_replay` tool |

[tools/README.md](tools/README.md) documents every pipeline stage and option.

</details>

## Known issues

- **Field 3D rendering is wrong:** flat green areas and magnified patterns instead of the scene; the subtitles and 2D overlays draw correctly.
- **The field is slow:** about 25 to 35 VSync/s instead of 50. VU1 runs in an interpreter, and the GS is a software rasterizer.
- **Not reached yet:** everything after the first field cutscene.

## Repository layout

| Path | Contents |
|---|---|
| `tools/` | Pipeline, disc extractor, ELF wrapper, SLZ/SLE library, entry-point finder |
| `port/` | Native front end: entry point, game-specific overrides, diagnostics, profiler |
| `ghidra_scripts/` | Headless Ghidra scripts (export for PS2Recomp, audits) |
| `scripts/` | One-off analysis and check scripts |
| `docs/` | Feasibility audit and recompilation log |
| `external/PS2Recomp` | PS2Recomp fork (submodule) |

Everything derived from the game (`build/`, `extracted/`, `out/`, `work/`, `ghidra/`, disc images) is git-ignored. Never commit or redistribute it.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL), the recompiler and runtime this port is built on.
- The Drakengard recompilation, developed alongside this port on the same PS2Recomp fork; many runtime fixes (GS, VU1, scheduler) are shared.
- *Valkyrie Profile 2: Silmeria* © tri-Ace / Square Enix. This is an unofficial fan project, not affiliated with or endorsed by them.
