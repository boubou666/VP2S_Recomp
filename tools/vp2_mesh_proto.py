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


def set_f(memory, qw, values, mask='xyzw'):
    for i, c in enumerate('xyzw'):
        if c in mask:
            struct.pack_into('<f', memory, (qw & 0x3FF) * 16 + i * 4, values[i])


def matrix(memory, qw):
    return [qword_f(memory, qw + r) for r in range(4)]


def transform(m, v, w):
    return [m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2] + m[3][i] * w for i in range(4)]


def run_setup(run, memory):
    """The object's set-up routines (header 5.x..w, data at 4.x..w), done natively on a copy of VU memory: 8 integer
    normals (12-bit) and positions (12.4) to float, 2 rigid skinning, 4 two-bone skinning, 6 integer normals only.
    Returns the routines it did not know."""
    top = run['top']
    h2, h4, h5 = qword_i(memory, top + 2), qword_i(memory, top + 4), qword_i(memory, top + 5)
    vertices = top + qword_i(memory, top)[1]
    count = h2[1]
    unknown = []
    for routine, data in zip(h5, h4):
        if routine in (0, 46):
            continue
        base = top + data
        if routine == 8:
            for k in range(count):
                v = vertices + 4 * k
                n = qword_i(memory, v)
                p = qword_i(memory, v + 3)
                set_f(memory, v, [n[0] / 4096.0, n[1] / 4096.0, n[2] / 4096.0, 0.0], 'xyz')
                set_f(memory, v + 3, [p[0] / 16.0, p[1] / 16.0, p[2] / 16.0, 0.0], 'xyz')
        elif routine == 6:
            for k in range(count):
                v = vertices + 4 * k
                n = qword_i(memory, v)
                set_f(memory, v, [n[0] / 4096.0, n[1] / 4096.0, n[2] / 4096.0, 0.0], 'xyz')
        elif routine == 2:
            for k in range(count + 1):
                v = vertices + 4 * k
                bone = qword_i(memory, v + 3)[3]
                m = matrix(memory, bone)
                n = qword_f(memory, v)
                p = qword_f(memory, v + 3)
                set_f(memory, v, transform(m, n, 0.0), 'xyz')
                set_f(memory, v + 3, transform(m, p, 1.0), 'xyzw')
        elif routine == 4:
            for k in range(count):
                v = vertices + 4 * k
                e = qword_i(memory, base + k)
                ef = qword_f(memory, base + k)
                a, b = matrix(memory, e[0]), matrix(memory, e[2])
                m = [[a[r][c] * ef[1] + b[r][c] * ef[3] for c in range(4)] for r in range(4)]
                n = qword_f(memory, v)
                p = qword_f(memory, v + 3)
                set_f(memory, v, transform(m, n, 0.0), 'xyz')
                set_f(memory, v + 3, transform(m, p, 1.0), 'xyz')
        elif routine == 48:
            # Texture scrolling: an offset (S, T in .zw) picked by the routine's data, added to every vertex's UV.
            index = qword_i(memory, base)[0]
            off = qword_f(memory, 38 + index)
            for k in range(count):
                v = vertices + 4 * k
                uv = list(qword_i(memory, v + 1))
                for c in range(2):
                    uv[c] = int((uv[c] / 4096.0 + off[2 + c]) * 4096.0)
                struct.pack_into('<2i', memory, (v + 1 & 0x3FF) * 16, uv[0], uv[1])
        else:
            unknown.append(routine)
    return unknown


def library_layout(code):
    """Where this build of the microcode library keeps its tables, found by following its code from 0x0880: the
    BAL at 0x08a8 to the set-up dispatcher, whose IADDIU vi09 gives the set-up table (instruction index) and whose
    IADDIU vi01, vi01, n give the lighting tables A, B, C, D in that order (D the last). Returns (setup, D) or None."""
    if code is None:
        return None
    lo = struct.unpack_from('<I', code, 0x08a8)[0]
    if (lo >> 25) & 0x7F != 0x21:
        return None
    imm = lo & 0x7FF
    imm = imm - 0x800 if imm & 0x400 else imm
    sub = 0x08a8 + 8 + imm * 8
    setup = None
    tables = []
    for pc in range(sub, min(sub + 0x400, len(code)), 8):
        lo = struct.unpack_from('<I', code, pc)[0]
        if (lo >> 25) & 0x7F == 0x08:  # IADDIU
            it, is_ = (lo >> 16) & 0x1F, (lo >> 11) & 0x1F
            value = (((lo >> 21) & 0xF) << 11) | (lo & 0x7FF)
            if it == 9 and is_ == 0 and setup is None:
                setup = value
            elif it == 1 and is_ == 1:
                tables.append(value)
    if setup is None or not tables:
        return None
    return setup, max(tables)


def light_w_clamped(code):
    """Whether lighting mode 1 clamps the fourth light's term at 0 (maxx.xyzw) or lets it go negative (maxx.xyz, a
    darkening light): the code image's own choice, read from the first MAX(bc x) after the routine's entry. Table D
    entry 1 is at 0x2b20 in the builds seen so far."""
    if code is None:
        return True
    target = None
    layout = library_layout(code)
    entry = (layout[1] + 2) * 8 if layout else 0x2b20
    lo = struct.unpack_from('<I', code, entry)[0]
    if (lo >> 25) & 0x7F == 0x20:
        imm = lo & 0x7FF
        imm = imm - 0x800 if imm & 0x400 else imm
        target = entry + 8 + imm * 8
    if target is None:
        return True
    for pc in range(target, target + 0x100, 8):
        up = struct.unpack_from('<I', code, pc + 4)[0]
        if (up & 0x3F) == 0x10:  # MAXx
            return ((up >> 21) & 1) != 0
    return True


def run_lighting(run, memory, state, code=None):
    """Vertex colours (+2) lit natively, for the lighting the field uses (mem[14].w = 0, table D): mode 0 colour times
    mem[13], mode 1 four directional lights (directions vf05..vf07 by component, colours mem[5..8],
    ambient mem[19], alpha mem[13].w) times the colour. Clamped to [0, mem[0]], truncated like ftoi0. Returns False for
    a mode not done yet."""
    top = run['top']
    mode = qword_i(memory, top + 8)[1]
    flags = qword_i(memory, 14)
    count = qword_i(memory, top + 2)[1]
    vertices = top + qword_i(memory, top)[1]
    limit = qword_f(memory, 0)
    if flags[3] != 0:
        return False

    def apply(v, factor):
        c = qword_i(memory, v + 2)
        out = [int(max(0.0, min(c[i] * factor[i], limit[i]))) for i in range(4)]
        struct.pack_into('<4i', memory, (v + 2 & 0x3FF) * 16, *out)

    if mode == 0:
        # Seen on the traces: with mem[14].x the colour is scaled by mem[13]; without it RGB goes to 0 and only the
        # alpha is scaled (the entry branches from a branch's delay slot; to confirm against the code).
        scale = list(qword_f(memory, 13))
        if not flags[0]:
            scale[0] = scale[1] = scale[2] = 0.0
        for k in range(count):
            apply(vertices + 4 * k, scale)
        return True
    if mode == 1:
        d = [vf(state, 5), vf(state, 6), vf(state, 7)]
        colours = [qword_f(memory, 5 + i) for i in range(4)]
        ambient = qword_f(memory, 19)
        alpha = qword_f(memory, 13)[3]
        clamp_w = light_w_clamped(code)
        for k in range(count):
            v = vertices + 4 * k
            n = qword_f(memory, v)
            dots = [d[0][i] * n[0] + d[1][i] * n[1] + d[2][i] * n[2] for i in range(4)]
            dots = [max(0.0, x) if (i < 3 or clamp_w) else x for i, x in enumerate(dots)]
            factor = [sum(colours[i][c] * dots[i] for i in range(4)) + ambient[c] for c in range(3)] + [alpha]
            apply(v, factor)
        return True
    return False


def packet_vertices(packets):
    """Per vertex of PACKED GIF packets: dict of st (S, T, Q floats), rgba (ints), xyz (x, y, z, adc)."""
    out = []
    current = {}
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
                    if reg == 0x2:
                        current['st'] = struct.unpack_from('<3f', packet, p)
                    elif reg == 0x1:
                        current['rgba'] = struct.unpack_from('<4I', packet, p)
                    elif reg in (0x4, 0x5):
                        w = struct.unpack_from('<4I', packet, p)
                        current['xyz'] = (w[0] & 0xFFFF, w[1] & 0xFFFF, w[2], (w[3] >> 15) & 1)
                        current['fog'] = (w[3] >> 4) & 0xFF if reg == 0x4 else None
                        current['raw'] = w
                        out.append(dict(current))
                    p += 16
            if (lo >> 15) & 1:
                break
    return out


def decode_full(run, data, state):
    """Each vertex: screen x, y, z (float, before ftoi4), S, T, Q, RGBA."""
    top = run['top']
    h0 = qword_i(data, top)
    vertices_at = top + h0[1]
    strips_at = top + h0[3]
    m = [vf(state, 1), vf(state, 2), vf(state, 3), vf(state, 4)]
    out = []
    v = vertices_at
    for s in range(h0[2]):
        for _ in range(qword_i(data, strips_at + s)[0] & 0xFFFF):
            p = qword_f(data, v + 3)
            c = transform(m, p, 1.0)
            q = 1.0 / c[3] if c[3] != 0.0 else 0.0
            uv = qword_i(data, v + 1)
            out.append(dict(xyz=(c[0] * q, c[1] * q, c[2] * q), st=(uv[0] / 4096.0 * q, uv[1] / 4096.0 * q, uv[2] / 4096.0 * q),
                            rgba=qword_i(data, v + 2)))
            v += 4
    return out


def decode_packets(run, data, state):
    """Each vertex as the GS gets it: S, T, Q, RGBA, XYZF2 (x, y, z as ftoi4 gives them, fog, adc), and the strip it
    starts (True on a strip's first vertex). ADC: a strip's first two vertices; any of the last three vertices
    outside the clip volume of the matrix at mem[85..88]; a back-facing triangle when the object culls (header 15.x
    + mem[41].z == 0: screen cross product of the last three, sign alternating along the strip, positive = back).
    Fog (mem[15].w != 0): mem[68].z + mem[68].w * Q, clamped to 0..255."""
    top = run['top']
    h0 = qword_i(data, top)
    m = [vf(state, 1), vf(state, 2), vf(state, 3), vf(state, 4)]
    clip = matrix(data, 85)
    culls = qword_i(data, top + 15)[0] + qword_i(data, 41)[2] == 0
    lit = qword_i(data, 15)[3] != 0
    fogp = qword_f(data, 68)
    out = []
    v = top + h0[1]
    for s in range(h0[2]):
        screens = []
        outside = []
        for k in range(qword_i(data, top + h0[3] + s)[0] & 0xFFFF):
            p = qword_f(data, v + 3)
            c = transform(m, p, 1.0)
            q = 1.0 / c[3] if c[3] != 0.0 else 0.0
            x, y, z = c[0] * q, c[1] * q, c[2] * q
            screens.append((x, y))
            cc = transform(clip, p, 1.0)
            outside.append(any(abs(cc[i]) > abs(cc[3]) for i in range(3)))
            adc = k < 2 or any(outside[max(0, k - 2):k + 1])
            if not adc and culls:
                a, b, d = screens[k - 2], screens[k - 1], screens[k]
                cross = (a[0] - b[0]) * (b[1] - d[1]) - (a[1] - b[1]) * (b[0] - d[0])
                adc = cross * (1 if k % 2 == 0 else -1) > 0
            fog = int(max(0.0, min(fogp[2] + fogp[3] * q, 255.0))) if lit else None
            uv = qword_i(data, v + 1)
            out.append(dict(st=(uv[0] / 4096.0 * q, uv[1] / 4096.0 * q, uv[2] / 4096.0 * q), rgba=qword_i(data, v + 2),
                            xyz=(int(x * 16), int(y * 16), int(z * 16)), fog=fog, adc=adc, first=k == 0, k=k))
            v += 4
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
    limit = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 20
    native = '--native' in sys.argv
    checked = matched = skipped = 0
    code = None
    for run in read_records(path):
        if run['code'] is not None:
            code = run['code']
        if run['start'] != 0x0880 or run['resume'] != 0:
            continue
        if native:
            memory = bytearray(run['data_before'])
            unknown = run_setup(run, memory)
            if unknown:
                skipped += 1
                continue
            got = decode(run, memory, run['state_before'])
        else:
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
    print('%d runs checked, %d fully matched, %d skipped (set-up routine not done yet)' % (checked, matched, skipped))


if __name__ == '__main__':
    main()
