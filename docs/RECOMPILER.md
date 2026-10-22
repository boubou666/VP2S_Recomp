# Choosing the recompiler base

Decision of 2026-10-20. Context: `docs/AUDIT.md` (GO for static recompilation).

## Recommendation

**Start from PS2Recomp, in a fork, and add slot-based overlay support to it.**

PS2Recomp already recompiles the main ELF and the game's largest overlay without errors. Its runtime covers the kernel, VU0 (macro mode and microprograms), VU1, the GS and execution of the original IOP modules. The only blocking piece missing for us, overlays, fits cleanly on top of its dynamic call mechanism. Writing our own recompiler would mean redoing this runtime, tens of thousands of lines, with no benefit at this stage.

## Our constraints

| Constraint | Source (audit) |
|---|---|
| Binary without symbols, built with **Metrowerks CodeWarrior** | Stripped ELF; `MWo3` overlay format |
| **25 code modules**, several of them at the same address | slot 2/6: modules 2 and 3; slot 3: 10 scenes; slot 4: 12 menus |
| About 3.3 MB of code, about 7,600 functions | section 7.4 |
| **≈ 15,200 VU0 macro instructions**, **195 VU0 microprogram calls** (`VCALLMS`) | mostly in module 1 |
| ≈ 1,870 VU1 microcode instructions | ELF + module 1 |
| ≈ 1,800 indirect calls (`jalr`) in module 3, plus C++ vtables | section 7.4 |
| Custom sound driver on the IOP (`Csm_`, `Csd_`, `Csi_`) | section 7.5 |
| MPEG-2 videos via libmpeg/libipu | section 7.7 |

## PS2Recomp: assessment

Repository: [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp), GPL-3.0, active (last commit on 2026-10-14), about 3,600 stars. It is used by around twenty ports: *God of War*, *Budokai Tenkaichi 3*, *Shadow of the Colossus*, *Def Jam Vendetta*, *Burnout Dominator*…

Components:
- `ps2xRecomp`: MIPS R5900 to C++ translation, instruction by instruction. Its input is a function map exported from Ghidra (`ExportPS2Functions.java`).
- `ps2xRuntime`: memory, function dispatch table, system calls, GS (software rasterizer), VU0/VU1 interpreters.
- `ps2xIOP`: R3000 interpreter that **runs the original IRX modules**, with a virtual IOP kernel and HLE services (libsd, mcserv, dbcman).

### Tests on our game

Run on 2026-10-20: recompiler built under Linux (g++ 13, in the `vp2s-toolchain` image), function maps exported from Ghidra 12.1.4.

| Input | Functions (Ghidra) | Time | Output | Errors | Warnings |
|---|---|---|---|---|---|
| ELF `SLES_546.45` | 972 + 5,592 labels | 14 s | 6,567 files, 772,000 lines of C++ | **0** | 245 (indirect jumps) |
| Module 3 (wrapped as an ELF by `tools/wrap_elf.py`) | 2,559 + 17,124 labels | 43 s | 19,686 files, 3.24 M lines | **0** | 3,428 (indirect jumps) |

The Ghidra export also **automatically identified 55 system calls** (`CreateThread`, `WaitSema`, `FlushCache`…) and linked them to the runtime implementations.

### Fit with our constraints

| Constraint | Verdict | Details |
|---|---|---|
| CodeWarrior, no symbols | ✅ | Translation is done instruction by instruction, independently of the compiler. Jump tables are detected, or can be declared. Tested: 0 errors. |
| **Overlays** | ❌ **to be added** | The runtime has only **one function table indexed by address**, and the generated code defines a global table with a fixed base. Two modules at the same address are impossible as is. See "Overlay support". |
| VU0 macro | ✅ | Translated by `vu_translator.cpp`. A few `CFC2`/`CTC2` control registers remain unhandled: to be checked for the ones the game uses. |
| VU0 microprograms (`VCALLMS`) | ✅ | `VCALLMS` is translated into a call to the runtime's VU0 interpreter (tested in `ps2xTest`). |
| VU1 | ⚠️ | Interpreted, hence slow (the README acknowledges it). Eventually: rewrite our ≈ 1,870 instructions by hand, as a shader or in C++. |
| GS | ⚠️ | **Software** rasterizer, slow. BT3 and *Shadow of the Colossus* wrote their own native OpenGL renderer: this is the main graphics workstream. |
| Indirect calls, vtables | ✅ | `dispatchGuestBranch` and `lookupFunction` resolve the target at run time. Missing re-entry points are added by hand (as for BT3). |
| Custom IOP sound driver | ✅ likely | The original IRX modules run unchanged. To check: `Csd_`'s imports and SPU2 access. |
| MPEG-2 (FMV) | ⚠️ | libipu goes through the IPU hardware: host decoding (FFmpeg) will need to be hooked up. |

### What other ports show

- **Budokai Tenkaichi 3** ([z3xox/BT3-Recomp](https://github.com/z3xox/BT3-Recomp)): the gameplay code is **a raw overlay** loaded at a fixed address. Method: ELF wrapper, separate recompilation, renamed function table (`g_ps2OverlayFunctionTable`), re-entry points added by hand. But there is **only one overlay**, always at the same place, and their game-specific patch file exceeds 7,600 lines.
- **God of War** ([KIexster/god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp), [Waffle565/God-Of-War-Native-Port](https://github.com/Waffle565/God-Of-War-Native-Port)): single ELF, no code overlays (its "overlays" are VU microcode).
- No known PS2Recomp port handles **several overlays at the same address**. On the PS1 side, [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp) compiles overlays ahead of time and picks the native code by checking bytes, which is the principle adopted below.

## Overlay support — implemented

Fork: submodule `external/PS2Recomp`, single branch `main` (shared with the Drakengard port), from upstream commit `eb8688b`. User documentation: "Overlay modules" section of the fork's README.

| Part | Files |
|---|---|
| Recompiler: `module_name`, `module_load`, `module_guard_size` in `[general]`. Names, headers and files prefixed with the module; private table; guard bytes read at the load address. | `config_manager.cpp`, `types.h`, `code_generator.*`, `ps2_recompiler.*`, `function_emitter.cpp`, `function_table_emitter.*` |
| Runtime: `ps2_overlays` registry (registration, guard-based or custom resolver, targeted replacement). `lookupFunction`, `hasFunction` and `replaceFunction` consult the resident module when the ELF table entry is empty. | `ps2_overlay_modules.h/.cpp`, `ps2_runtime.cpp`, `ps2_memory.h` |
| Tests | `ps2xTest/src/ps2_overlay_module_tests.cpp` (6 tests) |
| ELF wrapper: `.modhdr` section for the module header, which serves as the guard | `tools/wrap_elf.py` |

Validation (2026-10-20):
- PS2Recomp test suite under Linux (g++ 13): **451 out of 451**. The 445 original tests still pass (baseline measured on the unmodified upstream commit: 445 out of 445), plus our 6 tests;
- modules 2 and 3, **both at `0x35ec80`**, recompiled in module mode: each one's guard is its header (identifier 2 vs. 3);
- C++ compilation of all of module 2 and of the module 3 functions in the same address range, with both registration files: **2,544 files out of 2,544, 2,542 functions defined, 0 duplicate symbols** (`scripts/check_module_link.py`).

Not yet verified: actual execution, which requires a complete runtime linked with the game (step 4 of the plan).

### Chosen design

What makes the addition simple in our case:
- **each module's header stays in memory at its slot address** (audit, section 7.3). The resident module's identifier can therefore be read from memory (`slot + 4`), without having to intercept the loader;
- every call leaving a module already goes through `dispatchGuestBranch` / `lookupFunction`.

Design:
1. **Per-module recompilation**: `tools/wrap_elf.py` produces one ELF per module (done), then Ghidra exports one map per module.
2. **Per-module prefix in the recompiler**: small change to `ps2xRecomp`. Each output has its own namespace and its own table (`g_mod003_FunctionTable`…), instead of the global table. This avoids BT3's post-processing renaming.
3. **Slot-aware dispatch in the runtime**: `lookupFunction(address)`
   - if the address is in the ELF or in module 1 (resident): direct table;
   - if it falls in an overlay slot: read the identifier at `slot + 4`, then consult that module's table. If the identifier is unknown or inconsistent, explicit error with a trace;
   - optionally, check a fingerprint of the first bytes (psxrecomp-style guard) in debug mode.
4. **`replaceFunction` and per-game patches**: extended to module tables.

Deviation from this plan: the recompiler takes the guard at the load address (the first 64 bytes, i.e. the module header in our case) instead of reading an identifier at a fixed offset. This is more generic, and a game can still install a faster resolver (`ps2_overlays::setResidentResolver`).

## Rejected alternatives

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **Custom recompiler** (N64Recomp-style) | Full control, native overlays, faster code (no resume point at every instruction) | The whole runtime would have to be redone (kernel, VU0/VU1, GS, IOP: tens of thousands of lines) | ❌ to start with. To be reconsidered only if the performance of literal translation becomes a problem. |
| **Matching decompilation** (splat + CodeWarrior PS2 + decomp.me) | Real C source code, clean long-term port | ≈ 7,600 functions to rewrite by hand so that they recompile identically, i.e. years. I have not checked whether mature `m2c`/decomp.me tooling exists for MIPS + CodeWarrior. | ❌ as a base. ✅ later as a **complement**: replace recompiled functions with hand-written C, function by function. |
| **Enhanced emulator** (PCSX2 + patches) | Already works | Not a native port | ❌ out of scope |
| **ps2-web-recomp** (WebAssembly) | Targets the browser | Not our target, less mature | ❌ |

## Points of attention

- **License**: PS2Recomp is **GPL-3.0**. A fork and the port derived from it must be distributed under GPL-3.0, and never with game data.
- **Generated code volume**: about 4 M lines for the ELF and module 3, i.e. **≈ 10 M lines** for the whole game. The first full build will take time: multi-file output, parallel compilation, and possibly one library per module.
- **Performance**: the translation is literal (one resume point per instruction) and the VU and GS are interpreted. The first milestone will be slow, which is expected.
- **Host compiler**: the project is mostly tested with MSVC. The recompiler also builds with g++ 13 (verified). The runtime (raylib, SDL2) remains to be built.

## Proposed plan

1. ~~**Fork PS2Recomp**~~ — done (submodule `external/PS2Recomp`).
2. ~~**Overlays**~~ — done (see "Overlay support — implemented").
3. ~~**Build pipeline**~~ — done for the recompilation part: `tools/pipeline.py` (see `tools/README.md`). Result on 2026-10-20: **26 inputs out of 26** (ELF + 25 modules), **7,191 functions**, **8.69 M lines of C++** (55,492 files, 338 MB), **0 errors**, 6,961 warnings (indirect jumps). About 6 minutes end to end; a rerun with no changes skips everything. Still to do: **compile the runtime and the generated code** and link them into an executable.
4. **First boot** — in progress. Done as of 2026-10-20:
   - **`build/port/vp2s.exe` (168 MB) built with MSVC 14.44** from the fork's runtime and the 26 recompiled inputs (`port/CMakeLists.txt`, `port` step of `tools/pipeline.py`). Unity build in batches of 64 and a precompiled header: about 13 minutes for a full build, 0 compiler errors and 0 warnings;
   - **the game boots**: runtime and window initialized, game patch applied (`port/vp2s_overrides.cpp`: ISO image, memory layout), then the game code runs (C startup, constructors, IOP initialization);
   - **memory layout**: VP2 uses almost all of RAM (heap from `0x549100` to `0x1FF0000`). The fork now allows moving the runtime's reserved area (`PS2Runtime::configureReservedMemory`, 2 tests); VP2 places it in kernel memory `0x80000`–`0x100000`. Without this, `malloc` failed and the game wiped its own memory.

   - **SDK functions linked to the runtime** (`sdk` step): `ps2xAnalyzer` recognizes **177 functions** by signature (160 in the ELF: CD, SIF/RPC, IOP, memory card, DMA, GS, libc; 17 `libmpeg`/`libipu` in module 1). The fork now synthesizes a binding's wrapper even when Ghidra has no function at that address (13 cases in the ELF; 1 test).
   - **Functions reached only through pointers** (`entries` step): 26 entry points added (threads, callbacks), including `0x11d460`.
   - **CD callback**: the fork records `sceCdCallback` and fires it like an interrupt at the end of each `sceCdRead` (1 test). Without this, the game's reader thread waited forever.
   - The game **reads and decrypts its file table** (LBA 1024, see audit 7.2) and starts its threads, including the sound RPC worker (`FUN_001086a0`).

   - **Diagnostics**: `VP2S_DIAG=1` prints the game threads, the sound worker and the IOP side (`[vp2s-iop]`: SIF modules, RPC clients and servers, IOP emulator, latest RPC calls). The runtime's ImGui debugger is compiled in (`VP2S_DEBUG_UI`, on by default) and opens with F1.
   - **Semaphore with `max_count = 0`**: the static constructor of the sound object (`0x151a20`) creates its semaphore with `max_count = 0`. The EE kernel does not validate this field, but the runtime refused the creation: `WaitSema(-1)` returned immediately and the sound worker (priority 42) spun in a loop, starving the main thread (priority 45). The fork now accepts `max_count = 0` as an unbounded semaphore, as Play! does (1 test).
   - **IRX loading**: the game reads 10 modules from the disc, copies them into IOP memory (`sceSifAllocIopHeap` then SIF DMA) and calls `sceSifLoadModuleBuffer` (wrapper at `0x1400f8`, bound by hand in `MANUAL_BINDINGS` of `tools/pipeline.py`, because the signature database does not recognize it). The runtime's IOP emulator only read the image from EE memory. The fork now reads it from IOP memory (`loadModuleBufferFromIop`, 1 test).
   - Result as of 2026-10-20: **all 10 IRX modules are loaded and emulated** (15 IOP threads, 6 RPC servers, about 7 M IOP instructions). The **sound driver answers** `sceSifCallRpc` (SID `0x1234`), and servers `0x80001300` to `0x8000131f` are bound. The game then enters its frame loop (`0x100800`–`0x10085c`: VSync, update of a state object, submission to the GS).

   - **Sound driver and IOP heap**: the sound driver init (`Csi_`, RPC #1) allocates 764 KB of IOP memory. The emulator's heap never reused a freed block and covered only 896 KB, so the init failed. `Csd_` then did not install its SIF command handlers, and the sound bank transfer (boot state 6) waited forever. The fork now allocates like `sysmem` (*first-fit* in a single pool above the modules) and forwards SIF commands from the EE to the IOP (`sceSifSendCmd`, with arguments 5 and 6 in `t0`/`t1` per the EABI). 1 test for each point.
   - **Overlay decryption key**: the `SLE` files are decrypted with a key that the game computes on VU0 (microprogram loaded from `0x145d00`), from 4 words returned by the sound driver (`Csi_` command #14, the complement of constants from its `.data`). The PS2 FMAC addition drops the small operand's bits before adding. An IEEE addition, even rounded toward zero, corrupted the first word of the key, and the title overlay came out as garbage. The fork now models this addition (PCSX2 hard-codes these 4 cases under the name "TriAce hack"). 1 test, which runs the real microprogram.
   - **Code that Ghidra does not see** (`wrap_elf.py`, `find_entry_points.py`, recompiler):
     - CodeWarrior static initializers placed at the end of `.data`, before the header's constructor table (`+0x18`…`+0x1c`): executable `.init` section;
     - functions straddling the declared end of `.text` (modules 2, 16, 19 and 22);
     - functions reached through pointers from one file to another (vtables from the main ELF into the core module), through `jal` from an overlay to the main ELF or to another module, and their calls transitively;
     - functions aligned behind padding, after a `jr ra`, a `j` or a `b`.
     About 4,200 entry points in total.
   - **Native pipeline**: no more Docker. Extraction and ELF wrappers in Python, fork tools and tests built with MSVC (`tool` and `test` steps).
   - **Incremental recompilation**: the `recomp` step only replaces files whose content changes. The unity build is grouped by input and by 8 KB address slice (`UNITY_BUILD_MODE GROUP`) instead of positional batches, which a single added file would all shift. A regeneration touching a few functions rebuilds in 2 to 5 minutes instead of 13.

   Status as of 2026-10-21: the key is correct, and the **core (module 1) and title (module 2) overlays are loaded, resident, with their constructors run**. The game runs in their code without crashing for at least one minute.

   - **The picture displays**: the "black" screen came from my captures (minimized window, or a raylib capture taken before drawing). The game was already displaying, in French, « Impossible de détecter la manette analogique (DUALSHOCK®2) » ("Unable to detect the analog controller").
   - **Controller**: VP2 reads the controller with the real IOP drivers (`sio2man`, `sio2d`, `ds2o_d`, `Dbc_Manager`), which talk to the SIO2 hardware. The fork now emulates the SIO2 controller and a DualShock 2 on port 0 (digital, analog and pressure modes, with the configuration commands), fed by the PC's gamepad or keyboard via `IopHost::readPadState`. It also handles DMA channels 11 and 12 in `dmacman`, the `sceSifSetDmaIntr` call (`sifman` #32, whose callback releases `Dbc_Manager`'s DMA semaphore), and SIF DMA from any IOP RAM area. Finally, EE VBlanks resync the IOP clock; otherwise threads paced by VBlank almost never ran. Test suite `ps2_iop_sio2_tests`.
   - Result as of 2026-10-21: controller detected, « Choisir format écran : □ 16:9 / △ 4:3 » screen ("Choose screen format"), and keyboard input is taken into account. With △, the game moves on.

   - **FFmpeg enabled** (`VP2S_FFMPEG`, ON by default): the fork downloads FFmpeg 7.1 LGPL as DLLs (`System233/ffmpeg-msvc-prebuilt`, 35 MB, under `build/port/ThirdParty`) and copies the DLLs next to `vp2s.exe`. The `libmpeg` accessors adjacent to those recognized by signature (`sceMpegIsEnd`, `sceMpegIsRefBuffEmpty`, `sceMpegSetDecodeMode`) are also linked to the runtime (`MANUAL_BINDINGS`).

   - **Intro video**: the game's player (`FUN_002b94a0`, core module) waits for 512 KB of video audio (`object+0xc999c`, about 2.7 s) before requesting its first frame. The runtime's `libmpeg` emulation stopped consuming the stream 8 frames ahead, so the audio capped at around 290 KB and the streaming CD read waited for room in its ring buffer. The real `libmpeg` buffers the compressed video and can therefore demultiplex seconds ahead. The fork now accepts 128 frames ahead (about 150 MB at 640×448).
   - Result as of 2026-10-21: **the intro video plays**, at 640×448 in letterbox format, with the SQUARE ENIX logo, the tri-Ace logo, then the opening cinematic with French subtitles.

   - **Speed**: the video is paced on VSyncs, and the runtime only emits a VSync once both real time **and** counted EE cycles have elapsed. But several host tasks consumed real time without counting cycles: software GS rasterization, FFmpeg decoding, IOP scheduler called every 32 cycles. The `VP2S_PROFILE` profiler (see `tools/README.md`) guided these fixes in the fork:
     - busy-wait timer reads: one cycle tick is credited;
     - FFmpeg decoding time credited as EE cycles, since it replaces the IPU, which runs in parallel with the EE;
     - IOP run in batches of 256 cycles, instead of one pass at every checkpoint: that was a third of the EE thread's time;
     - GS bilinear filtering in integer arithmetic (CRT rounding calls dominated), with a shortcut when the texel is aligned;
     - GS texture cache over 8 pages instead of one: it copied 8 KB per texel whenever a primitive straddled two pages;
     - **50 Hz in PAL**: the game calls `sceGsResetGraph(0, 1, 3, 1)`, i.e. PAL mode. The runtime paced everything at NTSC 59.94 Hz. VBlanks, the IOP, the video clock and the CD stream rate now follow `EeScheduler::fieldRate()`.
   - Result as of 2026-10-21: video at about 24 frames/s out of 25 (about 48 VSync/s out of 50), i.e. about 96% of real speed, versus about 11 frames/s before. The cinematic lasts more than 4 minutes. **Start** (Enter) skips it.

   - **Faster GS rendering** (branch `vp2s/gs-fast`, merged into `vp2s/overlay-dispatch` at commit 4b3fb3e). The scene following the video draws about 160 textured triangles per VSync via VU1, i.e. about 5.5 million pixels (some twenty screens' worth of translucent layers). The software rasterizer drew it at about 1.3 VSync/s:
     - the GS state is decoded once per primitive, no longer at every pixel; triangles only traverse the useful part of each line;
     - asynchronous drawing (`GSCpuBackend::SetAsyncRasterization`, enabled by the port; `VP2S_GS_SYNC=1` to compare): primitives go into a queue that 8 threads draw in order, each on its own interleaved 4-line bands, so every pixel keeps its draw order;
     - the queue is only drained for VRAM reads outside drawing (transfers to the host, local copies, presentation): render-to-texture goes through a barrier in the queue, image uploads are queued and written in bands, `TEXFLUSH` is a marker, and palette loads go into copies referenced by the primitives;
     - each thread keeps the textures already converted to RGBA (palette and TEXA applied, 32×32 tiles decoded on demand), for primitives larger than 1,024 pixels, refreshed on `TEXFLUSH` like the page cache;
     - VU1 no longer steps through its pipelines on cycles where nothing is due;
     - a test checks that asynchronous drawing leaves exactly the same VRAM as immediate drawing (blending, depth, bilinear, image upload onto a texture being read, render-to-texture).
   - **End of video**: `sceMpegIsEnd` waited for the end of the runtime's CD stream, which VP2 never produces (it reads the video with its own `sceCdRead` calls): the player stayed on a black screen after the last frame. Without a runtime stream, the end of the MPEG stream is now enough.
   - **Sound** (fork commit `e0bc8f0`): SPU2 emulated on the IOP side (two cores, 48 ADPCM voices with ADSR envelopes, AutoDMA, reverb, DMA 4/7); IOP VBlank handlers are delivered (this is how VP2's sound driver wakes its streaming thread); the `sceMpegStrDATA` callbacks receive the video's audio. Output goes through a WSOLA time-stretch (pitch preserved) that absorbs speed variations; `PS2X_SPU2_WAV=<file>` records the output.
   - Result as of 2026-10-21: menu and video at 50 VSync/s (PAL target), with sound. The video plays to the end (5,547 frames) or is skipped with **Start**, then the game moves on to an animated scene (green background, feather, blue shapes) that runs at about 10 to 13 VSync/s. After about 70 s of game time in this scene, the "Press Start" screen has not yet appeared.
   - **Title screen VU1 geometry**: this "animated scene" was in fact the title screen, whose entire VU1 geometry (sky, logo, menu) was wrong. A trace of the VU1 microprograms (incoming VIF data, instructions, XGKICK packets) traced the chain back:
     - program 0x6e0 calls its loader via `IADDIU vi1, vi0, 1301` then `JALR vi15, vi1`. The interpreter applied to `JR`/`JALR` the rule for conditional branches, which read the old value of a VI written by the previous instruction. It therefore jumped to the old value of vi1, and the program went into an endless loop. PCSX2 (microVU) applies this delay only to conditional branches. `JR`/`JALR` now read the new value;
     - the runtime cut off every microprogram at 65,536 cycles. The stray program was therefore abandoned midway, with VU memory half written (output buffer addresses, camera constants), and the next program (0x880) wrote its vertices over the matrices. On the console, `MSCAL`/`MSCNT` waits for the program to finish: the cap is raised to 2^24 cycles, with a warning if a program reaches it;
     - a `FLG=3` GIFtag ("disable") behaves like IMAGE on the GS. XGKICK stopped on it as a reserved instruction, and the GS ignored it without advancing through the packet: both now treat it as IMAGE.
   - Result as of 2026-10-21: **the title screen displays correctly** (sky, valkyrie logo, NOUVELLE PARTIE / CHARGER PARTIE / OPTIONS (New Game / Load Game / Options), copyright), at about 23 to 30 VSync/s out of 50.
   - **NOUVELLE PARTIE** (New Game) starts the opening cinematic (credits over landscapes), at about 50 VSync/s, then the game loads modules 3 and 4 (`field=mod003 scene=mod004`): the screen stayed black. Three causes fixed: (1) the scratchpad DMA channels ignored chain mode (source chain for toSPR, destination chain for fromSPR), and the field module waited forever for `D_STAT` CIS9 then CIS8; (2) the main ELF calls 0x248dc0 in module 1, which Ghidra had merged into the preceding routine: `find_entry_points.py` did not keep `jal` calls from the main ELF into a module, and the main thread stopped; (3) the « CHAPITRE 1 — Offenseurs des dieux » card ("Chapter 1 — Offenders of the Gods") now displays. After that, the field crashed in two ways. (4) Host stack overflow: some recompiled functions have huge MSVC frames (up to about 550 KB, 69 above 32 KB) and the game's calls nest as host calls on the game thread, whose default stack is 1 MB; the port now reserves 256 MB per thread (`/STACK`). (5) Return into data, depending on interrupt timing: the recursive parent/child transform update (`0x39f100` ↔ `0x248f40`, on a stack placed at the top of the scratchpad) was interrupted by a checkpoint on an inner call whose target was also that of an enclosing call; while unwinding, `dispatchGuestBranch` mistook `ctx->pc == entryPc` for a finished HLE stub and let the caller continue with the inner frames' stack. Fixed in the runtime: a transfer-to-dispatcher flag makes the enclosing frames unwind instead. The field scene now plays with its subtitled dialogue. Still open: the field's 3D rendering is wrong (flat green areas, enlarged patterns) and slow (about 25 to 35 VSync/s). A GS dump (`PS2X_GS_DUMP`) shows that the 3D scene itself is rendered correctly into a 512×480 off-screen buffer (FRAME fbp=200, fbw=8), and that the final copy into the CT24 display buffers (fbp=120 and 320, 16 bilinear strips from texture tbp=6400) has the right parameters and writes every pixel (temporary counters: no scissor, alpha or Z rejection). Before that copy the game runs a large post-processing chain on page 200: the scene downsampled to 128 px buffers (pages 400 to 449, partly CT16), blur passes, the Z buffer read as a PSMZ24 texture into page 402 and reread as PSMT8 with a CLUT and REGION_REPEAT to build a depth mask (page 432), a glow, then blended back onto page 200, and finally a full-screen fade (black, alpha 0x80) when the game fades out. The green picture looks like the low-resolution blur buffer covering the whole scene, so the depth mask is the main suspect. Forcing a texture cache invalidation before every textured primitive does not change it. In some runs the screen instead stays black because the game's fade never progresses. Next step: replay a recorded frame offline (GIF stream plus starting VRAM) to check each pass of that chain, instead of 6-minute game runs. `VP2S_HANG_DUMP=<s>` writes the host thread stacks to `vp2s_hang.txt` if the display freezes, and the full stack on overflow.
   - **Opening cinematic audio**: its audio stream is compressed (32-byte header, then high-entropy data), unlike the raw PCM of the intro video. DATA packets carry their channel in the 4th byte of the subheader (`ff 90 00 01`), whereas the runtime took it from the low nibble of the 2nd: everything went to channel 0 (PCM path) and the SPU2 played the compressed bytes, hence the bursts of white noise. The game now receives them on its channel 1 callback, which feeds the `Csd_.irx` decoder on the IOP.
   - **Music** (title screen and cinematic): `Csd_` decodes, on the IOP, coefficients that the EE hands to VU1. A VIF1 DMA task chains three microprograms: dequantization (0x6e0), windowing (0x1410), IMDCT with integer conversion (0x1690). The EE then reads the result back at 0x1100E000–0x1100FFFF (`0x105b00`, `PPACH` to 16 bits) and sends the 0x1040-byte PCM frames back to the IOP, which sends them to the SPU2. The runtime only routed 0x11004000–0x1100BFFF through the slow path. VU1 data memory was therefore read from RAM (0x1100E000 → RAM 0x0100E000), and the music stayed silent. The whole 0x11000000–0x1100FFFF window now goes through the slow path, and the music plays. Dropouts of 35 to 230 ms during the cinematic: the EE scheduler's wall-clock catch-up (inherited from Drakengard) skipped the EE time lost waiting on a saturated GS. The IOP then consumed frames that the EE sound thread had not produced, and `Csd_` stopped then restarted AutoDMA (callback 0x1770). The port calls `setWallClockCatchUp(false)` (`VP2S_WALLCLOCK_CATCHUP=1` to restore the default behavior): no more dropouts, and the game runs at about 90% of real time during the video. `PS2X_PAD_SCRIPT` (runtime) simulates controller presses without a keyboard or focus, for automated tests.
   - **Runtime shared with Drakengard** (2026-10-22): the fork now tracks the shared runtime (bc208c4, branch `main`), which merges the generic work from the Drakengard session. VU interpreter 3 to 4 times faster, cycle-exact on 3,000 VU1 runs recorded on VP2's title screen (`PS2X_VU1_RECORD`, tool `ps2_vu1_replay`). VIF0, SPR and FPU fixes, GS thread (`PS2X_GS_SYNC=1` to disable it), VRAM checking by a second GS (`PS2X_GS_SHADOW_CHECK=1`). The title screen runs in real time, but only shows about 13 distinct frames per second: presentation waits for the GS thread; this will be fixed with the unification of the two GS renderers. `VP2S_RECOMP` (pipeline) builds another fork's recompiled code in its own folder.
   - Along the way: the IOP interpreter did not merge `LWL`/`LWR` with a pending load of the same register (incorrect unaligned reads). Fixed, with a test.

   **Open issues**:
   - **Title screen speed** (about 23 to 30 VSync/s out of 50): a VU1 recompiler and a GPU (or vectorized) GS renderer would be needed to reach full speed; the audio stutters as long as the game runs well below real time.
   - The captures (`vp2s_diag_N.png`) are only 640×256 in this scene: to check whether they really show the whole screen.

   Keyboard (without a controller plugged in), keys identified by their **position on a QWERTY keyboard** (on AZERTY, □ is therefore the W key): arrows = D-pad, WASD = left stick, Z = □, X = ✕, C = ○, V = △, Q/E = L1/R1, 1/3 = L2/R2, left/right Ctrl = L3/R3, Enter = Start, right Shift = Select.
5. **Milestone: the title screen.** Reached on 2026-10-21 (correct rendering, not yet at full speed).
6. Next: module 3 (exploration), native GS renderer, VU1 as a shader, FMV via FFmpeg.

## Reproducing the tests

The fork's test suites (`ps2x_tests`, including the recompiler, and the 4 IOP suites), built with MSVC:

```bash
python tools/pipeline.py --only test
```

Under Linux (Docker, optional), the build requires `-march=x86-64-v2` with g++; the `/deps` mount is optional and reuses already downloaded dependency sources:

```bash
docker run --rm -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp_tests.sh linux-tests
```

Recompiling a module in module mode (4th argument), with the fork's recompiler:

```bash
docker run --rm -e RECOMP=/src/out/linux-tests/ps2xRecomp/ps2_recomp -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_recomp.sh /work/work/recomp/mod003.toml /work/work/recomp/mod003.elf /work/work/recomp/module_mod003 mod003
```

Cross-compiling two modules at the same address:

```bash
docker run --rm -v D:\VP2S_Recomp\external\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain python3 -I /work/scripts/check_module_link.py /work/work/recomp/module_mod002 /work/work/recomp/module_mod003
```

Initial assessment, on the unmodified upstream clone:

```bash
docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/build_ps2recomp.sh
```

```bash
docker run --rm -v D:\VP2S_Recomp:/work vp2s-toolchain python3 -I /work/tools/wrap_elf.py /work/out/modules/0035ec80_003.bin /work/work/recomp/mod003.elf --flags-from /work/out/iso/SLES_546.45
```

Then headless Ghidra with `-postScript ExportPS2Functions.java <toml> <csv>` (PS2Recomp script), and:

```bash
docker run --rm -v D:\Tools\src\PS2Recomp:/src -v D:\VP2S_Recomp:/work vp2s-toolchain sh /work/scripts/run_recomp.sh /work/work/recomp/mod003.toml /work/work/recomp/mod003.elf /work/work/recomp/output_mod003
```

