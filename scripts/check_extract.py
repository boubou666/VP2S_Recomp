"""Cross-check extractor output against the reference scan (work/parts from scan_parts.py)."""
import glob
import hashlib
import json
import os

out = "/work/out"
mods = json.load(open(f"{out}/modules.json"))
ref = {os.path.basename(p).rsplit("_", 1)[1][:10] for p in glob.glob("/work/work/parts/*.bin")}
got = {m["sha1"][:10] for m in mods}
print(f"extractor modules: {len(got)}, reference: {len(ref)}, identical: {len(got & ref)}, "
      f"only extractor: {len(got - ref)}, only reference: {len(ref - got)}")
for m in mods:
    data = open(f"{out}/modules/{m['file']}", "rb").read()
    assert hashlib.sha1(data).hexdigest() == m["sha1"], m["file"]
print("module files match their recorded sha1")
print("sample:", json.dumps({k: mods[0][k] for k in ("file", "id", "load", "slots", "code_start", "text_size",
                                                    "bss_size", "matches_elf_segment")}))
named = [m["file"] for m in mods if m["name"]]
print(f"named overlays: {len(named)}  e.g. {named[:4]}")
print("iop:", sorted(os.listdir(f"{out}/iop")))
print("iso:", sorted(os.listdir(f"{out}/iso")))
man = json.load(open(f"{out}/manifest.json"))
print("manifest:", {k: man[k] for k in ("chains", "parts_by_kind", "modules", "seconds")})
