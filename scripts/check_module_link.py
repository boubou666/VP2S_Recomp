"""Compile two overlay modules that share a load address and check they can live in one program.

Compiles every source of module A, both registration files, and the sources of module B
whose functions overlap module A's address range, then reports duplicate global symbols.
Usage: check_module_link.py <module A output dir> <module B output dir>
"""
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

RUNTIME = "/src/ps2xRuntime"
INCLUDES = [f"{RUNTIME}/include", f"{RUNTIME}/src/lib/Kernel", "/src/ps2xIOP/include"]
FLAGS = ["-std=c++20", "-O0", "-march=x86-64-v2", "-c", "-w"]

dir_a, dir_b = sys.argv[1], sys.argv[2]
addr_re = re.compile(r"_0x([0-9a-f]+)\.cpp$")


def addr(path):
    m = addr_re.search(path)
    return int(m.group(1), 16) if m else None


sources_a = sorted(glob.glob(f"{dir_a}/*.cpp"))
addrs_a = [a for a in map(addr, sources_a) if a is not None]
lo, hi = min(addrs_a), max(addrs_a)
sources_b = [p for p in glob.glob(f"{dir_b}/*.cpp")
             if "register_functions" in p or (addr(p) is not None and lo <= addr(p) <= hi)]

objdir = "/tmp/modlink"
os.makedirs(objdir, exist_ok=True)


def compile_one(job):
    src, incdir = job
    obj = os.path.join(objdir, os.path.basename(incdir) + "__" + os.path.basename(src) + ".o")
    r = subprocess.run(["g++", *FLAGS, *[f"-I{i}" for i in INCLUDES], f"-I{incdir}", src, "-o", obj],
                       capture_output=True, text=True)
    return obj, r.returncode, r.stderr[-600:]


jobs = [(s, dir_a) for s in sources_a] + [(s, dir_b) for s in sources_b]
print(f"compiling {len(sources_a)} sources of {os.path.basename(dir_a)} + "
      f"{len(sources_b)} of {os.path.basename(dir_b)} (range 0x{lo:x}-0x{hi:x})", flush=True)
with ThreadPoolExecutor(os.cpu_count()) as pool:
    results = list(pool.map(compile_one, jobs))
failed = [r for r in results if r[1] != 0]
print(f"compiled: {len(results) - len(failed)} ok, {len(failed)} failed")
for obj, _, err in failed[:3]:
    print("---", obj, "\n", err)

defined = {}
for obj, rc, _ in results:
    if rc != 0:
        continue
    out = subprocess.run(["nm", "-g", "--defined-only", obj], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TDBR":
            defined.setdefault(parts[2], []).append(os.path.basename(obj))
dups = {s: o for s, o in defined.items() if len(o) > 1}
print(f"global symbols defined: {len(defined)}, defined more than once: {len(dups)}")
for s, o in list(dups.items())[:10]:
    print("  ", s, o)
prefix_a, prefix_b = (os.path.basename(d).replace("module_", "") + "_" for d in (dir_a, dir_b))
# nm prints mangled C++ names, so match the module prefix anywhere in the symbol
print(f"{sum(prefix_a in n for n in defined)} {prefix_a}* symbols alongside "
      f"{sum(prefix_b in n for n in defined)} {prefix_b}* symbols in the same address range")
