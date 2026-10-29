#include "vp2s_vu1_hle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// The object a run of program 0x0880 draws, decoded from VU1 data memory (docs/vp2_vu1.md, checked against recorded
// runs with tools/vp2_mesh_proto.py and vp2s_hle_check):
//   header at TOP: 0.y vertices, 0.z strip count, 0.w strip lengths, 2.y vertex count, 4/5 set-up routines (5.n the
//   table entry, 4.n its data), 8.y lighting mode, 13 the strip tag (y..w), 15.x culling off when non-zero
//   vertex: 4 qwords, +0 normal, +1 UV (12-bit fixed), +2 colour (ints), +3 position
//   globals: 0 colour limit, 5..8 light colours, 13 mode-0 scale / alpha, 14 lighting flags, 15.w fog on,
//   16.z cull sign, 19 ambient, 38.. UV scroll offsets, 41.z, 68 fog (z + w * Q), 85..88 clip matrix
//   registers: vf01..vf04 the screen matrix, vf05..vf07 light directions (set by the frame's other programs)

namespace
{
    using Vec4 = std::array<float, 4>;
    using Mat4 = std::array<Vec4, 4>;

    constexpr uint32_t kQwords = 1024u;

    struct Memory
    {
        std::array<uint8_t, kQwords * 16u> bytes{};

        uint8_t *at(uint32_t qw, uint32_t lane) { return bytes.data() + (qw % kQwords) * 16u + lane * 4u; }
        const uint8_t *at(uint32_t qw, uint32_t lane) const { return bytes.data() + (qw % kQwords) * 16u + lane * 4u; }
        int32_t i(uint32_t qw, uint32_t lane) const
        {
            int32_t v;
            std::memcpy(&v, at(qw, lane), 4u);
            return v;
        }
        float f(uint32_t qw, uint32_t lane) const
        {
            float v;
            std::memcpy(&v, at(qw, lane), 4u);
            return v;
        }
        void setI(uint32_t qw, uint32_t lane, int32_t v) { std::memcpy(at(qw, lane), &v, 4u); }
        void setF(uint32_t qw, uint32_t lane, float v) { std::memcpy(at(qw, lane), &v, 4u); }
        Vec4 vf(uint32_t qw) const { return {f(qw, 0), f(qw, 1), f(qw, 2), f(qw, 3)}; }
        Mat4 matrix(uint32_t qw) const { return {vf(qw), vf(qw + 1u), vf(qw + 2u), vf(qw + 3u)}; }
    };

    Vec4 transform(const Mat4 &m, const Vec4 &v, float w)
    {
        Vec4 out{};
        for (int c = 0; c < 4; ++c)
            out[c] = m[0][c] * v[0] + m[1][c] * v[1] + m[2][c] * v[2] + m[3][c] * w;
        return out;
    }

    int32_t ftoi(double value)
    {
        if (!(value == value))
            return 0;
        return static_cast<int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
    }

    int32_t signedImm11(uint32_t word)
    {
        const int32_t imm = static_cast<int32_t>(word & 0x7FFu);
        return (imm & 0x400) ? imm - 0x800 : imm;
    }

    uint32_t lower(const uint8_t *code, uint32_t pc)
    {
        uint32_t v;
        std::memcpy(&v, code + (pc & 0x3FF8u), 4u);
        return v;
    }

    uint32_t upper(const uint8_t *code, uint32_t pc)
    {
        uint32_t v;
        std::memcpy(&v, code + (pc & 0x3FF8u) + 4u, 4u);
        return v;
    }

    // The library build's layout, followed from its code: the BAL at 0x08a8 to the set-up dispatcher; there, the
    // IADDIU into vi09 is the set-up table and the IADDIU vi01, vi01, n give the lighting tables (D, the last, is
    // the one with mem[14].w == 0). Then whether lighting mode 1 clamps the fourth light (MAXx.xyzw) or not.
    struct Layout
    {
        bool valid = false;
        uint32_t setupTable = 0;
        uint32_t lightTableD = 0;
        bool clampFourthLight = true;
    };

    Layout findLayout(const uint8_t *code)
    {
        Layout layout;
        const uint32_t bal = lower(code, 0x08a8u);
        if (((bal >> 25) & 0x7Fu) != 0x21u)
            return layout;
        const uint32_t dispatcher = static_cast<uint32_t>(0x08a8 + 8 + signedImm11(bal) * 8);
        bool haveSetup = false;
        uint32_t tableD = 0;
        for (uint32_t pc = dispatcher; pc < dispatcher + 0x400u && pc < 0x4000u; pc += 8u)
        {
            const uint32_t lo = lower(code, pc);
            if (((lo >> 25) & 0x7Fu) != 0x08u)
                continue;
            const uint32_t it = (lo >> 16) & 0x1Fu, is = (lo >> 11) & 0x1Fu;
            const uint32_t value = (((lo >> 21) & 0xFu) << 11) | (lo & 0x7FFu);
            if (it == 9u && is == 0u && !haveSetup)
            {
                layout.setupTable = value;
                haveSetup = true;
            }
            else if (it == 1u && is == 1u)
            {
                tableD = std::max(tableD, value);
            }
        }
        if (!haveSetup || tableD == 0u)
            return layout;
        layout.lightTableD = tableD;
        const uint32_t entry = (tableD + 2u) * 8u; // mode 1
        const uint32_t branch = lower(code, entry);
        if (((branch >> 25) & 0x7Fu) == 0x20u)
        {
            const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(entry) + 8 + signedImm11(branch) * 8);
            for (uint32_t pc = target; pc < target + 0x100u && pc < 0x4000u; pc += 8u)
            {
                const uint32_t up = upper(code, pc);
                if ((up & 0x3Fu) == 0x10u) // MAXx
                {
                    layout.clampFourthLight = ((up >> 21) & 1u) != 0u;
                    break;
                }
            }
        }
        layout.valid = true;
        return layout;
    }

    // VP2S_NATIVE_MESH_LOG: each distinct case the native code does not cover yet, once ("setup 36", "lighting 3/0").
    void noteUnhandled(const char *what, int a, int b)
    {
        static const bool s_on = std::getenv("VP2S_NATIVE_MESH_LOG") != nullptr;
        if (!s_on)
            return;
        static std::mutex s_mutex;
        static std::vector<std::pair<int, int>> s_seen[2];
        const int kind = what[0] == 's' ? 0 : 1;
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto &seen : s_seen[kind])
            if (seen.first == a && seen.second == b)
                return;
        s_seen[kind].emplace_back(a, b);
        std::fprintf(stderr, "[vp2s-native] not handled: %s %d/%d\n", what, a, b);
    }

    // The set-up routines (header 5.x..w with data at 4.x..w), in order. False for one not done yet.
    bool runSetup(Memory &mem, uint32_t top)
    {
        const uint32_t vertices = top + static_cast<uint32_t>(mem.i(top, 1));
        const int32_t count = mem.i(top + 2u, 1);
        for (int r = 0; r < 4; ++r)
        {
            const int32_t routine = mem.i(top + 5u, static_cast<uint32_t>(r));
            const uint32_t base = top + static_cast<uint32_t>(mem.i(top + 4u, static_cast<uint32_t>(r)));
            switch (routine)
            {
            case 0:
            case 46:
                break;
            case 8: // integer normal (12-bit fixed) and position (12.4) to float
                for (int32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                    for (uint32_t c = 0; c < 3u; ++c)
                    {
                        mem.setF(v, c, static_cast<float>(mem.i(v, c)) / 4096.0f);
                        mem.setF(v + 3u, c, static_cast<float>(mem.i(v + 3u, c)) / 16.0f);
                    }
                }
                break;
            case 6: // integer normals only
                for (int32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                    for (uint32_t c = 0; c < 3u; ++c)
                        mem.setF(v, c, static_cast<float>(mem.i(v, c)) / 4096.0f);
                }
                break;
            case 2: // rigid skinning: +3.w is the bone matrix's address
                for (int32_t k = 0; k <= count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                    const Mat4 m = mem.matrix(static_cast<uint32_t>(mem.i(v + 3u, 3)));
                    const Vec4 n = transform(m, mem.vf(v), 0.0f);
                    const Vec4 p = transform(m, mem.vf(v + 3u), 1.0f);
                    for (uint32_t c = 0; c < 3u; ++c)
                        mem.setF(v, c, n[c]);
                    for (uint32_t c = 0; c < 4u; ++c)
                        mem.setF(v + 3u, c, p[c]);
                }
                break;
            case 4: // two-bone skinning: per vertex x = matrix A, z = matrix B, y / w their weights
                for (int32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                    const uint32_t e = base + static_cast<uint32_t>(k);
                    const Mat4 a = mem.matrix(static_cast<uint32_t>(mem.i(e, 0)));
                    const Mat4 b = mem.matrix(static_cast<uint32_t>(mem.i(e, 2)));
                    const float wa = mem.f(e, 1), wb = mem.f(e, 3);
                    Mat4 m{};
                    for (int row = 0; row < 4; ++row)
                        for (int c = 0; c < 4; ++c)
                            m[row][c] = a[row][c] * wa + b[row][c] * wb;
                    const Vec4 n = transform(m, mem.vf(v), 0.0f);
                    const Vec4 p = transform(m, mem.vf(v + 3u), 1.0f);
                    for (uint32_t c = 0; c < 3u; ++c)
                    {
                        mem.setF(v, c, n[c]);
                        mem.setF(v + 3u, c, p[c]);
                    }
                }
                break;
            case 48: // texture scrolling: offset .zw at mem[38 + data.x], added to the 12-bit UV
            {
                const Vec4 off = mem.vf(38u + static_cast<uint32_t>(mem.i(base, 0)));
                for (int32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k) + 1u;
                    for (uint32_t c = 0; c < 2u; ++c)
                        mem.setI(v, c, ftoi((static_cast<double>(mem.i(v, c)) / 4096.0 + off[2 + c]) * 4096.0));
                }
                break;
            }
            default:
                noteUnhandled("setup routine/slot", routine, r);
                return false;
            }
        }
        return true;
    }

    // Vertex colours lit in place (lighting table D, mem[14].w == 0). False for a mode not done yet.
    bool runLighting(Memory &mem, const VU1State &state, uint32_t top, const Layout &layout)
    {
        const int32_t mode = mem.i(top + 8u, 1);
        if (mem.i(14u, 3) != 0)
        {
            // Lighting table A (mem[14].w != 0), then B (mem[14].y) and C (mem[14].z) when set. A's modes 1-3 are
            // table D's code; its mode 0 (0x31c0) gives every vertex mem[13] as it is (no mem[14].x test).
            if (mem.i(14u, 1) != 0 || mem.i(14u, 2) != 0 || (mode != 0 && mode != 1))
            {
                noteUnhandled("lighting mode/table (mem[14].w)", mode, mem.i(14u, 3) * 100 + mem.i(14u, 1) * 10 + mem.i(14u, 2));
                return false;
            }
            if (mode == 0)
            {
                const Vec4 factor = mem.vf(13u);
                const int32_t count = mem.i(top + 2u, 1);
                const uint32_t vertices = top + static_cast<uint32_t>(mem.i(top, 1));
                const Vec4 limit = mem.vf(0u);
                for (int32_t k = 0; k < count; ++k)
                {
                    const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                    for (uint32_t c = 0; c < 4u; ++c)
                    {
                        const float lit = static_cast<float>(mem.i(v + 2u, c)) * factor[c];
                        mem.setI(v + 2u, c, ftoi(std::max(0.0f, std::min(lit, limit[c]))));
                    }
                }
                return true;
            }
        }
        const int32_t count = mem.i(top + 2u, 1);
        const uint32_t vertices = top + static_cast<uint32_t>(mem.i(top, 1));
        const Vec4 limit = mem.vf(0u);
        auto apply = [&](uint32_t v, const Vec4 &factor) {
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const float lit = static_cast<float>(mem.i(v + 2u, c)) * factor[c];
                mem.setI(v + 2u, c, ftoi(std::max(0.0f, std::min(lit, limit[c]))));
            }
        };
        if (mode == 0)
        {
            Vec4 scale = mem.vf(13u);
            if (mem.i(14u, 0) == 0)
                scale[0] = scale[1] = scale[2] = 0.0f;
            for (int32_t k = 0; k < count; ++k)
                apply(vertices + 4u * static_cast<uint32_t>(k), scale);
            return true;
        }
        if (mode == 1)
        {
            const Vec4 d[3] = {{state.vf[5][0], state.vf[5][1], state.vf[5][2], state.vf[5][3]},
                               {state.vf[6][0], state.vf[6][1], state.vf[6][2], state.vf[6][3]},
                               {state.vf[7][0], state.vf[7][1], state.vf[7][2], state.vf[7][3]}};
            const Vec4 colours[4] = {mem.vf(5u), mem.vf(6u), mem.vf(7u), mem.vf(8u)};
            const Vec4 ambient = mem.vf(19u);
            const float alpha = mem.f(13u, 3);
            for (int32_t k = 0; k < count; ++k)
            {
                const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                const Vec4 n = mem.vf(v);
                float dots[4];
                for (int i = 0; i < 4; ++i)
                {
                    dots[i] = d[0][i] * n[0] + d[1][i] * n[1] + d[2][i] * n[2];
                    if (i < 3 || layout.clampFourthLight)
                        dots[i] = std::max(0.0f, dots[i]);
                }
                Vec4 factor{};
                for (int c = 0; c < 3; ++c)
                    factor[c] = colours[0][c] * dots[0] + colours[1][c] * dots[1] + colours[2][c] * dots[2] +
                                colours[3][c] * dots[3] + ambient[c];
                factor[3] = alpha;
                apply(v, factor);
            }
            return true;
        }
        noteUnhandled("lighting mode/table (mem[14].w)", mode, 0);
        return false;
    }

    void put32(std::vector<uint8_t> &out, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        const uint32_t words[4] = {a, b, c, d};
        const size_t at = out.size();
        out.resize(at + 16u);
        std::memcpy(out.data() + at, words, 16u);
    }

    uint32_t bits(float value)
    {
        uint32_t v;
        std::memcpy(&v, &value, 4u);
        return v;
    }
}

namespace
{
    thread_local int g_rejected = 0; // why vp2sBuildMeshPacket last returned false (VP2S_NATIVE_MESH_LOG's columns)
}

bool vp2sBuildMeshPacket(const uint8_t *code, const uint8_t *data, const VU1State &state, uint32_t top,
                         std::vector<uint8_t> &packet, Vp2sNativeMesh *native)
{
    const Layout layout = findLayout(code);
    g_rejected = 11;
    if (!layout.valid)
        return false;
    static Memory mem; // one run at a time (the VIF1 thread)
    std::memcpy(mem.bytes.data(), data, mem.bytes.size());
    top %= kQwords;
    const int32_t cullSum = mem.i(top + 15u, 0) + mem.i(41u, 2);
    g_rejected = cullSum == 3 ? 12 : 13;
    if (cullSum == 3 || mem.i(41u, 2) != 0) // paths not checked yet
        return false;
    g_rejected = 14;
    if (!runSetup(mem, top) || !runLighting(mem, state, top, layout))
        return false;
    g_rejected = 15;

    const Mat4 screen = {Vec4{state.vf[1][0], state.vf[1][1], state.vf[1][2], state.vf[1][3]},
                         Vec4{state.vf[2][0], state.vf[2][1], state.vf[2][2], state.vf[2][3]},
                         Vec4{state.vf[3][0], state.vf[3][1], state.vf[3][2], state.vf[3][3]},
                         Vec4{state.vf[4][0], state.vf[4][1], state.vf[4][2], state.vf[4][3]}};
    const Mat4 clip = mem.matrix(85u);
    const bool culls = cullSum == 0;
    const bool fogOn = mem.i(15u, 3) != 0;
    const Vec4 fog = mem.vf(68u);
    const uint32_t tagY = static_cast<uint32_t>(mem.i(top + 13u, 1));
    const uint32_t tagZ = static_cast<uint32_t>(mem.i(top + 13u, 2));
    const uint32_t tagW = static_cast<uint32_t>(mem.i(top + 13u, 3));
    const int32_t strips = mem.i(top, 2);
    const uint32_t lengths = top + static_cast<uint32_t>(mem.i(top, 3));
    uint32_t v = top + static_cast<uint32_t>(mem.i(top, 1));

    packet.clear();
    if (native)
    {
        native->triangles.clear();
        native->prim = (tagY >> 15) & 0x7FFu; // the strip tag's PRIM (PRE set)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                native->matrix[4 * c + r] = screen[c][r];
    }
    // A triangle strip the native renderer draws needs no GS packet (the caller sends the packet otherwise).
    const bool packetWanted = !native || (native->prim & 7u) != 4u;
    ps2x::native::Vertex strip[3];
    for (int32_t s = 0; s < strips; ++s)
    {
        const uint32_t count = static_cast<uint32_t>(mem.i(lengths + static_cast<uint32_t>(s), 0)) & 0xFFFFu;
        if (packetWanted)
            put32(packet, count | (s + 1 == strips ? 0x8000u : 0u), tagY, tagZ, tagW);
        float sx[3] = {}, sy[3] = {};
        bool outside[3] = {};
        for (uint32_t k = 0; k < count; ++k, v += 4u)
        {
            const Vec4 p = mem.vf(v + 3u);
            const Vec4 c = transform(screen, p, 1.0f);
            const float q = c[3] != 0.0f ? 1.0f / c[3] : 0.0f;
            const float x = c[0] * q, y = c[1] * q, z = c[2] * q;
            const Vec4 cc = transform(clip, p, 1.0f);
            const bool out = std::fabs(cc[0]) > std::fabs(cc[3]) || std::fabs(cc[1]) > std::fabs(cc[3]) ||
                             std::fabs(cc[2]) > std::fabs(cc[3]);
            sx[0] = sx[1], sy[0] = sy[1], outside[0] = outside[1];
            sx[1] = sx[2], sy[1] = sy[2], outside[1] = outside[2];
            sx[2] = x, sy[2] = y, outside[2] = out;
            // A strip's first two vertices kick no triangle: only the clip test (over the vertices so far) marks them.
            bool adc = outside[2] || (k >= 1u && outside[1]) || (k >= 2u && outside[0]);
            if (!adc && culls && k >= 2u)
            {
                const float cross = (sx[0] - sx[1]) * (sy[1] - sy[2]) - (sy[0] - sy[1]) * (sx[1] - sx[2]);
                adc = cross * ((k % 2u) == 0u ? 1.0f : -1.0f) > 0.0f;
                static const bool s_adcDebug = std::getenv("VP2S_ADC_DEBUG") != nullptr;
                if (s_adcDebug && adc)
                    std::printf("adc-debug cull k=%u cross=%g\n", k, cross);
            }
            else if (adc && k >= 2u)
            {
                static const bool s_adcDebug = std::getenv("VP2S_ADC_DEBUG") != nullptr;
                if (s_adcDebug)
                    std::printf("adc-debug clip k=%u out=%d%d%d cc=%g,%g,%g,%g\n", k, outside[0] ? 1 : 0, outside[1] ? 1 : 0,
                                outside[2] ? 1 : 0, cc[0], cc[1], cc[2], cc[3]);
            }
            const float u = static_cast<float>(mem.i(v + 1u, 0)) / 4096.0f, t = static_cast<float>(mem.i(v + 1u, 1)) / 4096.0f;
            const float r = static_cast<float>(mem.i(v + 1u, 2)) / 4096.0f;
            if (packetWanted)
            {
                put32(packet, bits(u * q), bits(t * q), bits(r * q), bits(fog[3]));
                put32(packet, static_cast<uint32_t>(mem.i(v + 2u, 0)), static_cast<uint32_t>(mem.i(v + 2u, 1)),
                      static_cast<uint32_t>(mem.i(v + 2u, 2)), static_cast<uint32_t>(mem.i(v + 2u, 3)));
            }
            uint32_t w = 0x0FF0u;
            if (fogOn)
            {
                // Q in double: the fog scale (mem[68].w, ~2e5) magnifies a float Q's last bits into a different fog.
                const double qd = c[3] != 0.0f ? 1.0 / static_cast<double>(c[3]) : 0.0;
                const double f = std::max(0.0, std::min(static_cast<double>(fog[2]) + static_cast<double>(fog[3]) * qd, 255.0));
                w = static_cast<uint32_t>(ftoi(f * 16.0));
            }
            const uint32_t fogW = w; // before ADC: the vertex still serves the strip's next triangles
            if (adc)
                w = 0x8FF0u;
            if (native)
            {
                // The vertex as the native renderer takes it: object space with the screen matrix, the texture
                // coordinates before the divide by w, the lit colour, the fog factor (1 keeps the colour).
                ps2x::native::Vertex nv;
                nv.x = p[0];
                nv.y = p[1];
                nv.z = p[2];
                nv.u = u;
                nv.v = t;
                // The colour as the GS takes a PACKED RGBAQ: the low 8 bits of each word (an alpha of 0x100 is 0,
                // which the PCRTC merge reads as "show the other circuit").
                uint32_t rgba = 0;
                for (uint32_t ch = 0; ch < 4u; ++ch)
                    rgba |= (static_cast<uint32_t>(mem.i(v + 2u, ch)) & 0xFFu) << (8u * ch);
                nv.rgba = rgba;
                nv.fog = fogOn ? static_cast<float>((fogW >> 4) & 0xFFu) / 255.0f : 1.0f;
                strip[0] = strip[1];
                strip[1] = strip[2];
                strip[2] = nv;
                if (k >= 2u && !adc)
                    native->triangles.insert(native->triangles.end(), strip, strip + 3);
            }
            if (packetWanted)
                put32(packet, static_cast<uint32_t>(ftoi(x * 16.0)), static_cast<uint32_t>(ftoi(y * 16.0)),
                      static_cast<uint32_t>(ftoi(z * 16.0)), w);
        }
    }
    return strips > 0;
}

namespace
{
    // The renderer's frame is built on the VIF1 worker (the objects) and ended on the EE thread (the frame wait).
    std::mutex g_nativeMutex;
    uint64_t g_nativeGroup = 0;
    bool g_nativeStarted = false;

    ps2x::native::Screen nativeScreen()
    {
        ps2x::native::Screen screen;
        // The microprogram writes FTOI4 Z into XYZF2, whose Z field drops the 4 fraction bits: one GS Z per unit of z/w
        // (16, as for XYZ2, made every near object the deepest Z of a Z24 buffer: drawn in stream order, over the UI
        // the game composes with a depth test).
        screen.gsZScale = 1.0f;
        return screen;
    }

    // Caller holds g_nativeMutex: the renderer draws in the GS stream, and a frame is being built.
    void ensureNativeStarted()
    {
        if (g_nativeStarted)
            return;
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        renderer.setStreamDraws(true);
        renderer.setEnabled(true);
        // VP2's objects take their textures where the GS keeps them (Material::fromGsContext): the renderer's own
        // texture cache would only copy every upload for nothing.
        ps2x::native::setUploadsWanted(false);
        renderer.beginFrame(nativeScreen());
        g_nativeStarted = true;
    }
}

bool vp2sNativeMeshEnabled()
{
    static const bool on = [] { const char *v = std::getenv("VP2S_NATIVE_MESH"); return v && v[0] == '1'; }();
    return on;
}

void vp2sNativeMeshFrameEnd()
{
    if (!vp2sNativeMeshEnabled())
        return;
    std::lock_guard<std::mutex> lock(g_nativeMutex);
    ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
    if (!g_nativeStarted)
    {
        ensureNativeStarted();
        return;
    }
    renderer.endFrame();
    renderer.beginFrame(nativeScreen());
}

namespace
{
    // VP2S_NATIVE_MESH_LOG: every 2 s, the VU1 programs seen: 0-7 objects sent as packets (by PRIM type), 8 drawn
    // natively, 9 culled whole, 10 another program (the VU1 runs it); the mesh program not handled here: 11 another
    // layout, 12 cull sum 3, 13 VI41.z set, 14 setup or lighting, 15 later.
    // The other VU1 programs by start PC since the last line (VP2S_NATIVE_MESH_LOG), most run first.
    std::mutex g_otherMutex;
    std::unordered_map<uint32_t, uint64_t> g_otherPrograms;

    void noteOtherProgram(uint32_t pc)
    {
        std::lock_guard<std::mutex> lock(g_otherMutex);
        ++g_otherPrograms[pc];
    }

    std::string otherProgramsLine()
    {
        std::lock_guard<std::mutex> lock(g_otherMutex);
        std::vector<std::pair<uint32_t, uint64_t>> programs(g_otherPrograms.begin(), g_otherPrograms.end());
        g_otherPrograms.clear();
        std::sort(programs.begin(), programs.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string line = " | other programs:";
        char buffer[32];
        for (size_t i = 0; i < programs.size() && i < 8u; ++i)
        {
            std::snprintf(buffer, sizeof(buffer), " %04x=%llu", programs[i].first, static_cast<unsigned long long>(programs[i].second));
            line += buffer;
        }
        return line;
    }

    void countHle(int kind)
    {
        static const bool s_on = std::getenv("VP2S_NATIVE_MESH_LOG") != nullptr;
        if (!s_on)
            return;
        static std::mutex s_mutex;
        static uint64_t s_counts[16] = {};
        static auto s_since = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(s_mutex);
        ++s_counts[kind];
        const auto now = std::chrono::steady_clock::now();
        if (now - s_since < std::chrono::seconds(2))
            return;
        std::string line = "[vp2s-native] 2 s:";
        for (uint64_t count : s_counts)
            line += " " + std::to_string(count);
        line += otherProgramsLine();
        std::fprintf(stderr, "%s (packets by PRIM type 0-7, native, culled, other program, not handled: layout, cull 3, vi41z, setup/lighting, later)\n", line.c_str());
        std::fill(std::begin(s_counts), std::end(s_counts), 0ull);
        s_since = now;
    }
}

bool vp2sVu1Hle(ps2x::native::Vu1HleRun &run)
{
    if (run.startPc != 0x0880u || !run.code || !run.data || !run.state || !run.kick)
    {
        static const bool s_log = std::getenv("VP2S_NATIVE_MESH_LOG") != nullptr;
        if (s_log)
            noteOtherProgram(run.startPc);
        countHle(10);
        return false;
    }
    thread_local std::vector<uint8_t> packet;
    thread_local Vp2sNativeMesh mesh;
    const bool native = vp2sNativeMeshEnabled() && run.nativeDraw;
    if (!vp2sBuildMeshPacket(run.code, run.data, *run.state, run.top, packet, native ? &mesh : nullptr))
    {
        countHle(g_rejected);
        return false;
    }
    static const bool s_log = std::getenv("VP2S_NATIVE_MESH_LOG") != nullptr;
    static int s_logged = 0;
    if (native && s_log && s_logged < 12)
    {
        ++s_logged;
        std::fprintf(stderr, "[vp2s-native] prim %#x, %zu vertices\n", mesh.prim, mesh.triangles.size());
    }
    if (native)
        countHle((mesh.prim & 7u) != 4u ? static_cast<int>(mesh.prim & 7u) : mesh.triangles.empty() ? 9 : 8);
    // Triangle strips only (PRIM type 4): anything else still goes to the GS as packets.
    if (!native || (mesh.prim & 7u) != 4u)
    {
        run.kick(run.user, packet.data(), static_cast<uint32_t>(packet.size()));
        return true;
    }
    ps2x::native::Material material;
    material.fromGsContext = true;
    material.textured = (mesh.prim >> 4) & 1u; // TME
    material.fog = (mesh.prim >> 5) & 1u;      // FGE
    material.alphaBlend = (mesh.prim >> 6) & 1u; // ABE
    material.context = static_cast<uint8_t>((mesh.prim >> 9) & 1u); // CTXT
    uint64_t group = 0;
    {
        std::lock_guard<std::mutex> lock(g_nativeMutex);
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        ensureNativeStarted();
        if (mesh.triangles.empty())
            return true; // everything culled: nothing to draw
        group = ++g_nativeGroup;
        renderer.beginGroup(group);
        renderer.draw(material, mesh.matrix, mesh.triangles.data(), static_cast<uint32_t>(mesh.triangles.size()));
    }
    run.nativeDraw(run.nativeUser, group, (mesh.prim >> 9) & 1u); // CTXT
    return true;
}
