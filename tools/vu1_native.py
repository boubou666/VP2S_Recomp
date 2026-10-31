#!/usr/bin/env python3
"""VP2's VU1 programs as C++: each code image the game loads (saved by a run with VP2S_VU1_IMAGES=<dir>: <image>.bin,
and <image>.txt with the start pcs it ran from) lifted by the PS2Recomp fork's tools/vu1_lift.py, with a line that
registers it by image for vp2s_vu1_programs.h. The output is game code, like the EE's: build tree only, never committed.

  tools/vu1_native.py [--images build/vu1_images] [--out build/vu1_native] [--lifter external/PS2Recomp/tools/vu1_lift.py]
"""
import argparse
import glob
import importlib.util
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def image_hash(code):
    # vp2s_vu1_programs.cpp's vp2sVu1ImageHash: FNV-1a 64 over the instruction pairs, folded to 32 bits.
    h = 1469598103934665603
    for (pair,) in struct.iter_unpack('<Q', code[:16384]):
        h = ((h ^ pair) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return (h ^ (h >> 32)) & 0xFFFFFFFF


def return_targets(code):
    # A subroutine's JR returns to the pair after its call's delay slot: BAL (0x21) or JALR (0x25).
    targets = set()
    for pc in range(0, 16384, 8):
        lower = struct.unpack_from('<I', code, pc)[0]
        if (lower >> 25) & 0x7F in (0x21, 0x25):
            targets.add((pc + 16) & 0x3FF8)
    return sorted(targets)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--images', default=os.path.join(ROOT, 'build', 'vu1_images'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'vu1_native'))
    ap.add_argument('--lifter', default=os.path.join(ROOT, 'external', 'PS2Recomp', 'tools', 'vu1_lift.py'))
    # Programs drawn natively by vp2s_vu1_hle.cpp (the meshes, the object set-up, the shadow): not lifted.
    ap.add_argument('--skip', default='880,770,238,6f0,678')
    ap.add_argument('--max-kib', type=int, default=6 * 1024, help='larger output is built without optimisation')
    ap.add_argument('--max-big-kib', type=int, default=32 * 1024, help='larger output is left out')
    a = ap.parse_args()
    spec_loader = importlib.util.spec_from_file_location('vu1_lift', a.lifter)
    lift = importlib.util.module_from_spec(spec_loader)
    spec_loader.loader.exec_module(lift)
    os.makedirs(a.out, exist_ok=True)
    skip = set(int(pc, 16) for pc in a.skip.split(',') if pc)
    done = set()  # (entry, reached-code hash): the same program in several images is lifted once
    for path in sorted(glob.glob(os.path.join(a.images, '*.bin'))):
        code = open(path, 'rb').read()[:16384]
        if len(code) != 16384:
            continue
        name = f'{image_hash(code):08x}'
        listed = os.path.splitext(path)[0] + '.txt'
        entries = sorted({int(line, 16) for line in open(listed) if line.strip()}) if os.path.exists(listed) else []
        if not entries:
            continue
        # One function a start pc: a code image's programs together make functions too big for MSVC to compile.
        targets = return_targets(code)
        for entry in entries:
            if entry in skip:
                continue
            # The pairs the program reaches from this entry, as runs of pairs, and their hash
            # (vp2s_vu1_programs.cpp's vp2sVu1CodeHash): the code memory around them may differ from run to run.
            try:
                prog = lift.Program(lift.parse(code), 'reach', False, False, 'VU1')
                prog.build([entry], targets)
            except lift.GenError as error:
                print(f'{name}:{entry:04x}: not lifted ({error})')
                continue
            reached = sorted({n.pc // 8 for n in prog.nodes} | {(n.pc // 8 + 1) & 0x7FF for n in prog.nodes})
            ranges = []
            for pair in reached:
                if ranges and ranges[-1][0] + ranges[-1][1] == pair:
                    ranges[-1][1] += 1
                else:
                    ranges.append([pair, 1])
            h = 1469598103934665603
            for first, count in ranges:
                for pair in range(first, first + count):
                    h = ((h ^ struct.unpack_from('<Q', code, pair * 8)[0]) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            if (entry, h) in done:
                continue
            function = f'vp2sVu1_{entry:04x}_{h:016x}'
            spec = {'hash': lift.code_hash(code), 'pairs': 2048, 'function': function, 'file': f'vu1_{entry:04x}_{h:016x}.cpp',
                    'entries': [entry], 'targets': targets, 'unit': 'VU1'}
            try:
                text, nodes = lift.generate(code, spec)
            except lift.GenError as error:
                print(f'{name}:{entry:04x}: not lifted ({error})')
                continue
            # Larger output goes to big/, which the build compiles without optimisation (MSVC's optimiser runs out of
            # heap on it); past --max-big-kib it is left out.
            out_dir = a.out
            if len(text) > a.max_kib * 1024:
                if len(text) > a.max_big_kib * 1024:
                    print(f'{name}:{entry:04x}: left out ({len(text) // 1024} KiB)')
                    continue
                out_dir = os.path.join(a.out, 'big')
                os.makedirs(out_dir, exist_ok=True)
            done.add((entry, h))
            listed_ranges = ', '.join(f'{first}, {count}' for first, count in ranges)
            text += ('\n#include "vp2s_vu1_programs.h"\n\nnamespace\n{\n'
                     f'    const uint16_t kRanges[] = {{{listed_ranges}}};\n'
                     f'    const bool g_registered = vp2sRegisterVu1Program({entry:#x}u, kRanges, {len(ranges)}u, 0x{h:016x}ull,\n'
                     f'                                                     &ps2x::vu1lift::{function}<true>);\n}}\n')
            lift.write(os.path.join(out_dir, spec['file']), text)
            print(f'{name}:{entry:04x}: {nodes} timed pairs, {len(text) // 1024} KiB -> {spec["file"]}')


if __name__ == '__main__':
    sys.exit(main())
