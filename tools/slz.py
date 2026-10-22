"""tri-Ace SLZ / SLE decompressor (Valkyrie Profile 2, SLES-54645).

Reimplemented from the game's own routines:
  FUN_001025d0  header check, SLE decryption, type dispatch, chunk chaining
  FUN_00101ea0  type 1: byte LZSS, ends on distance 0
  FUN_001018d0  type 2: byte LZSS + RLE, ends at decompressed size
  FUN_00101370  type 3: 16-bit word LZSS, ends on distance 0
  type 0: stored

Chunk header (16 bytes, little endian):
  0x0 magic 'SLZ' (or 'SLE' = encrypted payload)   0x3 type (u8)
  0x4 compressed size   0x8 decompressed size   0xC offset to next chunk (0 = last)
"""
import os
import pathlib
import struct

HEADER = struct.Struct("<3sBIII")

KEY_FILE = pathlib.Path(__file__).resolve().parent.parent / "build" / "sle_key.txt"


def load_sle_key() -> bytes | None:
    """The 16-byte SLE key is not distributed: VP2S_SLE_KEY or build/sle_key.txt (32 hex digits).

    The game computes it at boot on VU0 and keeps it at 0x1d5930 (docs/RECOMPILER.md); it can
    also be recovered by frequency analysis of the two SLE chunks (LBA 255055, 900723), checked
    by exact-size decompression of both.
    """
    text = os.environ.get("VP2S_SLE_KEY") or (KEY_FILE.read_text() if KEY_FILE.is_file() else "")
    text = text.strip()
    return bytes.fromhex(text) if text else None


SLE_KEY = load_sle_key()


class SlzError(Exception):
    pass


def decrypt_sle(payload: bytes, key: bytes) -> bytes:
    """SLE payload: b = (b - k) ^ key[i % 16], k starts at 3 and grows by 3 per byte."""
    out = bytearray(payload)
    k = 3
    for i in range(len(out)):
        out[i] = ((out[i] - k) & 0xFF) ^ key[i & 0xF]
        k = (k + 3) & 0xFF
    return bytes(out)


def _type1(src: bytes, limit: int | None = None) -> bytes:
    out = bytearray()
    i = 0
    bits = 0
    left = 0
    while limit is None or len(out) < limit:
        if left == 0:
            bits = src[i]
            i += 1
            left = 8
        flag = bits & 1
        bits >>= 1
        left -= 1
        if flag:
            out.append(src[i])
            i += 1
            continue
        dist = src[i] | (src[i + 1] & 0xF) << 8
        if dist == 0:
            return bytes(out)
        n = (src[i + 1] >> 4) + 3
        i += 2
        for _ in range(n):
            out.append(out[-dist])
    return bytes(out)


def _type2(src: bytes, size: int) -> bytes:
    out = bytearray()
    i = 0
    bits = 0
    left = 0
    while len(out) < size:
        if left == 0:
            bits = src[i]
            i += 1
            left = 8
        flag = bits & 1
        bits >>= 1
        left -= 1
        if flag:
            out.append(src[i])
            i += 1
            continue
        b0, b1 = src[i], src[i + 1]
        dist = b0 | (b1 & 0xF) << 8
        n = b1 >> 4
        if n < 0xF:
            for _ in range(n + 3):
                out.append(out[-dist])
        else:  # RLE
            if dist < 0x100:
                value = src[i + 2]
                count = dist + 0x13
                i += 1
            else:
                value = b0
                count = (dist >> 8) + 3
            out.extend(bytes([value]) * count)
        i += 2
    return bytes(out[:size])


def _type3(src: bytes, limit: int | None = None) -> bytes:
    words = struct.unpack_from(f"<{len(src) // 2}H", src)
    out = []
    i = 0
    bits = 0
    left = 0
    while limit is None or len(out) * 2 < limit:
        if left == 0:
            bits = words[i]
            i += 1
            left = 16
        flag = bits & 1
        bits >>= 1
        left -= 1
        if flag:
            out.append(words[i])
            i += 1
            continue
        w = words[i]
        dist = w & 0xFFF
        if dist == 0:
            return struct.pack(f"<{len(out)}H", *out)
        for _ in range((w >> 12) + 2):
            out.append(out[-dist])
        i += 1
    return struct.pack(f"<{len(out)}H", *out)


def parse_header(buf: bytes, off: int = 0):
    magic, typ, comp, decomp, nxt = HEADER.unpack_from(buf, off)
    if magic not in (b"SLZ", b"SLE"):
        raise SlzError(f"bad magic {magic!r} at 0x{off:x}")
    return magic, typ, comp, decomp, nxt


def decompress_chunk(buf: bytes, off: int = 0, key: bytes | None = None, peek: int | None = None) -> bytes:
    """Decompress one chunk; with peek, stop after about that many output bytes (no size check)."""
    magic, typ, comp, decomp, _ = parse_header(buf, off)
    payload = buf[off + 16:off + 16 + comp]
    if magic == b"SLE":
        if key is None:
            raise SlzError("SLE chunk needs the 16-byte key: set VP2S_SLE_KEY or write build/sle_key.txt")
        payload = decrypt_sle(payload, key)
    if typ == 0:
        data = payload[:decomp]
    elif typ == 1:
        data = _type1(payload, peek)
    elif typ == 2:
        data = _type2(payload, min(decomp, peek) if peek else decomp)
    else:
        data = _type3(payload, peek)
    if peek:
        return data[:peek]
    if len(data) != decomp:
        raise SlzError(f"type {typ}: got {len(data)} bytes, header says {decomp}")
    return data


def read_chain(f, off: int) -> bytes:
    """Read a whole chunk chain from a file, following each header's 'next' field."""
    f.seek(off)
    buf = bytearray(f.read(16))
    pos = 0
    while True:
        _, _, comp, _, nxt = parse_header(buf, pos)
        end = pos + 16 + comp
        if nxt:
            end = max(end, pos + nxt + 16)
        if len(buf) < end:
            buf += f.read(end - len(buf))
        if not nxt:
            return bytes(buf[:pos + 16 + comp])
        pos += nxt


def decompress_chain(buf: bytes, off: int = 0, key: bytes | None = None) -> list[bytes]:
    """Decompress every chunk of a chain linked by the 'next' field."""
    parts = []
    while True:
        parts.append(decompress_chunk(buf, off, key))
        nxt = parse_header(buf, off)[4]
        if nxt == 0:
            return parts
        off += nxt
