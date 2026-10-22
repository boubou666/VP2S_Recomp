# VP2S_Recomp

A native Windows port of **Valkyrie Profile 2: Silmeria** (PAL, SLES-54645), made by statically recompiling the game's PlayStation 2 MIPS code to C++ with a fork of [PS2Recomp](https://github.com/ran-j/PS2Recomp).

> This repository contains no game code, data or keys. You need your own copy of the game.

## Status

Work in progress, not playable yet.

- Boots through the logos to the title screen, with music.
- **New Game** plays the opening movie, then the chapter title card, then the first field cutscene with its subtitles.
- Open: the field's 3D rendering is wrong (flat green areas, magnified patterns), and the field runs at about 25 to 35 VSync/s.

[docs/RECOMPILER.md](docs/RECOMPILER.md) tracks what works, what was fixed and how, and what remains open. [docs/AUDIT.md](docs/AUDIT.md) is the initial feasibility audit: disc layout, overlay modules, compression and encryption formats.

## How it works

1. `tools/extract.py` reads the disc image, then decrypts and decompresses the 25 overlay code modules the game loads at run time.
2. Ghidra (headless, with the Emotion Engine extension) analyzes the main ELF and each module; `tools/find_entry_points.py` adds the functions it misses.
3. PS2Recomp turns every function into C++.
4. `port/` links the generated code with the PS2Recomp runtime (EE kernel, interpreted IOP modules, SPU2, GS, VU0/VU1, DMA) into `vp2s.exe`.

The PS2Recomp fork lives in `external/PS2Recomp` (git submodule, [boubou666/PS2Recomp](https://github.com/boubou666/PS2Recomp)). Most runtime fixes made for this port land there.

## Requirements

- Windows, Python 3.10 or later
- Visual Studio 2022 Build Tools (MSVC, CMake, Ninja); found with `vswhere` or `VP2S_VS_DIR`
- Ghidra (`GHIDRA_INSTALL_DIR`), plus the Emotion Engine extension zip for that Ghidra version (`VP2S_GHIDRA_EE_ZIP`)
- A disc image of the PAL release (SLES-54645), dumped from your own copy
- The 16-byte SLE key, in `VP2S_SLE_KEY` or `build/sle_key.txt` (32 hex digits). It is not distributed. The game computes it at boot and keeps it at `0x1d5930`; see `load_sle_key` in `tools/slz.py`.

## Build and run

```bash
git clone --recurse-submodules https://github.com/boubou666/VP2S_Recomp.git
cd VP2S_Recomp
python tools/pipeline.py
```

The pipeline runs these stages: `extract`, `wrap`, `ghidra`, `tool`, `sdk`, `entries`, `recomp`, `report`, `port`, `test`. Each stage skips work whose outputs are up to date. Options: `--only <stage>`, `--from <stage>`, `--force`, `--inputs main,mod003`.

Run the game:

```bash
build\port\vp2s.exe build\extract\iso\SLES_546.45
```

The disc image is found through `VP2S_ISO`, or by searching upward from the ELF's folder. [tools/README.md](tools/README.md) documents every stage and the diagnostics: `VP2S_DIAG`, `VP2S_PROFILE`, `VP2S_HANG_DUMP`, F9 screenshots, and the F1 runtime debugger.

## Repository layout

| Path | Contents |
|---|---|
| `tools/` | Pipeline, disc extractor, ELF wrapper, SLZ/SLE library, entry-point finder |
| `port/` | The native front end: entry point, game-specific overrides, diagnostics |
| `ghidra_scripts/` | Headless Ghidra scripts (export for PS2Recomp, audits) |
| `scripts/` | One-off analysis and check scripts |
| `docs/` | Feasibility audit and recompilation log |
| `external/PS2Recomp` | PS2Recomp fork (submodule) |

Everything derived from the game (`build/`, `extracted/`, `out/`, `work/`, `ghidra/`, disc images) is git-ignored. Never commit or redistribute it.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPL), the recompiler and runtime this port is built on.
- Valkyrie Profile 2: Silmeria © tri-Ace / Square Enix. This is an unofficial fan project, not affiliated with or endorsed by them.
