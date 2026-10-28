"""Prototype of VP2's native mesh decode, checked against recorded VU1 runs.

    python tools/vp2_mesh_proto.py <trace.vu1rec> [max_runs]

For each run of the mesh microprogram (start pc 0x0880, see docs/vp2_vu1.md) the object is decoded from VU1 data
memory without running the microcode: header at TOP, vertices (4 qwords, position at +3), triangle strips, and the
4x4 matrix in vf01..vf04. The screen positions it computes are compared with the XYZ the microprogram sent to the GS
(the run's PATH1 packets). First pass: positions are taken after the run's set-up routines (data memory after the
run), so skinning is not reimplemented yet.
"""
import struct
import sys

MAGIC = 0x52315556
MEMORY = 16 * 1024


def read_records(path):
    data = open(path, 'rb').read()
    pos = 0
    state_size = None
    while pos + 28 <= len(data):
        magic, resume, start, top, itop, max_cycles, has_code = struct.unpack_from('<7I', data, pos)
        if magic != MAGIC:
            raise ValueError('bad record at %d' % pos)
        pos += 28
        code = None
        if has_code:
            code = data[pos:pos + MEMORY]
            pos += MEMORY
        if state_size is None:
            # VU1State's size depends on the compiler's padding: find the one that lands on the packet count and
            # the next record's magic.
            for size in range(640, 720, 4):
                count_at = pos + size + MEMORY + size + MEMORY
                if count_at + 4 > len(data):
                    continue
                count = struct.unpack_from('<I', data, count_at)[0]
                p = count_at + 4
                ok = count < 4096
                for _ in range(count if ok else 0):
                    if p + 4 > len(data):
                        ok = False
                        break
                    p += 4 + struct.unpack_from('<I', data, p)[0]
                if ok and (p == len(data) or (p + 4 <= len(data) and struct.unpack_from('<I', data, p)[0] == MAGIC)):
                    state_size = size
                    break
            if state_size is None:
                raise ValueError('cannot find the VU1State size')
        state_before = data[pos:pos + state_size]
        pos += state_size
        data_before = data[pos:pos + MEMORY]
        pos += MEMORY
        state_after = data[pos:pos + state_size]
        pos += state_size
        data_after = data[pos:pos + MEMORY]
        pos += MEMORY
        count = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        packets = []
        for _ in range(count):
            size = struct.unpack_from('<I', data, pos)[0]
            packets.append(data[pos + 4:pos + 4 + size])
            pos += 4 + size
        yield dict(resume=resume, start=start, top=top, itop=itop, code=code, state_before=state_before,
                   data_before=data_before, state_after=state_after, data_after=data_after, packets=packets)


def vf(state, index):
    return struct.unpack_from('<4f', state, index * 16)


def qword_i(memory, qw):
    return struct.unpack_from('<4i', memory, (qw & 0x3FF) * 16)


def qword_f(memory, qw):
    return struct.unpack_from('<4f', memory, (qw & 0x3FF) * 16)


def packet_xyz(packets):
    """XYZ2/XYZ3 of PACKED GIF packets, in order: (x, y, z, adc) with x, y in 1/16 pixel."""
    out = []
    for packet in packets:
        p = 0
        while p + 16 <= len(packet):
            lo, hi = struct.unpack_from('<QQ', packet, p)
            p += 16
            nloop = lo & 0x7FFF
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            if flg != 0:
                break
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            for _ in range(nloop):
                for reg in regs:
                    if p + 16 > len(packet):
                        return out
                    w = struct.unpack_from('<4I', packet, p)
                    p += 16
                    if reg in (0x4, 0x5):  # XYZF2 / XYZ2 packed: x, y 16 bits, z 32 bits, ADC bit 15 of w's top
                        out.append((w[0] & 0xFFFF, w[1] & 0xFFFF, w[2], (w[3] >> 15) & 1))
            if (lo >> 15) & 1:
                break
    return out


def decode(run, data, state):
    top = run['top']
    h0 = qword_i(data, top)
    vertices_at = top + h0[1]
    strips_at = top + h0[3]
    strip_count = h0[2]
    m = [vf(state, 1), vf(state, 2), vf(state, 3), vf(state, 4)]
    out = []
    v = vertices_at
    for s in range(strip_count):
        count = qword_i(data, strips_at + s)[0]
        for _ in range(count & 0xFFFF):
            px, py, pz, _ = qword_f(data, v + 3)
            x = m[0][0] * px + m[1][0] * py + m[2][0] * pz + m[3][0]
            y = m[0][1] * px + m[1][1] * py + m[2][1] * pz + m[3][1]
            z = m[0][2] * px + m[1][2] * py + m[2][2] * pz + m[3][2]
            w = m[0][3] * px + m[1][3] * py + m[2][3] * pz + m[3][3]
            if w != 0.0:
                out.append((x / w, y / w, z / w))
            else:
                out.append((0.0, 0.0, 0.0))
            v += 4
    return out


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    checked = matched = 0
    code = None
    for run in read_records(path):
        if run['code'] is not None:
            code = run['code']
        if run['start'] != 0x0880 or run['resume'] != 0:
            continue
        got = decode(run, run['data_after'], run['state_after'])
        want = packet_xyz(run['packets'])
        n = min(len(got), len(want))
        # ftoi4: x, y, z times 16 as integers; x and y land in 16 bits (wrapping), z in 32.
        fixed = lambda value, bits: int(value * 16) & ((1 << bits) - 1)
        close = sum(1 for g, w in zip(got, want)
                    if abs(fixed(g[0], 16) - w[0]) <= 1 and abs(fixed(g[1], 16) - w[1]) <= 1 and
                    abs(fixed(g[2], 32) - w[2]) <= 64)
        checked += 1
        matched += close == n and n > 0
        if checked <= 8:
            print('run %d: %d decoded, %d sent, %d close; first decoded %s sent %s' %
                  (checked, len(got), len(want), close, [round(c, 1) for c in got[0]] if got else None,
                   (want[0][0] / 16.0, want[0][1] / 16.0, want[0][2]) if want else None))
        if checked >= limit:
            break
    print('%d runs checked, %d fully matched' % (checked, matched))


if __name__ == '__main__':
    main()
