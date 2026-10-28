"""Build pipeline: game ISO -> recompiled C++ for the main ELF and every code module.

Stages, in order (each skips the work whose outputs are newer than its inputs):
  extract  tools/extract.py                                    -> build/extract/
  wrap     main ELF copy + tools/wrap_elf.py per module         -> build/elf/
  ghidra   Ghidra headless analysis + PS2Recomp function export -> build/ghidra/export/
  tool     MSVC build of the fork tools (ps2_recomp, ps2_analyzer, ps2_vu_aot) -> build/tests/
  sdk      ps2_analyzer SDK signature matching per input        -> build/sdk/<name>.toml
  entries  functions reached only through pointers              -> build/entries/<name>.txt
  recomp   ps2_recomp per input (Ghidra map + SDK bindings);
           modules in overlay module mode                       -> build/recomp/<name>/
  vu       ps2_vu_aot per input: VU1 microcode compiled ahead
           of time (inputs without microcode get nothing;
           fast mode unless VP2S_VU_EXACT=1)                    -> build/vu_aot/<name>/
  report   per-input and total figures                         -> build/report.json, build/report.md
  port     MSVC build of port/ (runtime + generated code)      -> build/port/vp2s.exe
  test     MSVC build of the fork test suites, then run them    -> build/tests/<suite>.log

Usage (Windows, from the project root):
  python tools/pipeline.py [--from STAGE | --only STAGE] [--force] [--inputs main,mod003]

Everything runs natively on Windows. Requires the Visual Studio 2022 C++ build tools (found
with vswhere, or VP2S_VS_DIR), Ghidra (GHIDRA_INSTALL_DIR, default D:\\Tools\\ghidra_12.1.4_PUBLIC)
and the Emotion Engine extension zip for that Ghidra version (VP2S_GHIDRA_EE_ZIP, default in
D:\\Tools\\downloads). toolchain/Dockerfile remains for Linux builds of the fork, not needed here.
"""
import argparse
import getpass
import hashlib
import json
import zipfile
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXTRACT = BUILD / "extract"
ELF_DIR = BUILD / "elf"
CAPTURED = BUILD / "captured_modules"
GHIDRA_DIR = BUILD / "ghidra"
EXPORT_DIR = GHIDRA_DIR / "export"
# VP2S_FORK / VP2S_TEST_BUILD / VP2S_RECOMP / VP2S_PORT_BUILD build another fork checkout (e.g. a
# git worktree) into its own build directories, without touching the default ones.
RECOMP_DIR = BUILD / os.environ.get("VP2S_RECOMP", "recomp")
# ps2_recomp writes here first; only changed files are then copied to RECOMP_DIR.
STAGING_DIR = BUILD / f"{RECOMP_DIR.name}-staging"
FORK = Path(os.environ.get("VP2S_FORK", ROOT / "external" / "PS2Recomp"))
# The fork tools and its test suites share one MSVC build of the fork.
TEST_BUILD = BUILD / os.environ.get("VP2S_TEST_BUILD", "tests")
TOOL = TEST_BUILD / "ps2xRecomp" / "ps2_recomp.exe"
ANALYZER = TEST_BUILD / "ps2xAnalyzer" / "ps2_analyzer.exe"
VU_AOT_TOOL = TEST_BUILD / "ps2xRecomp" / "ps2_vu_aot.exe"
VU_AOT_DIR = BUILD / os.environ.get("VP2S_VU_AOT", "vu_aot")
# VU1 microcode on its blocks' static schedule with relaxed floats (ps2_vu_aot, PS2X_VU_AOT_FAST): VU1 about 25%
# faster, the same GS packets on recorded field runs. VP2S_VU_EXACT=1 keeps the exact pipeline model.
VU_AOT_MODE = "exact" if os.environ.get("VP2S_VU_EXACT") else "fast"
SDK_DIR = BUILD / "sdk"
ENTRIES_DIR = BUILD / "entries"
# Optional offline cache of CMake dependency sources (<name>-src directories, e.g. copied
# from a previous PS2Recomp build's _deps); dependencies are downloaded when absent.
DEPS_CACHE = BUILD / "deps"
GHIDRA = Path(os.environ.get("GHIDRA_INSTALL_DIR", r"D:\Tools\ghidra_12.1.4_PUBLIC"))
EE_ZIP = Path(os.environ.get("VP2S_GHIDRA_EE_ZIP", r"D:\Tools\downloads\ghidra-emotionengine-reloaded_12.1.4.zip"))
GHIDRA_SETTINGS = GHIDRA_DIR / "settings"
PORT_BUILD = BUILD / os.environ.get("VP2S_PORT_BUILD", "port")
STAGES = ["extract", "wrap", "ghidra", "tool", "sdk", "entries", "recomp", "vu", "report", "port", "test"]
# Roles of the modules without an original file name, inferred during the audit (docs/AUDIT.md).
INFERRED_ROLES = {1: "resident core (inferred)", 2: "title / memory card (inferred)", 3: "field engine (inferred)"}


# ---------------------------------------------------------------- helpers

def hpath(path: Path) -> str:
    """Absolute path with forward slashes, as written into the recompiler configs."""
    return path.resolve().as_posix()


def run_logged(cmd: list[str], log: Path) -> None:
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        result = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SystemExit(f"{Path(cmd[0]).name} failed ({result.returncode}) - see {log}")


def mtime(path: Path) -> float:
    return path.stat().st_mtime if path.exists() else 0.0


def stale(outputs: list[Path], inputs: list[Path], force: bool) -> bool:
    if force or not all(p.exists() for p in outputs):
        return True
    return min(map(mtime, outputs)) < max(map(mtime, inputs), default=0.0)


def modules() -> list[dict]:
    """The extracted modules, then those captured from memory at run time (build/captured_modules,
    see captured_modules)."""
    found = json.loads((EXTRACT / "modules.json").read_text())
    known = {m["id"] for m in found}
    return found + [m for m in captured_modules() if m["id"] not in known]


def captured_modules() -> list[dict]:
    """Modules the game loads that the extractor never found on the disc (docs/AUDIT.md: slots declared
    but not found, e.g. the tutorial's module 27). The VP2S diagnostics save each unknown module they see in
    a slot as build/port/vp2s_module_<load>_<hash>.bin (header included, extractor format); copied into
    build/captured_modules they are wrapped and recompiled like the others."""
    result = []
    for path in sorted(CAPTURED.glob("*.bin")):
        data = path.read_bytes()
        _, ident, load, text, dsize, bss, ctor_lo, ctor_hi = struct.unpack_from("<8I", data, 0)
        ctors = [struct.unpack_from("<I", data, a - load)[0] for a in range(ctor_lo, ctor_hi, 4)
                 if 0 <= a - load <= len(data) - 4] if ctor_hi - ctor_lo <= 0x1000 else []
        result.append({"file": str(path), "id": ident, "name": None, "format": "module", "load": f"0x{load:08x}",
                       "code_start": f"0x{load + 0x80:08x}", "text_size": text - 0x80, "data_size": dsize,
                       "bss_size": bss, "ctors": [f"0x{c:08x}" for c in ctors], "size": len(data),
                       "sha1": hashlib.sha1(data).hexdigest(), "captured": True})
    return result


def module_name(module: dict) -> str:
    return f"mod{module['id']:03d}"


def input_names(selection: set[str] | None) -> list[str]:
    names = ["main"] + [module_name(m) for m in modules()]
    return [n for n in names if selection is None or n in selection]


# ---------------------------------------------------------------- stages

def stage_extract(args) -> None:
    isos = sorted(ROOT.glob("*.iso"))
    if not isos:
        raise SystemExit("no .iso in the project root")
    inputs = [isos[0], ROOT / "tools" / "extract.py", ROOT / "tools" / "slz.py"]
    if not stale([EXTRACT / "manifest.json"], inputs, args.force):
        print("  up to date")
        return
    run_logged([sys.executable, "-I", str(ROOT / "tools" / "extract.py"), str(isos[0]), str(EXTRACT)],
               BUILD / "extract.log")


def stage_wrap(args) -> None:
    ELF_DIR.mkdir(parents=True, exist_ok=True)
    boot = json.loads((EXTRACT / "manifest.json").read_text())["iso"]["boot"]
    main_src, main_elf = EXTRACT / "iso" / boot, ELF_DIR / "main.elf"
    if stale([main_elf], [main_src], args.force):
        shutil.copyfile(main_src, main_elf)

    from wrap_elf import wrap

    wrapper = ROOT / "tools" / "wrap_elf.py"
    flags = struct.unpack_from("<I", main_src.read_bytes(), 0x24)[0]
    wrapped = 0
    for module in modules():
        src, dst = EXTRACT / "modules" / module["file"], ELF_DIR / f"{module_name(module)}.elf"
        if stale([dst], [src, wrapper], args.force):
            # Rewrite only on change so unchanged modules are not reanalyzed downstream.
            elf = wrap(src.read_bytes(), flags)
            if not dst.exists() or dst.read_bytes() != elf:
                dst.write_bytes(elf)
                wrapped += 1
    print(f"  {wrapped} module(s) wrapped, {len(modules()) - wrapped} up to date")


def ghidra_env() -> dict:
    """Environment giving Ghidra a settings directory under build/ with the EE extension.

    Python started through the Windows app alias (Store / Python install manager) runs with
    %APPDATA% redirected, so a Ghidra child would not see the user's installed extensions and
    would fall back to a generic MIPS64 language. XDG_CONFIG_HOME avoids %APPDATA% entirely.
    """
    if not EE_ZIP.is_file():
        raise SystemExit(f"Emotion Engine extension zip not found: {EE_ZIP} (set VP2S_GHIDRA_EE_ZIP)")
    # Ghidra names the directory "<user>-ghidra" outside the home directory; fill both forms.
    for app in ("ghidra", f"{getpass.getuser()}-ghidra"):
        extensions = GHIDRA_SETTINGS / app / GHIDRA.name / "Extensions"
        if not (extensions / "ghidra-emotionengine-reloaded").is_dir():
            extensions.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(EE_ZIP) as archive:
                archive.extractall(extensions)
    return dict(os.environ, XDG_CONFIG_HOME=str(GHIDRA_SETTINGS))


def stage_ghidra(args) -> None:
    EXPORT_DIR.mkdir(parents=True, exist_ok=True)
    scripts = GHIDRA_DIR / "scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    # analyzeHeadless.bat splits arguments on ';', so both scripts share one directory.
    shutil.copy(ROOT / "ghidra_scripts" / "PipelineExport.java", scripts)
    shutil.copy(FORK / "ps2xRecomp" / "tools" / "ghidra" / "ExportPS2Functions.java", scripts)

    todo = []
    for name in input_names(args.inputs):
        elf = ELF_DIR / f"{name}.elf"
        outs = [EXPORT_DIR / f"{name}.toml", EXPORT_DIR / f"{name}_functions.csv"]
        if stale(outs, [elf], args.force):
            todo.append(elf)
    if not todo:
        print("  up to date")
        return

    project = GHIDRA_DIR / "project"
    project.mkdir(parents=True, exist_ok=True)
    # Force the Emotion Engine language: auto-detection can pick a generic MIPS64 language
    # for the wrapped modules, which silently ruins the analysis.
    cmd = [str(GHIDRA / "support" / "analyzeHeadless.bat"), str(project), "pipeline",
           "-import", *map(str, todo), "-overwrite", "-processor", "r5900:LE:32:default",
           "-scriptPath", str(scripts), "-postScript", "PipelineExport.java", str(EXPORT_DIR)]
    print(f"  analyzing {len(todo)} ELF(s) in one Ghidra session (log: build/ghidra/ghidra.log)")
    env = ghidra_env()
    with open(GHIDRA_DIR / "ghidra.log", "w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    missing = [e.stem for e in todo if not (EXPORT_DIR / f"{e.stem}.toml").exists()]
    if result.returncode != 0 or missing:
        raise SystemExit(f"Ghidra export failed (exit {result.returncode}, missing: {missing}) "
                         "- see build/ghidra/ghidra.log")
    languages = set(re.findall(r"Using Language/Compiler: (\S+)",
                               (GHIDRA_DIR / "ghidra.log").read_text(errors="replace")))
    if languages != {"r5900:LE:32:default:default"}:
        raise SystemExit(f"unexpected Ghidra language(s) {sorted(languages)} - see build/ghidra/ghidra.log")


def stage_tool(args) -> None:
    sources = [p for d in ("ps2xRecomp/src", "ps2xRecomp/include", "ps2xAnalyzer/src", "ps2xAnalyzer/include")
               for p in (FORK / d).rglob("*") if p.is_file()]
    # ps2_vu_aot plans its blocks with the runtime's VU1 headers.
    sources += list((FORK / "ps2xRuntime" / "include" / "runtime").glob("ps2_vu1*.h"))
    if not stale([TOOL, ANALYZER, VU_AOT_TOOL], sources, args.force):
        print("  up to date")
        return
    configure_fork_build(args.force)
    run_msvc([f'cmake --build "{TEST_BUILD.as_posix()}" --target ps2_recomp ps2_analyzer ps2_vu_aot'],
             TEST_BUILD / "tools.log")


def stage_sdk(args) -> None:
    """Match Sony SDK functions by signature so they bind to the runtime's HLE handlers."""
    SDK_DIR.mkdir(parents=True, exist_ok=True)
    analyzed = 0
    for name in input_names(args.inputs):
        elf, out = ELF_DIR / f"{name}.elf", SDK_DIR / f"{name}.toml"
        if stale([out], [elf, ANALYZER], args.force):
            run_logged([str(ANALYZER), str(elf), str(out)], SDK_DIR / f"{name}.log")
            analyzed += 1
    total = sum(len(sdk_bindings(SDK_DIR / f"{n}.toml")) for n in input_names(args.inputs))
    print(f"  {analyzed} input(s) analyzed; {total} SDK bindings in total")


def stage_entries(args) -> None:
    """Functions reached only through pointers become recompiler entry points."""
    from find_entry_points import Image, find_entry_points

    names = input_names(None)  # main is always needed to attribute cross-references
    inputs = [ELF_DIR / f"{n}.elf" for n in names] + [EXPORT_DIR / f"{n}_functions.csv" for n in names]
    outputs = [ENTRIES_DIR / f"{n}.txt" for n in names]
    inputs.append(ROOT / "tools" / "find_entry_points.py")
    if not stale(outputs, inputs, args.force):
        print("  up to date")
        return
    ENTRIES_DIR.mkdir(parents=True, exist_ok=True)
    images = {n: Image(ELF_DIR / f"{n}.elf") for n in names}
    found = find_entry_points(images, {n: EXPORT_DIR / f"{n}_functions.csv" for n in names})
    for name in names:
        # Rewrite only on change so unchanged inputs are not recompiled downstream.
        path, text = ENTRIES_DIR / f"{name}.txt", "".join(f"0x{a:08x}\n" for a in found[name])
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
        else:
            path.touch()
    print(f"  {sum(map(len, found.values()))} entry points ({len(found['main'])} in the main ELF)")


def entry_points(name: str) -> list[str]:
    path = ENTRIES_DIR / f"{name}.txt"
    return path.read_text().split() if path.exists() else []


STUB_ENTRY = re.compile(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"')


def stub_block(text: str) -> re.Match | None:
    return re.search(r"(?ms)^stubs = \[(.*?)^\]", text)


def sdk_bindings(path: Path) -> dict[int, str]:
    """name@address stub bindings of a TOML file, keyed by address."""
    if not path.exists():
        return {}
    block = stub_block(path.read_text())
    if not block:
        return {}
    return {int(addr, 16): name for name, addr in STUB_ENTRY.findall(block.group(1))}


# SDK functions the signature base does not match, identified by hand. The runtime's HLE
# handler named here replaces the recompiled guest code at that address.
MANUAL_BINDINGS: dict[str, dict[int, str]] = {
    "main": {
        # sceSifLoadModuleBuffer(addr, args, argp): calls _sceSifLoadModuleBuffer with a
        # result slot on its stack. Loads the IRX modules copied to IOP memory at boot.
        0x001400F8: "sceSifLoadModuleBuffer",
    },
    "mod001": {
        # libmpeg accessors of mp->sys (pointer at +0x40) next to the matched libmpeg functions:
        # the runtime emulates libmpeg with its own state, so they must use it too.
        0x001F0438: "sceMpegIsEnd",
        0x001F0448: "sceMpegIsRefBuffEmpty",
        0x001F0458: "sceMpegSetDecodeMode",
    },
}


def write_config(name: str) -> Path:
    """Export TOML rewritten with container paths; modules get overlay module mode."""
    text = (EXPORT_DIR / f"{name}.toml").read_text()
    replacements = {
        "input": hpath(ELF_DIR / f"{name}.elf"),
        "output": hpath(STAGING_DIR / name),
        "ghidra_output": hpath(EXPORT_DIR / f"{name}_functions.csv"),
    }
    for key, value in replacements.items():
        text = re.sub(rf'^{key} = .*$', f'{key} = "{value}"', text, count=1, flags=re.M)

    # SDK functions matched by signature win over Ghidra's names at the same address.
    stubs = sdk_bindings(EXPORT_DIR / f"{name}.toml") | sdk_bindings(SDK_DIR / f"{name}.toml")
    stubs |= MANUAL_BINDINGS.get(name, {})
    entries = "".join(f'  "{stubs[a]}@0x{a:08X}",\n' for a in sorted(stubs))
    block = stub_block(text)
    if block:
        text = text[:block.start(1)] + "\n" + entries + text[block.end(1):]
    elif stubs:
        text = text.replace("[general]\n", f"[general]\nstubs = [\n{entries}]\n", 1)

    hints = entry_points(name)
    if hints:
        listed = "".join(f'  "{h}",\n' for h in hints)
        text = text.replace("[general]\n", f"[general]\nentry_points = [\n{listed}]\n", 1)
    if name != "main":
        text = text.replace("[general]\n", f'[general]\nmodule_name = "{name}"\n', 1)
    config = RECOMP_DIR / "config" / f"{name}.toml"
    config.parent.mkdir(parents=True, exist_ok=True)
    # Rewrite only on change: the config's date tells stage_recomp whether to regenerate.
    if not config.exists() or config.read_text() != text:
        config.write_text(text)
    return config


def stage_recomp(args) -> None:
    RECOMP_DIR.mkdir(parents=True, exist_ok=True)
    done = 0
    for name in input_names(args.inputs):
        stamp = RECOMP_DIR / name / ".done"
        config = write_config(name)
        inputs = [config, EXPORT_DIR / f"{name}_functions.csv", TOOL]
        if not stale([stamp], inputs, args.force):
            continue
        staging = STAGING_DIR / name
        shutil.rmtree(staging, ignore_errors=True)
        start = time.time()
        run_logged([str(TOOL), str(config)], RECOMP_DIR / f"{name}.log")
        changed, stats = sync_generated(staging, RECOMP_DIR / name)
        shutil.rmtree(staging, ignore_errors=True)
        (RECOMP_DIR / name / ".stats").write_text(json.dumps(stats))
        stamp.write_text(f"{time.time() - start:.1f}\n")
        done += 1
        print(f"  {name}: {time.time() - start:.0f}s, {changed} file(s) changed")
    print(f"  {done} input(s) recompiled")


def stage_vu(args) -> None:
    """Compile each input's VU1 microcode (VIF MPG uploads in its data) ahead of time.

    The port links the generated fragments; at run time one replaces the VU1 interpreter only
    while VU1 code memory holds its exact bytes, so stray matches in the scan are harmless.
    """
    VU_AOT_DIR.mkdir(parents=True, exist_ok=True)
    for name in input_names(args.inputs):
        out = VU_AOT_DIR / name
        stamp = VU_AOT_DIR / f".{name}.done"  # also for inputs without microcode
        same_mode = stamp.exists() and stamp.read_text().startswith(f"mode={VU_AOT_MODE}\n")
        if same_mode and not stale([stamp], [ELF_DIR / f"{name}.elf", VU_AOT_TOOL], args.force):
            continue
        staging = VU_AOT_DIR / f".{name}-staging"
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir(parents=True)
        env = {key: value for key, value in os.environ.items() if key != "PS2X_VU_AOT_FAST"}
        if VU_AOT_MODE == "fast":
            env["PS2X_VU_AOT_FAST"] = "1"
        result = subprocess.run([str(VU_AOT_TOOL), str(ELF_DIR / f"{name}.elf"), str(staging), f"{name}_"],
                                capture_output=True, text=True, env=env)
        if result.returncode != 0:
            raise SystemExit(f"ps2_vu_aot failed on {name}: {result.stderr.strip()}")
        if any(staging.glob("*.cpp")):
            changed, stats = sync_generated(staging, out)
            print(f"  {name}: {stats['cpp_files']} fragment file(s), {changed} changed")
        else:
            shutil.rmtree(out, ignore_errors=True)
        shutil.rmtree(staging, ignore_errors=True)
        stamp.write_text(f"mode={VU_AOT_MODE}\n" + result.stdout.strip() + "\n")


def sync_generated(source: Path, target: Path) -> tuple[int, dict]:
    """Mirror source into target, touching only files whose content changed.

    Unchanged generated files keep their timestamps, so the MSVC build does not recompile
    them (nor the unity groups that include them). Also returns the report figures, counted
    while the files are in memory anyway.
    """
    target.mkdir(parents=True, exist_ok=True)
    fresh = {p.relative_to(source) for p in source.rglob("*") if p.is_file()}
    changed = 0
    stats = {"cpp_files": 0, "cpp_lines": 0, "bytes": 0}
    for rel in fresh:
        src, dst = source / rel, target / rel
        data = src.read_bytes()
        stats["bytes"] += len(data)
        if rel.suffix == ".cpp":
            stats["cpp_files"] += 1
            stats["cpp_lines"] += data.count(b"\n")
        if not dst.exists() or dst.read_bytes() != data:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            changed += 1
    for old in [p for p in target.rglob("*") if p.is_file()]:
        rel = old.relative_to(target)
        if rel not in fresh and rel.name not in (".done", ".stats"):
            old.unlink()
            changed += 1
    return changed, stats


def stage_report(args) -> None:
    rows = []
    module_info = {module_name(m): m for m in modules()}
    for name in input_names(None):
        log = RECOMP_DIR / f"{name}.log"
        if not log.exists():
            continue
        text = log.read_text(errors="replace")
        counts = re.search(r"Warnings: (\d+), errors: (\d+)", text)
        export = (EXPORT_DIR / f"{name}.toml").read_text()
        functions = re.search(r"function_count = (\d+)", export)
        out = RECOMP_DIR / name
        stats_file = out / ".stats"
        if stats_file.exists():  # written by stage_recomp
            stats = json.loads(stats_file.read_text())
        else:
            cpp = list(out.glob("*.cpp"))
            stats = {"cpp_files": len(cpp), "cpp_lines": sum(p.read_bytes().count(b"\n") for p in cpp),
                     "bytes": sum(p.stat().st_size for p in out.iterdir() if p.is_file())}
        info = module_info.get(name, {})
        rows.append({
            "input": name,
            "original": info.get("name") or INFERRED_ROLES.get(info.get("id"), "SLES main ELF" if name == "main" else ""),
            "load": info.get("load", "0x00100000"),
            "functions": int(functions.group(1)) if functions else None,
            "ok": "Recompilation completed successfully" in text,
            "warnings": int(counts.group(1)) if counts else None,
            "errors": int(counts.group(2)) if counts else None,
            "cpp_files": stats["cpp_files"],
            "cpp_lines": stats["cpp_lines"],
            "megabytes": round(stats["bytes"] / 2**20, 1),
            "seconds": float((out / ".done").read_text()) if (out / ".done").exists() else None,
        })
    totals = {k: sum(r[k] or 0 for r in rows) for k in ("functions", "warnings", "errors", "cpp_files", "cpp_lines", "megabytes")}
    totals["inputs"], totals["ok"] = len(rows), sum(r["ok"] for r in rows)
    (BUILD / "report.json").write_text(json.dumps({"inputs": rows, "totals": totals}, indent=1))

    md = ["# Pipeline report", "",
          "| Input | Original | Load | Functions | OK | Warnings | Errors | C++ files | C++ lines | MB | s |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        md.append(f"| {r['input']} | {r['original']} | {r['load']} | {r['functions']} | {'yes' if r['ok'] else 'NO'} "
                  f"| {r['warnings']} | {r['errors']} | {r['cpp_files']} | {r['cpp_lines']:,} | {r['megabytes']} "
                  f"| {r['seconds'] or ''} |")
    md.append(f"| **total** | {totals['inputs']} inputs, {totals['ok']} ok | | {totals['functions']} | "
              f"| {totals['warnings']} | {totals['errors']} | {totals['cpp_files']} | {totals['cpp_lines']:,} "
              f"| {totals['megabytes']:.1f} | |")
    (BUILD / "report.md").write_text("\n".join(md) + "\n")
    print(f"  {totals['ok']}/{totals['inputs']} inputs recompiled, {totals['functions']} functions, "
          f"{totals['cpp_lines']:,} lines of C++, {totals['errors']} errors -> build/report.md")


def vs_install_dir() -> Path:
    if os.environ.get("VP2S_VS_DIR"):
        return Path(os.environ["VP2S_VS_DIR"])
    vswhere = (Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
               / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
    found = subprocess.run([str(vswhere), "-latest", "-products", "*",
                            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    if not found:
        raise SystemExit("Visual Studio 2022 C++ build tools not found (set VP2S_VS_DIR)")
    return Path(found.splitlines()[0])


def run_msvc(commands: list[str], log: Path) -> None:
    """Run command lines in a cmd session set up by vcvars64, with VS's CMake and Ninja first on PATH."""
    vs = vs_install_dir()
    tools = vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
    script = log.with_suffix(".cmd")
    lines = ["@echo off",
             f'call "{vs}\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1',
             f'set "PATH={tools}\\CMake\\bin;{tools}\\Ninja;%PATH%"']
    lines += [f"{command} || exit /b 1" for command in commands]
    script.write_text("\r\n".join(lines) + "\r\n")
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        result = subprocess.run(["cmd", "/c", str(script)], stdout=out, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise SystemExit(f"MSVC step failed ({result.returncode}) - see {log}")


def deps_args() -> str:
    """FetchContent overrides pointing at the dependency source cache (no network needed)."""
    if not DEPS_CACHE.is_dir():
        return ""
    return " ".join(f'"-DFETCHCONTENT_SOURCE_DIR_{src.name[:-4].upper()}={src.as_posix()}"'
                    for src in DEPS_CACHE.glob("*-src"))


def stage_port(args) -> None:
    PORT_BUILD.mkdir(parents=True, exist_ok=True)
    if args.force or not (PORT_BUILD / "CMakeCache.txt").exists():
        print(f"  configuring (log: {(PORT_BUILD / 'configure.log').relative_to(ROOT)})")
        # Reuse an FFmpeg archive another port build already downloaded.
        archives = sorted(BUILD.glob("*/ThirdParty/ffmpeg-prefix/src/ffmpeg-*.zip"))
        ffmpeg = f'"-DFFMPEG_PREBUILT_URL={archives[0].as_posix()}" ' if archives else ""
        run_msvc([f'cmake -S "{(ROOT / "port").as_posix()}" -B "{PORT_BUILD.as_posix()}" -G Ninja '
                  f'-DCMAKE_BUILD_TYPE=Release "-DVP2S_FORK={FORK.as_posix()}" '
                  f'"-DVP2S_RECOMP_DIR={RECOMP_DIR.as_posix()}" "-DVP2S_VU_AOT_DIR={VU_AOT_DIR.as_posix()}" '
                  f'{ffmpeg}{deps_args()}'], PORT_BUILD / "configure.log")
    print(f"  building vp2s.exe (log: {(PORT_BUILD / 'build.log').relative_to(ROOT)})", flush=True)
    run_msvc([f'cmake --build "{PORT_BUILD.as_posix()}" --target vp2s'], PORT_BUILD / "build.log")
    exe = PORT_BUILD / "vp2s.exe"
    print(f"  {exe.relative_to(ROOT)}: {exe.stat().st_size / 2**20:.1f} MB")


TEST_TARGETS = ["ps2x_tests", "ps2_iop_emulator_tests", "ps2_iop_import_tests",
                "ps2_iop_compatibility_tests", "ps2_iop_import_version_tests", "ps2_iop_sio2_tests",
                "ps2_iop_spu2_tests"]


def configure_fork_build(force: bool) -> None:
    """MSVC build of the whole fork: recompiler tools, runtime and test suites."""
    TEST_BUILD.mkdir(parents=True, exist_ok=True)
    if force or not (TEST_BUILD / "CMakeCache.txt").exists():
        print("  configuring (log: build/tests/configure.log)")
        run_msvc([f'cmake -S "{FORK.as_posix()}" -B "{TEST_BUILD.as_posix()}" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                  "-DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_DEBUG_UI=OFF "
                  "-DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_IOP_BUILD_TESTS=ON "
                  f"{deps_args()}"], TEST_BUILD / "configure.log")


def stage_test(args) -> None:
    """Build the fork's test suites with MSVC and run them."""
    configure_fork_build(args.force)
    print("  building (log: build/tests/build.log)", flush=True)
    # A fork checkout (worktree) may predate some suites: only build the ones it defines.
    cmake_text = "".join(f.read_text(errors="replace") for f in FORK.glob("*/CMakeLists.txt"))
    targets = [t for t in TEST_TARGETS if t in cmake_text]
    run_msvc([f'cmake --build "{TEST_BUILD.as_posix()}" --target {" ".join(targets)}'], TEST_BUILD / "build.log")
    failed = []
    for target in targets:
        exe = next(TEST_BUILD.rglob(f"{target}.exe"))
        log = TEST_BUILD / f"{target}.log"
        with open(log, "w", encoding="utf-8", errors="replace") as out:
            # Some tests read fork sources by relative path: run from the fork root.
            code = subprocess.run([str(exe)], cwd=FORK, stdout=out, stderr=subprocess.STDOUT).returncode
        summary = re.findall(r"^(?:Total Tests|Failed): \d+", log.read_text(errors="replace"), flags=re.M)
        print(f"  {target}: exit {code}" + (f" ({', '.join(summary)})" if summary else ""))
        if code != 0:
            failed.append(target)
    if failed:
        raise SystemExit(f"failing test suites: {', '.join(failed)} - see build/tests/<suite>.log")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--from", dest="start", choices=STAGES, help="run from this stage on")
    group.add_argument("--only", choices=STAGES, help="run a single stage")
    ap.add_argument("--force", action="store_true", help="redo work even when outputs are up to date")
    ap.add_argument("--inputs", help="comma-separated subset for ghidra/recomp, e.g. main,mod003")
    args = ap.parse_args()
    args.inputs = set(args.inputs.split(",")) if args.inputs else None

    stages = [args.only] if args.only else STAGES[STAGES.index(args.start or "extract"):]
    BUILD.mkdir(exist_ok=True)
    for stage in stages:
        start = time.time()
        print(f"[{stage}]", flush=True)
        globals()[f"stage_{stage}"](args)
        print(f"  ({time.time() - start:.0f}s)", flush=True)


if __name__ == "__main__":
    main()
