"""Recover the 16-byte SLE key by frequency analysis, then validate it on the SLE chunks.

Cipher (from FUN_00102360): c = (p - k) ^ key[i % 16], so p = ((c ^ key[j]) + k) is wrong order;
the game *decrypts* with p = (c - k) ^ key[i % 16], k = 3 + 3*i (mod 256).
If plaintext byte p is 0 then key[i % 16] = (c - k) & 0xff, so the most frequent (c - k)
per residue class is the key byte when the plaintext has many zero bytes.
"""
import glob
import sys
from collections import Counter

sys.path.insert(0, "/work/tools")
import slz  # noqa: E402

SECTOR = 2048
ISO = glob.glob("/work/*.iso")[0]
TOC_LBA, TOC_SECTORS = 1024, 18
SLE_LBAS = (255055, 900723)


def guess_key(cipher: bytes, phase: int = 0):
    key = bytearray(16)
    conf = []
    for j in range(16):
        c = Counter()
        for i in range(j, len(cipher), 16):
            k = (3 + 3 * (i + phase)) & 0xFF
            c[(cipher[i] - k) & 0xFF] += 1
        (val, n), = c.most_common(1)
        key[(j + phase) & 0xF] = val
        conf.append(n / max(1, sum(c.values())))
    return bytes(key), conf


with open(ISO, "rb") as f:
    f.seek(TOC_LBA * SECTOR)
    toc = f.read(TOC_SECTORS * SECTOR)
    sle = {}
    for lba in SLE_LBAS:
        f.seek(lba * SECTOR)
        sle[lba] = f.read(4 * 1024 * 1024)

candidates = []
key, conf = guess_key(toc)
print("TOC-derived key:", key.hex(), "min/avg confidence: %.2f / %.2f" % (min(conf), sum(conf) / 16))
candidates.append(("toc", key))
for lba, buf in sle.items():
    comp = slz.parse_header(buf)[2]
    k2, conf2 = guess_key(buf[16:16 + comp])
    print(f"SLE@{lba}-derived key:", k2.hex(), "avg confidence %.2f" % (sum(conf2) / 16))
    candidates.append((f"sle{lba}", k2))

for name, key in candidates:
    for lba, buf in sle.items():
        try:
            parts = slz.decompress_chain(buf, 0, key)
            print(f"[{name}] SLE@{lba}: OK, {len(parts)} chunk(s), {sum(map(len, parts))} bytes, head {parts[0][:16].hex()}")
            with open(f"/work/work/sle_{lba}.bin", "wb") as o:
                o.write(b"".join(parts))
        except Exception as e:
            print(f"[{name}] SLE@{lba}: fail ({str(e)[:60]})")

plain = bytes(((c - ((3 + 3 * i) & 0xFF)) & 0xFF) ^ candidates[0][1][i & 0xF] for i, c in enumerate(toc))
open("/work/work/toc_decrypted.bin", "wb").write(plain)
print("TOC decrypted head:")
for i in range(0, 128, 16):
    print(f"  {i:04x}  " + " ".join(f"{b:02x}" for b in plain[i:i + 16]))
