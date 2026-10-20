"""Inspect a decrypted module image: header words, then disassemble the start of its payload."""
import struct
import subprocess
import sys

path = sys.argv[1]
data = open(path, "rb").read()
words = struct.unpack_from("<16I", data)
print(path, len(data), "bytes")
print("header words:", " ".join(f"{w:08x}" for w in words))
load = words[2]
for hdr in (0x10, 0x20, 0x40, 0x80):
    tmp = "/tmp/m.bin"
    open(tmp, "wb").write(data[hdr:hdr + 0x80])
    out = subprocess.run(["mips-linux-gnu-objdump", "-D", "-b", "binary", "-m", "mips:5900", "-EL",
                          f"--adjust-vma=0x{load:x}", tmp], capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if "\t" in l][:10]
    print(f"--- payload at +0x{hdr:x} as code @0x{load:x}")
    print("\n".join(lines))
