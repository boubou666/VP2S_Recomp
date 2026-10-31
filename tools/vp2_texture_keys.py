#!/usr/bin/env python3
"""The game's key for every texture in VP2's files, read from the resource the texture sits in (offline only).

Every texture VP2 loads from a file is a 'FIS\\0' chunk (the files are chains of chunks: u32 magic, u32 payload size,
u32 previous chunk's size, u32 offset of the next chunk, then the payload). The payload starts with the info block
the loaders read:

  +0x00 u16 flags     0x9000 / 0x9001: pixels stored in the texture's own format; 0xb000 / 0xb001: 4- or 8-bit
                      texels stored as a CT32 picture (flag 0x2000), the header's width / height being that picture's;
                      0x1004: CT32 texture without palette
  +0x02 u16 colours   palette entries (0 = none, 32, 64, 96, 224, 256); nonzero: the loader allocates a palette too
  +0x04 u32 tag       file tag ('vp07', 'JOHN', 'SHI\\0', ...): a2 of the texture cache (mod001 FUN_0029d8f0)
  +0x08 u32 index     a3 of the texture cache
  +0x0C u32 header    offset of the texture header from the info block (the palette transfer, if any, in between)

The texture header (u16 w, u16 h, u32 pixel size, u32 +8 pixel block, u16 psm, u8 log2 w, u8 log2 h, u32 +0x10
palette block) is the one tools/vp2_textures.py converts. Loaders that read this layout (recompiled code under
build/recomp): mod001 FUN_002ebb10 (resource = chunk, info = chunk + 0x10), FUN_00238a10, FUN_0023cb20 (info at
object + 0x40 or *(object + 0x28); falls back to the key ('TeXs', running counter) when its slot is not marked),
mod003 FUN_004433b0 (info = *(object + 0x10b8) + 0x10), mod028 FUN_003cd020; all pass t1 = flags. The other callers of
FUN_0029d8f0 build their texture at run time under constant tags ('L@tF', 'g@iL', 'gEpM', 'f@pA', '1@fH', '2@fH',
'f@dH', 'o@?', 'sP', 'TO', 'ku', 'John' for the movie) and have nothing in the files.

  tools/vp2_texture_keys.py [--assets build/extract/assets] [--index build/vp2_texture_index.json]

Writes the index tools/vp2_textures.py reads: a list of {file, at, w, h, psm, tag, index} (at = texture header;
w, h = the header's), plus flags, colours, and texture = [width, height] of the texture as sampled (for flag 0x2000:
2w x 2h for 8-bit, 2w x 4h for 4-bit texels, as a CT32 page of 64 x 32 holds 128 x 64 / 128 x 128 of them).
"""
import argparse
import json
import os
import struct
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x13: 8, 0x14: 4}


def texture(data, chunk):
    """The entry for the 'FIS\\0' chunk at chunk, or None when it is not one (the magic found inside other data)."""
    if chunk % 4 or chunk + 0x20 > len(data):
        return None
    size, = struct.unpack_from('<I', data, chunk + 4)
    flags, colours, tag, index, offset = struct.unpack_from('<HHIII', data, chunk + 0x10)
    at = chunk + 0x10 + offset
    if offset < 0x10 or offset + 0x14 > size or at + 0x14 > len(data):
        return None
    w, h, pixels, _, psm, tw, th, _ = struct.unpack_from('<HHIIHBBI', data, at)
    if psm not in BPP or not (1 <= w <= 1024 and 1 <= h <= 1024) or tw > 10 or th > 10:
        return None
    if flags & 0x2000:
        # 4- or 8-bit texels kept as a CT32 picture: the header gives the picture's size.
        width, height = (2 * w, 2 * h) if psm == 0x13 else (2 * w, 4 * h) if psm == 0x14 else (w, h)
        stored = w * h * 4
    else:
        width, height, stored = w, h, w * h * BPP[psm] // 8
    if pixels != stored or (1 << tw) < width or (1 << th) < height:
        return None
    return {'at': at, 'w': w, 'h': h, 'psm': psm, 'tag': tag, 'index': index,
            'flags': flags, 'colours': colours, 'texture': [width, height]}


def scan(assets):
    """Every texture in the extracted assets, keyed by its chunk's info block."""
    index = []
    for folder, _, names in os.walk(assets):
        for name in sorted(names):
            path = os.path.join(folder, name)
            data = open(path, 'rb').read()
            chunk = data.find(b'FIS\0')
            while chunk >= 0:
                entry = texture(data, chunk)
                if entry:
                    index.append(dict(file=os.path.relpath(path, assets), **entry))
                chunk = data.find(b'FIS\0', chunk + 1)
    return index


def tag_name(tag):
    raw = struct.pack('<I', tag)
    return raw.decode('latin-1') if all(32 <= c < 127 or c == 0 for c in raw) else f'0x{tag:08x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--assets', default=os.path.join(ROOT, 'build', 'extract', 'assets'))
    ap.add_argument('--index', default=os.path.join(ROOT, 'build', 'vp2_texture_index.json'))
    a = ap.parse_args()
    index = scan(a.assets)
    json.dump(index, open(a.index, 'w'))
    keyed = [e for e in index if e['tag']]
    keys = {}
    for e in keyed:
        keys.setdefault((e['tag'], e['index']), set()).add((tuple(e['texture']), e['psm']))
    print(f'textures: {len(index)}, with key: {len(keyed)}, without key (tag 0): {len(index) - len(keyed)}')
    print(f'distinct keys: {len(keys)}, keys naming textures of different size / format: '
          f'{sum(1 for v in keys.values() if len(v) > 1)}')
    print('flags: ' + ', '.join(f'0x{k:04x} {v}' for k, v in Counter(e['flags'] for e in index).most_common()))
    by_tag = Counter(tag_name(e['tag']) for e in keyed)
    distinct = Counter(tag_name(t) for t, _ in keys)
    for tag, count in sorted(by_tag.items()):
        print(f'  {tag!r}: {count} ({distinct[tag]} keys)')


if __name__ == '__main__':
    sys.exit(main())
