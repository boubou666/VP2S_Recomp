# Tools

Workshop tools, run once on the development machine. They are not part of the port: the port will be native code produced by static recompilation of the game's MIPS code. `extract.py` supplies the recompiler with the modules' code, which is encrypted and compressed on the disc.

## `pipeline.py` — from the ISO to recompiled C++

Orchestrator, to be run on Windows from the project root (Python 3.10 or later, no dependencies). Everything runs natively, without Docker:

```bash
python tools/pipeline.py
```

| Stage | Role | Output (`build/`) |
|---|---|---|
| `extract` | `extract.py` | `extract/` |
| `wrap` | copies the main ELF, runs `wrap_elf.py` on each module; the static initializers (end of `.data`, before the header's constructor table) become an executable `.init` section | `elf/main.elf`, `elf/modNNN.elf` |
| `ghidra` | headless Ghidra analysis (`r5900` processor forced) and PS2Recomp export, in a single session | `ghidra/export/<name>.toml`, `<name>_functions.csv` |
| `tool` | builds `ps2_recomp` and `ps2_analyzer` from the fork (`external/PS2Recomp`) with MSVC | `tests/` |
| `sdk` | `ps2_analyzer` identifies Sony SDK functions by signature, to bind them to the runtime's C++ implementations | `sdk/<name>.toml` |
| `entries` | `find_entry_points.py`: functions Ghidra does not see (reached through a pointer, including from another file, such as a vtable in the main ELF pointing into the core module; module constructors; main ELF functions called only from an overlay); they become recompiler `entry_points` | `entries/<name>.txt` |
| `recomp` | `ps2_recomp` on each input, with the Ghidra map and the SDK bindings merged (at the same address, the SDK name wins); modules are built in module mode (`module_name = "modNNN"`). Output goes through `recomp-staging/` and only modified files are copied over, so that MSVC only rebuilds what changed | `recomp/<name>/`, `recomp/<name>.log` |
| `report` | per-input figures and totals | `report.md`, `report.json` |
| `port` | builds `port/` (fork runtime + generated code) with MSVC (VS 2022 Build Tools, located via `vswhere` or `VP2S_VS_DIR`) | `port/vp2s.exe` |
| `test` | builds and runs the fork's test suites with MSVC (`ps2x_tests` and the 4 IOP suites) | `tests/<suite>.log` |

Running the game: `build\port\vp2s.exe build\extract\iso\SLES_546.45`. The ISO image is looked up via `VP2S_ISO`, otherwise by walking up from the ELF's folder.

Diagnostics and runtime options:

- `VP2S_DIAG=1` prints every 5 seconds the PC, the resident modules per slot, the game threads (state, wait, PC), the state of the sound worker and, with the `[vp2s-iop]` prefix, the IOP side: modules loaded via SIF, RPC clients and servers, state of the IOP emulator and its threads, latest RPC calls; with the `[vp2s-gs]` prefix, the GS side: display registers, presented buffer, non-black pixels per buffer, recent GS activity. It also saves a screenshot of the game screen (`vp2s_diag_N.png`), taken directly from the image produced by the runtime's GS, even when the window is minimized.
- **F9** saves a screenshot at any time (`vp2s_shot_N.png`).
- `VP2S_DUMP=<address>:<size>` writes that memory range to `vp2s_dump.bin`, and `VP2S_WATCH=<address>,<address>…` prints those memory words on each pass.
- Video decoding goes through FFmpeg (`-DVP2S_FFMPEG=OFF` to remove it): on the first build, the fork downloads FFmpeg 7.1 LGPL as DLLs (35 MB) into `build/port/ThirdParty`.
- `VP2S_PROFILE=<seconds>` samples the game threads about 1,000 times per second and prints at that interval (`[vp2s-prof]` prefix) the functions where the busiest thread spends its time, exclusive and inclusive. Names come from the public symbols of `vp2s.pdb`, produced at link time without recompiling. Static functions are attributed to the preceding public symbol, hence a few misleading names (`std::invalid_argument`…). Sampling slows the game down: do not measure speed at the same time. Other threads busy more than 30% of the time (the GS drawing threads) are also listed, with their main functions.
- The port enables the fork's asynchronous GS drawing (`GSCpuBackend::SetAsyncRasterization`): primitives are drawn by dedicated threads, which makes direct VRAM reads (the non-black pixel diagnostic) approximate while a draw is in progress. `VP2S_GS_SYNC=1` reverts to immediate drawing on the game thread, for comparison.
- The runtime debugger (ImGui) opens with **F1**; it is built by default (`-DVP2S_DEBUG_UI=OFF` to remove it).

SDK functions that the signature database does not recognize are bound manually in `MANUAL_BINDINGS` (`tools/pipeline.py`), for example the `sceSifLoadModuleBuffer` wrapper at `0x1400f8`. The `recomp` stage regenerates an input as soon as its configuration changes.

Each stage skips work whose outputs are newer than its inputs. Options: `--only <stage>`, `--from <stage>`, `--force`, `--inputs main,mod003`.

Prerequisites: Visual Studio 2022 Build Tools, Ghidra (`GHIDRA_INSTALL_DIR`), and the Emotion Engine extension **zip** for that Ghidra version (`VP2S_GHIDRA_EE_ZIP`, by default in `D:\Tools\downloads`). The optional `build/deps` folder holds the sources of the CMake dependencies (`<name>-src`), for building without network access.

> Why the extension zip: when launched through the Windows `python` alias (Store or Python install manager), Python and its child processes see a redirected `%APPDATA%`. Ghidra does not find the installed extensions there and **silently** falls back to a generic MIPS64 language. The orchestrator therefore gives Ghidra its own settings folder (`XDG_CONFIG_HOME` → `build/ghidra/settings`), installs the extension there, and refuses to continue if any language other than `r5900` appears in the log.

To build another state of the fork (a git worktree, for example) without touching the default folders, `pipeline.py` reads `VP2S_FORK` (fork folder), `VP2S_TEST_BUILD` and `VP2S_PORT_BUILD` (names of the build folders under `build/`):

```bash
VP2S_FORK=D:/VP2S_Recomp/build/wt/gs-fast VP2S_PORT_BUILD=port-gs python tools/pipeline.py --only port
```

An FFmpeg archive already downloaded by another build of the port is reused. The `test` stage only builds the suites defined by the selected fork.

## `extract.py` — disc extractor

Python 3.10 or later, no dependencies (no 7-Zip, no third-party packages).

```bash
python -I tools/extract.py "Valkyrie Profile 2 - Silmeria (France).iso" out
```

Add `--assets` to also decompress all the data (≈ 2.7 GiB, about 1 minute). `--jobs N` limits the number of processes.

Contents of `out/`:

| Path | Contents |
|---|---|
| `iso/` | ISO9660 files: `SLES_546.45`, `IOPRP300.IMG`, `SYSTEM.CNF` |
| `modules/` | the 25 code modules, decrypted and decompressed, **as laid out in memory**: the 0x80-byte header is at the load address, the code at `+0x80` |
| `modules.json` | for each module: identifier, original name, address, slots, sizes (code, data, BSS), constructors, hash, occurrences on the disc, and consistency with the segment declared in the ELF |
| `iop/` | the 10 IOP modules (`sio2man`, `mcman`, `Csm_`/`Csd_`/`Csi_`…) |
| `chunks.tsv` | index of the 25,558 SLZ/SLE chains: offset, LBA, number of chunks, type of each chunk |
| `assets/` | (`--assets`) each decompressed chunk, `<offset>_<chunk>.<type>` |
| `manifest.json` | summary, errors, and slots found / declared |

Expected result on SLES-54645: 25,642 headers, 25,558 chains, 0 errors, 25 modules (all matching their ELF segment), 10 IOP modules.

## `wrap_elf.py` — module → ELF

Wraps an extracted module in a minimal R5900 ELF, loaded at its address, with the module header in memory (`.modhdr` section, which acts as the guard for PS2Recomp's module mode), followed by `.text`, `.data` and `.bss` as described by the header.

## `slz.py` — SLZ compression / SLE encryption

Library used by the extractor: SLZ types 0 to 3, SLE decryption (the key is not provided: `VP2S_SLE_KEY` variable or `build/sle_key.txt` file, 32 hexadecimal digits; see `load_sle_key`), reading of block chains. The formats are described in `docs/AUDIT.md`, sections 7.1 to 7.3.

> `out/`, `work/` and `extracted/` contain game data: never version or redistribute them.
