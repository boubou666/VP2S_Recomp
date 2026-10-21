"""Decompile functions of a pipeline input with the Ghidra project built by tools/pipeline.py.

Usage: python scripts/decompile.py <input> <out file> <addr> [addr...]
       e.g. python scripts/decompile.py main build/decomp/frame_loop.c 0x100800 0x10e7a0
The project is opened read-only: functions created on the fly are not saved.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pipeline  # noqa: E402


def main() -> None:
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    name, out, addrs = sys.argv[1], Path(sys.argv[2]).resolve(), sys.argv[3:]
    scripts = pipeline.GHIDRA_DIR / "scripts"
    scripts.mkdir(parents=True, exist_ok=True)
    shutil.copy(ROOT / "ghidra_scripts" / "Decompile.java", scripts)
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(pipeline.GHIDRA / "support" / "analyzeHeadless.bat"), str(pipeline.GHIDRA_DIR / "project"),
           "pipeline", "-process", f"{name}.elf", "-readOnly", "-noanalysis",
           "-scriptPath", str(scripts), "-postScript", "Decompile.java", str(out), *addrs]
    log = out.with_suffix(".log")
    with open(log, "w", encoding="utf-8", errors="replace") as handle:
        result = subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT, env=pipeline.ghidra_env())
    if result.returncode != 0 or not out.exists():
        raise SystemExit(f"Ghidra failed (exit {result.returncode}) - see {log}")
    print(out)


if __name__ == "__main__":
    main()
