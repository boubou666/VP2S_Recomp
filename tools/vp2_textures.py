#!/usr/bin/env python3
"""VP2's textures converted offline to RGBA, for the native renderer to load by the game's own texture key.

VP2 keeps its textures only as prebuilt GS transfer blocks inside its files: a texture header (width, height,
pixel size, +0x8 the pixel block, +0xC the format, +0xE / +0xF log2 width / height, +0x10 the palette block), each
block a transfer header (BITBLTBUF, TRXPOS, TRXREG, TRXDIR, IMAGE) then the data. Those blocks are read here, once,
from the user's own disc (tools/extract.py --assets); while the game runs nothing reads them: the renderer takes the
RGBA files this writes, by the key the game passes its texture cache (file tag, index; mod001 FUN_0029d8f0).

  tools/vp2_textures.py [--assets build/extract/assets] [--index build/vp2_texture_index.json] [--out build/vp2_textures]

Output (game data: build tree only, never committed): <out>/<tag>_<index>.rgba (hexadecimal) = u32 width, u32 height, then
width * height RGBA8, named <tag in hex>_<index in hex> (alpha: the GS's 0x80 = opaque doubled, clamped to 255).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONVERTER = os.path.join(ROOT, 'build', 'port', 'vp2s_texconv.exe')
TEMP = tempfile.mkdtemp(prefix='vp2tex')
BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x13: 8, 0x14: 4}


def block(data, at):
    """A transfer block: (destination format, buffer width, width, height, offset of its data)."""
    regs = {}
    tag, = struct.unpack_from('<Q', data, at)
    nloop = tag & 0x7fff
    for k in range(nloop):
        value, address = struct.unpack_from('<QQ', data, at + 16 + 16 * k)
        regs[address & 0xff] = value
    bitblt, trxreg = regs.get(0x50, 0), regs.get(0x52, 0)
    # The IMAGE tag: right after the registers, or after the VIF codes (FLUSHA, DIRECT) of a PATH2 packet.
    image = at + 16 + 16 * nloop
    for step in range(3):
        lo, = struct.unpack_from('<Q', data, image + 16 * step)
        if (lo >> 58) & 3 == 2 and lo & 0x7fff:
            image += 16 * step
            break
    return (bitblt >> 56) & 0x3f, (bitblt >> 48) & 0x3f, trxreg & 0xfff, (trxreg >> 32) & 0xfff, image + 16


def csm1(i):
    return ((i & 7) | ((i & 0x10) >> 1)) + 16 * (((i >> 5) << 1) | ((i >> 3) & 1))


def transfers(data, at, span):
    """The transfer packets after a texture header: (offset of the A+D tag, format, buffer width, width, height,
    offset of the data), found by their GIF tag (A+D, 4 to 6 registers, BITBLTBUF among them) then IMAGE tag."""
    found = []
    end = min(len(data) - 0x60, at + span)
    p = at + 16
    while p < end:
        lo, hi = struct.unpack_from('<QQ', data, p)
        if (lo >> 58) & 3 == 0 and (lo >> 60) & 15 == 1 and hi & 15 == 0xE and 4 <= (lo & 0x7fff) <= 6:
            fmt, bw, w, h, start = block(data, p)
            ilo, = struct.unpack_from('<Q', data, start - 16)
            if (ilo >> 58) & 3 == 2 and w and h:
                found.append((p, fmt, bw, w, h, start))
                p = start + (ilo & 0x7fff) * 16
                continue
        p += 16
    return found


def convert(data, at):
    w, h, size, _, psm, tw, th, _ = struct.unpack_from('<HHIIHBBI', data, at)
    blocks = transfers(data, at, size * 2 + 0x2000)
    pixel_blocks = [b for b in blocks if b[3] * b[4] * BPP.get(b[1], 32) // 8 == size or (b[3], b[4]) == (w, h)]
    palette_blocks = [b for b in blocks if b[1] == 0x00 and (b[3], b[4]) in ((16, 16), (8, 2))]
    if not pixel_blocks:
        return None, 'no pixel block'
    _, pdpsm, pdbw, pw, ph, pixels = pixel_blocks[0]
    palette_block = palette_blocks[0][0] - at if palette_blocks else None
    if psm in (0x13, 0x14) and palette_block is None:
        return None, 'no palette block'
    if pdpsm == 0x00 and psm in (0x13, 0x14) and CONVERTER:
        # 4- or 8-bit texels kept as a CT32 image in the GS's memory order: laid out and read back by vp2s_texconv.
        cdpsm, _, cw, chh, palette = block(data, at + palette_block)
        if cdpsm != 0x00:
            return None, f'palette 0x{cdpsm:x}'
        count = 256 if psm == 0x13 else 16
        pal = bytearray()
        for i in range(256):
            j = (csm1(i) if count == 256 else i) if i < count else 0
            r, g, b, a = data[palette + 4 * j: palette + 4 * j + 4]
            pal += bytes((r, g, b, min(255, a * 2)))
        job = os.path.join(TEMP, 'block.bin')
        with open(job, 'wb') as f:
            f.write(struct.pack('<6I', psm, w, h, pw, ph, pdbw) + bytes(pal) + data[pixels: pixels + pw * ph * 4])
        result = os.path.join(TEMP, 'out.rgba')
        if subprocess.run([CONVERTER, job, result]).returncode != 0:
            return None, 'converter failed'
        d = open(result, 'rb').read()
        return (w, h, d[8:]), None
    if pdpsm != psm:
        return None, f'stored as 0x{pdpsm:x}'
    colours = None
    if psm in (0x13, 0x14):
        cdpsm, _, cw, chh, palette = block(data, at + palette_block)
        if cdpsm != 0x00:
            return None, f'palette 0x{cdpsm:x}'
        count = 256 if psm == 0x13 else 16
        colours = []
        for i in range(count):
            j = csm1(i) if count == 256 else i
            r, g, b, a = data[palette + 4 * j: palette + 4 * j + 4]
            colours.append(bytes((r, g, b, min(255, a * 2))))
    out = bytearray()
    for y in range(h):
        for x in range(w):
            k = y * pw + x
            if psm == 0x13:
                out += colours[data[pixels + k]]
            elif psm == 0x14:
                out += colours[(data[pixels + k // 2] >> (4 * (k & 1))) & 15]
            else:
                r, g, b, a = data[pixels + 4 * k: pixels + 4 * k + 4]
                out += bytes((r, g, b, min(255, a * 2)))
    return (w, h, bytes(out)), None


def scan(assets):
    """The texture headers in the extracted assets and the game's key for each: the info block before a header
    (info + [info + 0xC] == header) holds the key the game passes its texture cache (+4 file tag, +8 index)."""
    index = []
    for folder, _, names in os.walk(assets):
        for name in names:
            path = os.path.join(folder, name)
            d = open(path, 'rb').read()
            for p in range(0, len(d) - 0x20, 16):
                w, h, size, o8, psm, tw, th, o10 = struct.unpack_from('<HHIIHBBI', d, p)
                if psm not in BPP or not (8 <= w <= 1024 and 8 <= h <= 1024) or tw > 10 or th > 10:
                    continue
                if (1 << tw) < w or (1 << th) < h or size != w * h * BPP[psm] // 8 or not (0 < o8 < 0x10000) or not (0 < o10 < 0x10000):
                    continue
                key = None
                for s in range(max(0, p - 0x400), p, 4):
                    if struct.unpack_from('<I', d, s + 0xC)[0] == p - s:
                        key = struct.unpack_from('<II', d, s + 4)
                        break
                index.append({'file': os.path.relpath(path, assets), 'at': p, 'w': w, 'h': h, 'psm': psm,
                              'tag': key[0] if key else None, 'index': key[1] if key else None})
    return index


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--assets', default=os.path.join(ROOT, 'build', 'extract', 'assets'))
    ap.add_argument('--index', default=os.path.join(ROOT, 'build', 'vp2_texture_index.json'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'vp2_textures'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if not os.path.exists(a.index):
        json.dump(scan(a.assets), open(a.index, 'w'))
    index = json.load(open(a.index))
    results = Counter()
    cache = {}
    for entry in index:
        if entry['tag'] is None:
            results['no key'] += 1
            continue
        path = os.path.join(a.assets, entry['file'])
        data = cache.get(path) or open(path, 'rb').read()
        cache = {path: data}
        try:
            image, why = convert(data, entry['at'])
        except (struct.error, IndexError) as error:
            image, why = None, f'out of range ({error})'
        if image is None:
            results[why.split(' (')[0]] += 1
            continue
        w, h, rgba = image
        name = f"{entry['tag']:08x}_{entry['index']:x}.rgba"
        with open(os.path.join(a.out, name), 'wb') as f:
            f.write(struct.pack('<II', w, h) + rgba)
        results['converted'] += 1
    for k, v in results.most_common():
        print(f'{k}: {v}')


if __name__ == '__main__':
    sys.exit(main())
