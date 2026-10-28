"""Probe of VP2's mesh microprogram XYZF2 w word (fog and ADC) against recorded runs.

    python tools/vp2_adc_probe.py <trace.vu1rec> [runs]

Hypotheses: fog = clamp(mem[68].z + mem[68].w * Q, 0, 255) (lit path, mem[15].w != 0), ADC set for a strip's first
two vertices, for a vertex whose last three vertices include one outside the clip volume of the matrix at mem[85..88]
(|x|, |y| or |z| > |w|), and for a back-facing triangle (sign of the screen cross product times mem[16].z, the sign
alternating along the strip).
"""
import sys
import collections

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
import vp2_mesh_proto as m


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    stats = collections.Counter()
    code = None
    runs = 0
    for run in m.read_records(path):
        if run['code'] is not None:
            code = run['code']
        if run['start'] != 0x0880 or run['resume']:
            continue
        runs += 1
        if runs > limit:
            break
        memory = bytearray(run['data_before'])
        if m.run_setup(run, memory) or not m.run_lighting(run, memory, run['state_before'], code):
            continue
        state = run['state_before']
        top = run['top']
        h0 = m.qword_i(memory, top)
        sign0 = m.qword_f(memory, 16)[2]
        fogp = m.qword_f(memory, 68)
        lit = m.qword_i(memory, 15)[3] != 0
        mat = [m.vf(state, 1), m.vf(state, 2), m.vf(state, 3), m.vf(state, 4)]
        clip = m.matrix(memory, 85)
        want = m.packet_vertices(run['packets'])
        v = top + h0[1]
        k_all = 0
        for s in range(h0[2]):
            count = m.qword_i(memory, top + h0[3] + s)[0] & 0xFFFF
            screens = []
            outside = []
            sign = sign0
            for k in range(count):
                p = m.qword_f(memory, v + 3)
                c = m.transform(mat, p, 1.0)
                q = 1.0 / c[3] if c[3] else 0.0
                screens.append((c[0] * q, c[1] * q))
                cc = m.transform(clip, p, 1.0)
                outside.append(any(abs(cc[i]) > abs(cc[3]) for i in range(3)))
                if k_all < len(want):
                    w = want[k_all]
                    xyzw = w['xyz']
                    adc = xyzw[3]
                    if lit:
                        fog = int(max(0.0, min(fogp[2] + fogp[3] * q, 255.0)))
                        stats['fog ok' if True else ''] += 0
                    clipped = any(outside[max(0, k - 2):k + 1])
                    back = None
                    if k >= 2:
                        a, b, cpt = screens[k - 2], screens[k - 1], screens[k]
                        e1 = (a[0] - b[0], a[1] - b[1])
                        e2 = (b[0] - cpt[0], b[1] - cpt[1])
                        cross = e1[0] * e2[1] - e1[1] * e2[0]
                        back = cross * sign
                    predicted = k < 2 or clipped
                    cat = ('k%d' % k if k < 2 else 'clip' if clipped else
                           ('back>0 s16=%g' % sign0) if back and back > 0 else 'back<=0')
                    stats[(cat, 'adc' if adc else 'draw')] += 1
                sign = -sign
                k_all += 1
                v += 4
    for key, value in sorted(stats.items(), key=lambda kv: str(kv[0])):
        print(key, value)


if __name__ == '__main__':
    main()
