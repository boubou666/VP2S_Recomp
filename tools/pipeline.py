"""Build pipeline: game ISO -> recompiled C++ for the main ELF and every code module.

Stages, in order (each skips the work whose outputs are newer than its inputs):
  extract  tools/extract.py in Docker                          -> build/extract/
  wrap     main ELF copy + tools/wrap_elf.py per module         -> build/elf/
  ghidra   Ghidra headless analysis + PS2Recomp function export -> build/ghidra/export/
  tool     PS2Recomp fork tools (ps2_recomp, ps2_analyzer)      -> external/PS2Recomp/out/linux-recomp/
  sdk      ps2_analyzer SDK signature matching per input        -> build/sdk/<name>.toml
  entries  functions reached only through pointers              -> build/entries/<name>.txt
  recomp   ps2_recomp per input (Ghidra map + SDK bindings);
           modules in overlay module mode                       -> build/recomp/<name>/
  report   per-input and total figures                         -> build/report.json, build/report.md
  port     MSVC build of port/ (runtime + generated code)      -> build/port/vp2s.exe

Usage (Windows, from the project root):
  python tools/pipeline.py [--from STAGE | --only STAGE] [--force] [--inputs main,mod003]

The port stage needs the Visual Studio 2022 C++ build tools (found with vswhere, or VP2S_VS_DIR).
Requires Docker with the vp2s-toolchain image (toolchain/Dockerfile), Ghidra
(GHIDRA_INSTALL_DIR, default D:\\Tools\\ghidra_12.1.4_PUBLIC) and the Emotion Engine extension
zip for that Ghidra version (VP2S_GHIDRA_EE_ZIP, default in D:\\Tools\\downloads).
"""
import argparse
import getpass
import json
import zipfile
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
EXTRACT = BUILD / "extract"
ELF_DIR = BUILD / "elf"
GHIDRA_DIR = BUILD / "ghidra"
EXPORT_DIR = GHIDRA_DIR / "export"
RECOMP_DIR = BUILD / "recomp"
FORK = ROOT / "external" / "PS2Recomp"
TOOL_BUILD = FORK / "out" / "linux-recomp"
TOOL = TOOL_BUILD / "ps2xRecomp" / "ps2_recomp"
ANALYZER = TOOL_BUILD / "ps2xAnalyzer" / "ps2_analyzer"
SDK_DIR = BUILD / "sdk"
ENTRIES_DIR = BUILD / "entries"
# Optional offline cache of CMake dependency sources (<name>-src directories, e.g. copied
# from a previous PS2Recomp build's _deps); dependencies are downloaded when absent.
DEPS_CACHE = BUILD / "deps"
IMAGE = "vp2s-toolchain"
GHIDRA = Path(os.environ.get("GHIDRA_INSTALL_DIR", r"D:\Tools\ghidra_12.1.4_PUBLIC"))
EE_ZIP = Path(os.environ.get("VP2S_GHIDRA_EE_ZIP", r"D:\Tools\downloads\ghidra-emotionengine-reloaded_12.1.4.zip"))
GHIDRA_SETTINGS = GHIDRA_DIR / "settings"
PORT_BUILD = BUILD / "port"
STAGES = ["extract", "wrap", "ghidra", "tool", "sdk", "entries", "recomp", "report", "port"]
# Roles of the modules without an original file name, inferred during the audit (docs/AUDIT.md).
INFERRED_ROLES = {1: "resident core (inferred)", 2: "title / memory card (inferred)", 3: "field engine (inferred)"}


# ---------------------------------------------------------------- helpers

def cpath(path: Path) -> str:
    """Container path of a file under the project root (mounted at /work)."""
    return "/work/" + path.resolve().relative_to(ROOT).as_posix()


def docker(*args: str, log: Path | None = None) -> None:
    cmd = ["docker", "run", "--rm", "-v", f"{ROOT}:/work", IMAGE, *args]
    if log:
        with open(log, "w", encoding="utf-8") as out:
            result = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
    else:
        result = subprocess.run(cmd)
    if result.returncode != 0:
        raise SystemExit(f"docker step failed ({result.returncode}): {' '.join(args[:3])}"
                         + (f" - see {log}" if log else ""))


def mtime(path: Path) -> float:
    return path.stat().st_mtime if path.exists() else 0.0


def stale(outputs: list[Path], inputs: list[Path], force: bool) -> bool:
    if force or not all(p.exists() for p in outputs):
        return True
    return min(map(mtime, outputs)) < max(map(mtime, inputs), default=0.0)


def modules() -> list[dict]:
    return json.loads((EXTRACT / "modules.json").read_text())


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
    docker("python3", "-I", "/work/tools/extract.py", cpath(isos[0]), cpath(EXTRACT))


def stage_wrap(args) -> None:
    ELF_DIR.mkdir(parents=True, exist_ok=True)
    boot = json.loads((EXTRACT / "manifest.json").read_text())["iso"]["boot"]
    main_src, main_elf = EXTRACT / "iso" / boot, ELF_DIR / "main.elf"
    if stale([main_elf], [main_src], args.force):
        shutil.copyfile(main_src, main_elf)

    wrapper = ROOT / "tools" / "wrap_elf.py"
    commands = []
    for module in modules():
        src, dst = EXTRACT / "modules" / module["file"], ELF_DIR / f"{module_name(module)}.elf"
        if stale([dst], [src, wrapper], args.force):
            commands.append(f"python3 -I /work/tools/wrap_elf.py '{cpath(src)}' '{cpath(dst)}' "
                            f"--flags-from '{cpath(main_src)}'")
    if commands:
        docker("sh", "-c", " && ".join(commands), log=BUILD / "wrap.log")
    print(f"  {len(commands)} module(s) wrapped, {len(modules()) - len(commands)} up to date")


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
    if not stale([TOOL, ANALYZER], sources, args.force):
        print("  up to date")
        return
    deps = []
    if DEPS_CACHE.is_dir():
        deps = [f"-DFETCHCONTENT_SOURCE_DIR_{src.name[:-4].upper()}={cpath(src)}"
                for src in DEPS_CACHE.glob("*-src")]
    TOOL_BUILD.mkdir(parents=True, exist_ok=True)
    docker("cmake", "-S", cpath(FORK), "-B", cpath(TOOL_BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_ANALYZER=ON", "-DPS2X_BUILD_TEST=OFF",
           "-DPS2X_BUILD_STUDIO=OFF", *deps, log=TOOL_BUILD / "configure.log")
    docker("cmake", "--build", cpath(TOOL_BUILD), "--target", "ps2_recomp", "ps2_analyzer",
           log=TOOL_BUILD / "build.log")


def stage_sdk(args) -> None:
    """Match Sony SDK functions by signature so they bind to the runtime's HLE handlers."""
    SDK_DIR.mkdir(parents=True, exist_ok=True)
    commands = []
    for name in input_names(args.inputs):
        elf, out = ELF_DIR / f"{name}.elf", SDK_DIR / f"{name}.toml"
        if stale([out], [elf, ANALYZER], args.force):
            commands.append(f"'{cpath(ANALYZER)}' '{cpath(elf)}' '{cpath(out)}' > '{cpath(SDK_DIR / name)}.log' 2>&1")
    if commands:
        docker("sh", "-c", " && ".join(commands), log=SDK_DIR / "sdk.log")
    total = sum(len(sdk_bindings(SDK_DIR / f"{n}.toml")) for n in input_names(args.inputs))
    print(f"  {len(commands)} input(s) analyzed; {total} SDK bindings in total")


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
}


def write_config(name: str) -> Path:
    """Export TOML rewritten with container paths; modules get overlay module mode."""
    text = (EXPORT_DIR / f"{name}.toml").read_text()
    replacements = {
        "input": cpath(ELF_DIR / f"{name}.elf"),
        "output": cpath(RECOMP_DIR / name),
        "ghidra_output": cpath(EXPORT_DIR / f"{name}_functions.csv"),
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
        shutil.rmtree(RECOMP_DIR / name, ignore_errors=True)
        start = time.time()
        docker(cpath(TOOL), cpath(config), log=RECOMP_DIR / f"{name}.log")
        stamp.write_text(f"{time.time() - start:.1f}\n")
        done += 1
        print(f"  {name}: {time.time() - start:.0f}s")
    print(f"  {done} input(s) recompiled")


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
        cpp = list(out.glob("*.cpp"))
        lines = 0
        for path in cpp:
            with open(path, "rb") as f:
                lines += sum(chunk.count(b"\n") for chunk in iter(lambda: f.read(1 << 20), b""))
        info = module_info.get(name, {})
        rows.append({
            "input": name,
            "original": info.get("name") or INFERRED_ROLES.get(info.get("id"), "SLES main ELF" if name == "main" else ""),
            "load": info.get("load", "0x00100000"),
            "functions": int(functions.group(1)) if functions else None,
            "ok": "Recompilation completed successfully" in text,
            "warnings": int(counts.group(1)) if counts else None,
            "errors": int(counts.group(2)) if counts else None,
            "cpp_files": len(cpp),
            "cpp_lines": lines,
            "megabytes": round(sum(p.stat().st_size for p in out.iterdir() if p.is_file()) / 2**20, 1),
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


def stage_port(args) -> None:
    PORT_BUILD.mkdir(parents=True, exist_ok=True)
    if args.force or not (PORT_BUILD / "CMakeCache.txt").exists():
        deps = []
        if DEPS_CACHE.is_dir():
            deps = [f'"-DFETCHCONTENT_SOURCE_DIR_{src.name[:-4].upper()}={src.as_posix()}"'
                    for src in DEPS_CACHE.glob("*-src")]
        print("  configuring (log: build/port/configure.log)")
        run_msvc([f'cmake -S "{(ROOT / "port").as_posix()}" -B "{PORT_BUILD.as_posix()}" -G Ninja '
                  f'-DCMAKE_BUILD_TYPE=Release {" ".join(deps)}'], PORT_BUILD / "configure.log")
    print("  building vp2s.exe (log: build/port/build.log)", flush=True)
    run_msvc([f'cmake --build "{PORT_BUILD.as_posix()}" --target vp2s'], PORT_BUILD / "build.log")
    exe = PORT_BUILD / "vp2s.exe"
    print(f"  {exe.relative_to(ROOT)}: {exe.stat().st_size / 2**20:.1f} MB")


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
