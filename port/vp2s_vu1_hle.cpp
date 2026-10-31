#include "vp2s_vu1_hle.h"
#include "vp2s_vu1_programs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

    // VU1 data memory as the run sees it: reads go to the VU's own memory until a quadword is written, which is then
    // copied here first (copy on write: an object touches a few hundred quadwords of the 1024, and a run the native
    // code does not handle after all leaves the VU's memory as it was for the microprogram).
    struct Memory
    {
        const uint8_t *source = nullptr;
        std::array<uint8_t, kQwords * 16u> bytes{};
        std::array<uint64_t, kQwords / 64u> written{};

        void reset(const uint8_t *data)
        {
            source = data;
            written.fill(0u);
        }
        bool isWritten(uint32_t qw) const { return ((written[qw >> 6] >> (qw & 63u)) & 1u) != 0u; }
        uint8_t *at(uint32_t qw, uint32_t lane)
        {
            qw %= kQwords;
            if (!isWritten(qw))
            {
                std::memcpy(bytes.data() + qw * 16u, source + qw * 16u, 16u);
                written[qw >> 6] |= 1ull << (qw & 63u);
            }
            return bytes.data() + qw * 16u + lane * 4u;
        }
        const uint8_t *at(uint32_t qw, uint32_t lane) const
        {
            qw %= kQwords;
            return (isWritten(qw) ? bytes.data() : source) + qw * 16u + lane * 4u;
        }
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

    // The mesh program's call into its set-up dispatcher: BAL at 0x08a8, or (the forest build) IADDIU vi01 then JALR
    // vi15, vi01 at 0x08a8 / 0x08b0. 0: neither.
    uint32_t meshDispatcher(const uint8_t *code)
    {
        const uint32_t first = lower(code, 0x08a8u);
        if (((first >> 25) & 0x7Fu) == 0x21u)
            return static_cast<uint32_t>(0x08a8 + 8 + signedImm11(first) * 8);
        const uint32_t jalr = lower(code, 0x08b0u);
        if (((first >> 25) & 0x7Fu) == 0x08u && ((first >> 16) & 0x1Fu) == 1u && ((first >> 11) & 0x1Fu) == 0u &&
            ((jalr >> 25) & 0x7Fu) == 0x25u && ((jalr >> 16) & 0x1Fu) == 15u && ((jalr >> 11) & 0x1Fu) == 1u)
            return ((((first >> 21) & 0xFu) << 11) | (first & 0x7FFu)) * 8u;
        return 0u;
    }

    // The library build's layout, followed from its code: the call at 0x08a8 to the set-up dispatcher; there, the
    // IADDIU into vi09 is the set-up table and the IADDIU vi01, vi01, n give the lighting tables (D, the last, is
    // the one with mem[14].w == 0). Then whether lighting mode 1 clamps the fourth light (MAXx.xyzw) or not.
    struct Layout
    {
        bool valid = false;
        uint32_t setupTable = 0;
        uint32_t lightTableD = 0;
        bool clampFourthLight = true;
        bool mode3FullFourthLight = false;
    };

    Layout findLayout(const uint8_t *code)
    {
        Layout layout;
        const uint32_t dispatcher = meshDispatcher(code);
        if (dispatcher == 0u)
            return layout;
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
        // Mode 3: the diffuse directions scaled by header 12.y, the fourth light's too (MULy.xyzw vf16) or not (MULy.xyz,
        // then MULx.w: the fourth light by 12.x, not clamped and without specular).
        const uint32_t mode3 = (tableD + 6u) * 8u;
        const uint32_t branch3 = lower(code, mode3);
        if (((branch3 >> 25) & 0x7Fu) == 0x20u)
        {
            const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(mode3) + 8 + signedImm11(branch3) * 8);
            for (uint32_t pc = target; pc < target + 0x100u && pc < 0x4000u; pc += 8u)
            {
                const uint32_t up = upper(code, pc);
                if ((up & 0x3Fu) == 0x19u && ((up >> 6) & 0x1Fu) == 16u) // MULy vf16
                {
                    layout.mode3FullFourthLight = ((up >> 21) & 1u) != 0u;
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

    // The set-up routines (header 5.x..w with data at 4.x..w), in order. False for one not done yet. With the code
    // and its set-up table (findLayout), a routine whose table entry only returns (JR) does nothing.
    bool runSetup(Memory &mem, uint32_t top, const uint8_t *code = nullptr, uint32_t setupTable = 0u)
    {
        const uint32_t vertices = top + static_cast<uint32_t>(mem.i(top, 1));
        const int32_t count = mem.i(top + 2u, 1);
        for (int r = 0; r < 4; ++r)
        {
            const int32_t routine = mem.i(top + 5u, static_cast<uint32_t>(r));
            const uint32_t base = top + static_cast<uint32_t>(mem.i(top + 4u, static_cast<uint32_t>(r)));
            if (code && routine > 0 && ((lower(code, (setupTable + static_cast<uint32_t>(routine)) * 8u) >> 25) & 0x7Fu) == 0x24u)
                continue;
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

    float inverseLength(float x, float y, float z) // ERLENG
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        return length != 0.0f ? 1.0f / length : length;
    }

    // Lighting table C (mem[14].z, after table A), pass n (0, then 1 when mem[14].z is 2): a spot light at mem[28 + n],
    // direction mem[30 + n], colour mem[32 + n], mem[34 + n] its cone (.x cosine squared offset, .y scale) and
    // strength (.z, over the distance). Its colour times the light is added to each vertex's factor (.xyz). Mode 3
    // adds a specular part: the half vector of the light and the vector to vf08, weighed by header 11.z, squared
    // header 9.w times; header 11.y weighs the diffuse part. False for a case not done (modes 2, power <= 0).
    bool addSpotLight(const Memory &mem, const VU1State &state, uint32_t top, uint32_t vertices, int32_t count,
                      int32_t mode, uint32_t n, std::vector<Vec4> &factors)
    {
        const int32_t power = mem.i(top + 9u, 3);
        if (mode == 2 || (mode == 3 && power <= 0))
            return false;
        const Vec4 light = mem.vf(28u + n), direction = mem.vf(30u + n), colour = mem.vf(32u + n), cone = mem.vf(34u + n);
        const Vec4 weights = mem.vf(top + 11u);
        const float *eyePoint = state.vf[8];
        for (int32_t k = 0; k < count; ++k)
        {
            const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
            const Vec4 normal = mem.vf(v), p = mem.vf(v + 3u);
            const float toLight[3] = {light[0] - p[0], light[1] - p[1], light[2] - p[2]};
            const float inverse = inverseLength(toLight[0], toLight[1], toLight[2]);
            float unit[3];
            for (int c = 0; c < 3; ++c)
                unit[c] = toLight[c] * inverse;
            const float cosine = direction[0] * unit[0] + direction[1] * unit[1] + direction[2] * unit[2];
            // The microprogram tests the cosine's MAC flags (sign or zero x: FMAND 0x88): no light behind the cone.
            float spot = 0.0f;
            if (cosine > 0.0f)
                spot = std::min(std::max((cosine * cosine - cone[0]) * cone[1], 0.0f), 1.0f);
            const float strength = cone[2] * inverse;
            float diffuse = 0.0f;
            {
                float t[3];
                for (int c = 0; c < 3; ++c)
                    t[c] = unit[c] * (normal[c] * strength) * spot;
                diffuse = std::max(t[0] + t[1] + t[2], 0.0f);
            }
            if (mode != 3)
            {
                for (int c = 0; c < 3; ++c)
                    factors[k][c] = colour[c] * diffuse + factors[k][c];
                continue;
            }
            const float toEye[3] = {eyePoint[0] - p[0], eyePoint[1] - p[1], eyePoint[2] - p[2]};
            const float squared = toEye[0] * toEye[0] + toEye[1] * toEye[1] + toEye[2] * toEye[2];
            const float root = std::sqrt(std::fabs(squared));
            const float q = root != 0.0f ? 1.0f / root : std::numeric_limits<float>::max(); // RSQRT 1, |E|^2
            float half[3];
            for (int c = 0; c < 3; ++c)
                half[c] = (toEye[c] * q + unit[c]) * weights[2];
            float s = std::max(half[0] * normal[0] + half[1] * normal[1] + half[2] * normal[2], 0.0f);
            for (int32_t r = 0; r < power; ++r)
                s *= s;
            const float specular = (s * spot) * strength;
            diffuse *= weights[1];
            for (int c = 0; c < 3; ++c)
                factors[k][c] = (colour[c] * diffuse + colour[c] * specular) + factors[k][c];
        }
        return true;
    }

    bool computeFactors(const Memory &mem, const VU1State &state, uint32_t top, const Layout &layout, int32_t mode,
                        bool tableA, uint32_t vertices, int32_t count, std::vector<Vec4> &factors);

    // Lighting table B (mem[14].y, after table A; routine 0x3150, modes 0-2): three point lights at mem[20..22], colours
    // mem[23..25], strengths mem[27].xyz. Each adds colour * max(0, n . (light - p) * strength / |light - p|^2), the
    // dot raised to the power 2^(header 9.w), to the vertex's factor. In the microprogram's order (light 0 scaled by
    // its strength, then by ERSADD's 1 / |d|^2; lights 1 and 2 by strength / |d|^2): its colours to within one unit,
    // where the EFU's rounding differs.
    void addPointLights(const Memory &mem, uint32_t top, uint32_t vertices, int32_t count, std::vector<Vec4> &factors)
    {
        const Vec4 lights[3] = {mem.vf(20u), mem.vf(21u), mem.vf(22u)};
        const Vec4 colours[3] = {mem.vf(23u), mem.vf(24u), mem.vf(25u)};
        const Vec4 strength = mem.vf(27u);
        const int32_t power = mem.i(top + 9u, 3);
        for (int32_t k = 0; k < count; ++k)
        {
            const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
            const Vec4 n = mem.vf(v), p = mem.vf(v + 3u);
            float dots[3];
            for (int i = 0; i < 3; ++i)
            {
                const float d[3] = {lights[i][0] - p[0], lights[i][1] - p[1], lights[i][2] - p[2]};
                const float squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                float scaled[3];
                if (i == 0)
                {
                    const float inverse = squared != 0.0f ? 1.0f / squared : 0.0f; // ERSADD
                    for (int c = 0; c < 3; ++c)
                        scaled[c] = d[c] * strength[0] * inverse;
                }
                else
                {
                    const float q = squared != 0.0f ? strength[i] / squared : 0.0f;
                    for (int c = 0; c < 3; ++c)
                        scaled[c] = d[c] * q;
                }
                dots[i] = std::max(0.0f, n[0] * scaled[0] + n[1] * scaled[1] + n[2] * scaled[2]);
                for (int32_t s = 0; s < power; ++s)
                    dots[i] *= dots[i];
            }
            Vec4 &factor = factors[static_cast<size_t>(k)];
            for (int c = 0; c < 3; ++c)
                factor[c] += colours[0][c] * dots[0] + colours[1][c] * dots[1] + colours[2][c] * dots[2];
        }
    }

    // Vertex colours lit in place: each vertex's factor from lighting table D (mem[14].w == 0) or A, then table C's
    // spot lights, then colour times factor. False for a case not done yet.
    bool runLighting(Memory &mem, const VU1State &state, uint32_t top, const Layout &layout)
    {
        const int32_t mode = mem.i(top + 8u, 1);
        const int32_t count = mem.i(top + 2u, 1);
        const uint32_t vertices = top + static_cast<uint32_t>(mem.i(top, 1));
        thread_local std::vector<Vec4> factors;
        factors.assign(static_cast<size_t>(std::max(count, 0)), Vec4{});
        const bool tableA = mem.i(14u, 3) != 0;
        const int32_t spots = tableA ? mem.i(14u, 2) : 0;
        if (tableA)
        {
            // Lighting table A (mem[14].w != 0), then B (mem[14].y) and C (mem[14].z) when set. A's modes 1-3 are
            // table D's code; its mode 0 gives every vertex mem[13] as it is (no mem[14].x test).
            // Table B (point lights) is done for modes 0-2 (its routine 0x3150), not mode 3's (0x32a0, with specular).
            if ((mem.i(14u, 1) != 0 && mode == 3) || (mode != 0 && mode != 1 && mode != 3))
            {
                noteUnhandled("lighting mode/table (mem[14].w)", mode, mem.i(14u, 3) * 100 + mem.i(14u, 1) * 10 + mem.i(14u, 2));
                return false;
            }
        }
        if (!computeFactors(mem, state, top, layout, mode, tableA, vertices, count, factors))
            return false;
        if (tableA && mem.i(14u, 1) != 0)
            addPointLights(mem, top, vertices, count, factors);
        if (spots != 0)
        {
            if (!addSpotLight(mem, state, top, vertices, count, mode, 0u, factors) ||
                (spots == 2 && !addSpotLight(mem, state, top, vertices, count, mode, 1u, factors)))
            {
                noteUnhandled("lighting table C", mode, spots);
                return false;
            }
        }
        const Vec4 limit = mem.vf(0u);
        for (int32_t k = 0; k < count; ++k)
        {
            const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
            for (uint32_t c = 0; c < 4u; ++c)
            {
                const float lit = static_cast<float>(mem.i(v + 2u, c)) * factors[k][c];
                mem.setI(v + 2u, c, ftoi(std::max(0.0f, std::min(lit, limit[c]))));
            }
        }
        return true;
    }

    // Table A or D's factor for each vertex (modes 0, 1, 3).
    bool computeFactors(const Memory &mem, const VU1State &state, uint32_t top, const Layout &layout, int32_t mode,
                        bool tableA, uint32_t vertices, int32_t count, std::vector<Vec4> &factors)
    {
        auto apply = [&](uint32_t v, const Vec4 &factor) { factors[(v - vertices) / 4u] = factor; };
        if (mode == 0)
        {
            Vec4 scale = mem.vf(13u);
            if (!tableA && mem.i(14u, 0) == 0)
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
        if (mode == 3)
        {
            // 0x2fd8: the four lights of mode 1 (directions by component of vf05..vf07, colours mem[5..8]) for the
            // diffuse part, plus a specular part: per light, the normal against the light direction plus the unit
            // vector from the vertex to vf08, squared header 9.w times. Header 12 weighs them: .x the ambient
            // mem[19], .y the diffuse, .z the specular. Accumulated in the microprogram's order.
            const Vec4 d[3] = {{state.vf[5][0], state.vf[5][1], state.vf[5][2], state.vf[5][3]},
                               {state.vf[6][0], state.vf[6][1], state.vf[6][2], state.vf[6][3]},
                               {state.vf[7][0], state.vf[7][1], state.vf[7][2], state.vf[7][3]}};
            const Vec4 eyePoint = {state.vf[8][0], state.vf[8][1], state.vf[8][2], state.vf[8][3]};
            const Vec4 colours[4] = {mem.vf(5u), mem.vf(6u), mem.vf(7u), mem.vf(8u)};
            const Vec4 ambient = mem.vf(19u);
            const Vec4 weights = mem.vf(top + 12u);
            const int32_t power = mem.i(top + 9u, 3);
            const float alpha = mem.f(13u, 3);
            // VP2S_MODE3_VARIANT (diagnosis, to match the microprogram): 1 one more squaring, 2 one fewer, 3 specular
            // clamped at 0, 4 the previous vertex's position, 5 diffuse not clamped.
            static const int s_variant = [] { const char *v = std::getenv("VP2S_MODE3_VARIANT"); return v ? std::atoi(v) : 0; }();
            const int32_t squarings = s_variant == 1 ? power + 1 : s_variant == 2 ? std::max(power - 1, 0) : power;
            for (int32_t k = 0; k < count; ++k)
            {
                const uint32_t v = vertices + 4u * static_cast<uint32_t>(k);
                const Vec4 n = mem.vf(v);
                const Vec4 p = mem.vf((s_variant == 4 && k > 0 ? v - 4u : v) + 3u);
                Vec4 eye{};
                for (int c = 0; c < 3; ++c)
                    eye[c] = (eyePoint[c] - p[c]) * weights[2];
                const float length = std::sqrt((eyePoint[0] - p[0]) * (eyePoint[0] - p[0]) + (eyePoint[1] - p[1]) * (eyePoint[1] - p[1]) +
                                               (eyePoint[2] - p[2]) * (eyePoint[2] - p[2]));
                const float inverse = length != 0.0f ? 1.0f / length : 0.0f;
                float diffuse[4], specular[4];
                for (int i = 0; i < 4; ++i)
                {
                    // The fourth light (.w): in one library build like the others; in the other weighed by header
                    // 12.x instead of .y, not clamped, and without specular.
                    const bool plain = i == 3 && !layout.mode3FullFourthLight;
                    const float weight = plain ? weights[0] : weights[1];
                    diffuse[i] = (d[0][i] * weight) * n[0] + (d[1][i] * weight) * n[1] + (d[2][i] * weight) * n[2];
                    if (s_variant != 5 && !plain)
                        diffuse[i] = std::max(0.0f, diffuse[i]);
                    float s = (d[0][i] * weights[2] + eye[0] * inverse) * n[0] + (d[1][i] * weights[2] + eye[1] * inverse) * n[1] +
                              (d[2][i] * weights[2] + eye[2] * inverse) * n[2];
                    if (s_variant == 3)
                        s = std::max(0.0f, s);
                    for (int32_t r = 0; r < squarings; ++r)
                        s *= s;
                    specular[i] = plain ? 0.0f : s;
                }
                Vec4 factor{};
                for (int c = 0; c < 3; ++c)
                {
                    float acc = ambient[c] * weights[0];
                    acc += colours[0][c] * diffuse[0];
                    acc += colours[1][c] * diffuse[1];
                    acc += colours[2][c] * diffuse[2];
                    acc += colours[3][c] * diffuse[3];
                    acc += colours[0][c] * specular[0];
                    acc += colours[3][c] * specular[3];
                    acc += colours[1][c] * specular[1];
                    factor[c] = acc + colours[2][c] * specular[2];
                }
                factor[3] = alpha;
                static const bool s_mode3Log = std::getenv("VP2S_MODE3_LOG") != nullptr;
                static int s_mode3Logged = 0;
                if (s_mode3Log && s_mode3Logged < 40)
                {
                    ++s_mode3Logged;
                    std::printf("mode3 k=%d colour=%d,%d,%d weights=%g,%g,%g power=%d ambient=%g,%g,%g diffuse=%g,%g,%g,%g "
                                "specular=%g,%g,%g,%g factor=%g,%g,%g\n",
                                k, mem.i(v + 2u, 0), mem.i(v + 2u, 1), mem.i(v + 2u, 2), weights[0], weights[1], weights[2], power,
                                ambient[0], ambient[1], ambient[2], diffuse[0], diffuse[1], diffuse[2], diffuse[3], specular[0],
                                specular[1], specular[2], specular[3], factor[0], factor[1], factor[2]);
                }
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

int vp2sMeshRejection()
{
    return g_rejected;
}

bool vp2sBuildMeshPacket(const uint8_t *code, const uint8_t *data, const VU1State &state, uint32_t top,
                         std::vector<uint8_t> &packet, Vp2sNativeMesh *native)
{
    const Layout layout = findLayout(code);
    g_rejected = 11;
    if (!layout.valid)
        return false;
    static Memory mem; // one run at a time (the VIF1 thread)
    mem.reset(data);
    top %= kQwords;
    // The back-face cull: header 15.x selects it (0 culls), unless 15.x plus mem[41].z make 3, which culls too.
    const int32_t cullSum = mem.i(top + 15u, 0) + mem.i(41u, 2);
    const int32_t cullSelect = cullSum == 3 ? 0 : mem.i(top + 15u, 0);
    g_rejected = 14;
    if (!runSetup(mem, top, code, layout.setupTable) || !runLighting(mem, state, top, layout))
        return false;
    g_rejected = 15;

    const Mat4 screen = {Vec4{state.vf[1][0], state.vf[1][1], state.vf[1][2], state.vf[1][3]},
                         Vec4{state.vf[2][0], state.vf[2][1], state.vf[2][2], state.vf[2][3]},
                         Vec4{state.vf[3][0], state.vf[3][1], state.vf[3][2], state.vf[3][3]},
                         Vec4{state.vf[4][0], state.vf[4][1], state.vf[4][2], state.vf[4][3]}};
    const Mat4 clip = mem.matrix(85u);
    const bool culls = cullSelect == 0;
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
                // The microprogram tests the sign flag: a degenerate triangle (+0, VP2's padding strips) is culled too.
                adc = !std::signbit(cross * ((k % 2u) == 0u ? 1.0f : -1.0f));
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
        // VP2's objects take their textures from the renderer's texture cache, fed by the game's uploads; only the
        // comparison path (VP2S_NATIVE_MATERIAL=0, Material::fromGsContext) does without them.
        if (const char *v = std::getenv("VP2S_NATIVE_MATERIAL"); v && v[0] == '0')
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
    std::unordered_map<uint64_t, uint64_t> g_otherPrograms; // (code image hash << 32 | start), runs

    void noteOtherProgram(uint32_t pc, uint32_t image)
    {
        std::lock_guard<std::mutex> lock(g_otherMutex);
        ++g_otherPrograms[(static_cast<uint64_t>(image) << 32) | pc];
    }

    std::string otherProgramsLine()
    {
        std::lock_guard<std::mutex> lock(g_otherMutex);
        std::vector<std::pair<uint64_t, uint64_t>> programs(g_otherPrograms.begin(), g_otherPrograms.end());
        g_otherPrograms.clear();
        std::sort(programs.begin(), programs.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string line = " | other programs:";
        char buffer[48];
        for (size_t i = 0; i < programs.size() && i < 12u; ++i)
        {
            std::snprintf(buffer, sizeof(buffer), " %04x@%08x=%llu", static_cast<unsigned>(programs[i].first & 0xffffu),
                          static_cast<unsigned>(programs[i].first >> 32), static_cast<unsigned long long>(programs[i].second));
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
        static uint64_t s_counts[18] = {};
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
        std::fprintf(stderr, "%s (packets by PRIM type 0-7, native, culled, other program, not handled: layout, cull 3, vi41z, setup/lighting, later; kick set-up 0x678, object set-up 0x238; programs as C++)\n", line.c_str());
        std::fill(std::begin(s_counts), std::end(s_counts), 0ull);
        s_since = now;
    }
}

namespace
{
    uint64_t fnv(const uint8_t *bytes, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        return hash;
    }

    // A GIF packet's size in VU1 data memory from its tags, up to the EOP tag (0: not one within memory).
    uint32_t gifPacketBytes(const uint8_t *data, uint32_t qw)
    {
        uint32_t bytes = 0;
        for (int tags = 0; tags < 64; ++tags)
        {
            if (qw >= 1024u)
                return 0u;
            uint64_t tag = 0;
            std::memcpy(&tag, data + qw * 16u, 8u);
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            const uint32_t qwords = flg == 0u ? nloop * nreg : flg == 1u ? (nloop * nreg + 1u) / 2u : nloop;
            bytes += 16u + qwords * 16u;
            qw += 1u + qwords;
            if ((tag >> 15) & 1u)
                return qw <= 1024u ? bytes : 0u;
        }
        return 0u;
    }

    // The GS drawing registers VP2's mesh programs set in the packets they kick (the frame's header, an object's
    // prefix, the kick set-up between batches), per context: the native objects' materials come from them, not from
    // the GS. TEXA and FOGCOL, which the game sets in other packets, come from the texture cache's scan of the game's
    // packets (gameTexa, gameFogColour).
    struct DrawRegisters
    {
        uint64_t tex0 = 0, alpha = 0, test = 0, zbuf = 0, clamp = 0, fba = 0;
    };
    DrawRegisters g_drawRegisters[2]; // the VIF1 thread's (one microprogram at a time)

    void noteRegisterWrites(const uint8_t *packet, uint32_t bytes)
    {
        uint32_t off = 0;
        while (off + 16u <= bytes)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, packet + off, 8u);
            std::memcpy(&regs, packet + off + 8u, 8u);
            off += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), flg = static_cast<uint32_t>((tag >> 58) & 3u);
            const uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 15u) == 0u ? 16u : static_cast<uint32_t>((tag >> 60) & 15u);
            if (flg != 0u) // REGLIST / IMAGE: not in these packets
                return;
            for (uint32_t k = 0; k < nloop; ++k)
                for (uint32_t j = 0; j < nreg; ++j)
                {
                    const uint32_t at = off + 16u * (k * nreg + j);
                    if (((regs >> (4u * j)) & 15u) != 0xEu || at + 16u > bytes) // A+D only
                        continue;
                    uint64_t data, address;
                    std::memcpy(&data, packet + at, 8u);
                    std::memcpy(&address, packet + at + 8u, 8u);
                    switch (address & 0xFFu)
                    {
                    case 0x06: g_drawRegisters[0].tex0 = data; break;
                    case 0x07: g_drawRegisters[1].tex0 = data; break;
                    case 0x08: g_drawRegisters[0].clamp = data; break;
                    case 0x09: g_drawRegisters[1].clamp = data; break;
                    case 0x42: g_drawRegisters[0].alpha = data; break;
                    case 0x43: g_drawRegisters[1].alpha = data; break;
                    case 0x47: g_drawRegisters[0].test = data; break;
                    case 0x48: g_drawRegisters[1].test = data; break;
                    case 0x4A: g_drawRegisters[0].fba = data; break;
                    case 0x4B: g_drawRegisters[1].fba = data; break;
                    case 0x4E: g_drawRegisters[0].zbuf = data; break;
                    case 0x4F: g_drawRegisters[1].zbuf = data; break;
                    default: break;
                    }
                }
            off += nloop * nreg * 16u;
            if ((tag >> 15) & 1u) // EOP
                return;
        }
    }

    // An object's material from its strips' PRIM and the registers its packets set.
    ps2x::native::Material nativeMaterial(uint32_t prim)
    {
        ps2x::native::Material m;
        const DrawRegisters &r = g_drawRegisters[(prim >> 9) & 1u]; // CTXT
        m.textured = (prim >> 4) & 1u;   // TME
        m.fog = (prim >> 5) & 1u;        // FGE
        m.alphaBlend = (prim >> 6) & 1u; // ABE
        m.tex0 = r.tex0;
        m.blend = m.alphaBlend ? ps2x::native::blendFromAlpha(r.alpha) : ps2x::native::Material::Blend::Opaque;
        if (m.alphaBlend)
        {
            // The named blends cover (Cs - Cd) As + Cd, Cs As + Cd, Cd - Cs As, Cd (1 - As), Cd (1 + As) and D alone;
            // any other (A - B) C + D (VP2's light grids: Cd As + Cs) as the GS blend itself.
            const uint32_t a = r.alpha & 3u, b = (r.alpha >> 2) & 3u, c = (r.alpha >> 4) & 3u, d = (r.alpha >> 6) & 3u;
            const bool named = a == b || (c == 0u && d == 1u && ((a == 0u && (b == 1u || b == 2u)) || (a == 2u && (b == 0u || b == 1u)) ||
                                                                 (a == 1u && b == 2u)));
            if (!named)
            {
                m.blend = ps2x::native::Material::Blend::Gs;
                m.gsAlpha = static_cast<uint8_t>(r.alpha & 0xFFu);
                m.gsFix = static_cast<uint8_t>((r.alpha >> 32) & 0xFFu);
            }
        }
        const bool ate = r.test & 1u;
        const uint32_t atst = static_cast<uint32_t>((r.test >> 1) & 7u), aref = static_cast<uint32_t>((r.test >> 4) & 0xFFu);
        m.alphaTest = ate && (atst == 5u || atst == 6u); // GEQUAL / GREATER (VP2's meshes: GREATER 0)
        m.alphaRef = static_cast<uint8_t>(atst == 5u && aref > 0u ? aref - 1u : aref);
        const bool zte = (r.test >> 16) & 1u;
        m.depthTest = zte && ((r.test >> 17) & 3u) != 1u; // ZTST ALWAYS: no test
        m.depthWrite = ((r.zbuf >> 32) & 1u) == 0u;      // ZMSK
        m.clampU = (r.clamp & 3u) == 1u;
        m.clampV = ((r.clamp >> 2) & 3u) == 1u;
        m.clamp = r.clamp; // VP2's atlases: REGION_CLAMP
        m.fba = (r.fba & 1u) != 0u;
        // TEXA and FOGCOL as the game wrote them in its own packets (the texture cache reads them there), not the GS's.
        m.texa = ps2x::native::textures().gameTexa();
        m.fogColour = ps2x::native::textures().gameFogColour();
        return m;
    }

    // Program 0x0238 (every known build, code 0x0000..0x0678 checked by its hash): the per-object set-up the mesh
    // programs read, from the object matrix O (mem[9..12], rows), the camera V (mem[48..51]), the screen projection P
    // (mem[52..55]), the clip matrix C (mem[56..59]) and the frame's four light directions L (mem[1..4].xyz):
    //   vf01..vf04 = the columns of P^T V O (object to screen); mem[77..80] = (V O)^T, mem[85..88] = (V O)^T C
    //   (rows); mem[89..91] = the 3x3 of V O transposed (w 0), mem[92] = the last row of (V O)^T P;
    //   mem[73..75] = Rn^T, Rn the rows of O's rotation normalised;
    //   vf05..vf07 = Rn^T L^T (the lights in object space, one component per register); vf08 = vf13 = the eye in
    //   object space, -(R^T t) of V O, and mem[72].xyz its direction; mem[37] = Rn^T times the shadow light's
    //   direction (or, with lighting table A and mem[40].w, its position through O's rigid inverse);
    //   with table A (mem[14].w): the point and spot light positions mem[20..22], mem[28..29] through O's rigid
    //   inverse (R^T, -R^T t times their w), the spot directions mem[30..31] by Rn^T;
    //   mem[81].z = 0x380, .w = mem[36].y, mem[47].x = 0x8000 (an empty GIF packet it kicks), mem[1023].w = 0,
    //   vf18 = 0, vf31.xyz = 1.
    // Checked against recorded runs (vp2s_hle_check): the same rows and registers, to float rounding.
    bool runObjectSetup(ps2x::native::Vu1HleRun &run)
    {
        if (fnv(run.code, 0x678u) != 0x1fb529560d4ac694ull)
            return false;
        uint8_t *data = run.data;
        VU1State &state = *run.state;
        const auto row = [&](uint32_t r) {
            Vec4 v;
            std::memcpy(v.data(), data + r * 16u, 16u);
            return v;
        };
        const auto setRow = [&](uint32_t r, const Vec4 &v) { std::memcpy(data + r * 16u, v.data(), 16u); };
        const auto rowI = [&](uint32_t r, uint32_t lane) {
            int32_t v;
            std::memcpy(&v, data + r * 16u + lane * 4u, 4u);
            return v;
        };
        const auto setRowI = [&](uint32_t r, uint32_t lane, int32_t v) { std::memcpy(data + r * 16u + lane * 4u, &v, 4u); };
        const auto block = [&](uint32_t r) { return Mat4{row(r), row(r + 1u), row(r + 2u), row(r + 3u)}; };
        const auto mul = [](const Mat4 &a, const Mat4 &b) {
            Mat4 m{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    m[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
            return m;
        };
        const auto transpose = [](const Mat4 &a) {
            Mat4 m{};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    m[i][j] = a[j][i];
            return m;
        };
        const Mat4 o = block(9u), v = block(48u), p = block(52u), c = block(56u);
        const Mat4 vo = mul(v, o), voT = transpose(vo);
        const Mat4 screen = mul(transpose(p), vo);
        for (int k = 0; k < 4; ++k)
            for (int r = 0; r < 4; ++r)
                state.vf[1 + k][r] = screen[r][k];
        const Mat4 voTc = mul(voT, c), voTp = mul(voT, p);
        for (uint32_t i = 0; i < 4u; ++i)
        {
            setRow(77u + i, voT[i]);
            setRow(85u + i, voTc[i]);
        }
        for (uint32_t i = 0; i < 3u; ++i)
            setRow(89u + i, Vec4{vo[0][i], vo[1][i], vo[2][i], 0.0f});
        setRow(92u, voTp[3]);
        // Rn: O's rotation rows normalised.
        float rn[3][3];
        for (int i = 0; i < 3; ++i)
        {
            const float length = std::sqrt(o[i][0] * o[i][0] + o[i][1] * o[i][1] + o[i][2] * o[i][2]);
            const float inverse = length != 0.0f ? 1.0f / length : 0.0f;
            for (int j = 0; j < 3; ++j)
                rn[i][j] = o[i][j] * inverse;
        }
        const auto byRnT = [&](const Vec4 &d) {
            return Vec4{rn[0][0] * d[0] + rn[1][0] * d[1] + rn[2][0] * d[2], rn[0][1] * d[0] + rn[1][1] * d[1] + rn[2][1] * d[2],
                        rn[0][2] * d[0] + rn[1][2] * d[1] + rn[2][2] * d[2], 0.0f};
        };
        for (uint32_t i = 0; i < 3u; ++i)
            setRow(73u + i, Vec4{rn[0][i], rn[1][i], rn[2][i], 0.0f});
        for (uint32_t light = 0; light < 4u; ++light)
        {
            const Vec4 d = byRnT(row(1u + light));
            for (int comp = 0; comp < 3; ++comp)
                state.vf[5 + comp][light] = d[comp];
        }
        // The eye in object space: the rigid inverse of V O applied to the origin.
        Vec4 eye{};
        for (int j = 0; j < 3; ++j)
            eye[j] = -(vo[0][j] * vo[0][3] + vo[1][j] * vo[1][3] + vo[2][j] * vo[2][3]);
        std::memcpy(state.vf[8], eye.data(), 16u);
        std::memcpy(state.vf[13], eye.data(), 16u);
        {
            const float length = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
            const float inverse = length != 0.0f ? 1.0f / length : 0.0f;
            setRow(72u, Vec4{eye[0] * inverse, eye[1] * inverse, eye[2] * inverse, 0.0f});
        }
        // O's rigid inverse (R^T, -R^T t scaled by the point's w), for light positions.
        const auto rigidInverse = [&](const Vec4 &x) {
            Vec4 out{};
            for (int j = 0; j < 3; ++j)
                out[j] = o[0][j] * (x[0] - o[0][3] * x[3]) + o[1][j] * (x[1] - o[1][3] * x[3]) + o[2][j] * (x[2] - o[2][3] * x[3]);
            out[3] = x[3];
            return out;
        };
        const bool tableA = rowI(14u, 3) != 0;
        if (tableA && rowI(40u, 3) != 0)
            setRow(37u, rigidInverse(row(37u)));
        else
            setRow(37u, byRnT(row(37u)));
        if (tableA)
        {
            for (uint32_t r : {20u, 21u, 22u, 28u, 29u})
                setRow(r, rigidInverse(row(r)));
            for (uint32_t r : {30u, 31u})
                setRow(r, byRnT(row(r)));
        }
        setRowI(81u, 2, 0x380);
        setRowI(81u, 3, rowI(36u, 1));
        setRowI(1023u, 3, 0);
        std::memset(state.vf[18], 0, 16u);
        state.vf[31][0] = state.vf[31][1] = state.vf[31][2] = 1.0f;
        setRowI(47u, 0, 0x8000);
        run.kick(run.user, data + 47u * 16u, 16u);
        return true;
    }

    // Program 0x06f0 (one build, code 0x06f0..0x0f30 checked by its hash): an object's shadow, which the microprogram
    // draws as a shadow volume counted in the frame's alpha (the CLUT trick). Natively: the object's triangles after
    // its set-up passes (the same ones as the meshes'), each extruded along the light (mem[37], object space) down to
    // the object's ground plane (y = 0) into a closed prism; the union of the prisms is the object's shadow volume,
    // which the native renderer counts and darkens the frame in once (Material::Blend::ShadowVolume), by the 0x40
    // alpha of the game's darkening sprites. Drawn with the object's screen matrix, mem[896..899] (its columns), whose
    // flattened copy along the same light the microprogram finds at mem[5..8].
    bool buildShadowVolume(const uint8_t *code, const uint8_t *data, uint32_t top, std::vector<ps2x::native::Vertex> &out,
                           float matrix[16])
    {
        if (fnv(code + 0x6f0u, 0x840u) != 0xd42b1eba408869a3ull)
            return false;
        static Memory mem;
        mem.reset(data);
        top %= kQwords;
        if (!runSetup(mem, top))
            return false;
        const Vec4 light = mem.vf(37u);
        if (std::fabs(light[1]) < 1e-3f)
            return false; // light along the ground: no extrusion to it
        for (uint32_t c = 0; c < 4u; ++c)
            for (uint32_t r = 0; r < 4u; ++r)
                matrix[4u * c + r] = mem.f(896u + c, r);
        const int32_t strips = mem.i(top, 2);
        const uint32_t lengths = top + static_cast<uint32_t>(mem.i(top, 3));
        uint32_t v = top + static_cast<uint32_t>(mem.i(top, 1));
        out.clear();
        using P3 = std::array<float, 3>;
        const auto sub = [](const P3 &a, const P3 &b) { return P3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        const auto cross = [](const P3 &a, const P3 &b) {
            return P3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const auto dot = [](const P3 &a, const P3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        // Down the light well past the object's ground plane: the PS2's volume darkens what lies under the object along
        // the light (PCSX2: Alicia's shadow on the rocks below her), not only what is between her and y = 0.
        const float length = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        const float reach = length > 0.0f ? 3000.0f / length : 0.0f;
        const auto flat = [&](const P3 &p) {
            const float t = std::min(0.0f, p[1] / light[1]) - reach; // p - light * t moves down the light
            return P3{p[0] - light[0] * t, p[1] - light[1] * t, p[2] - light[2] * t};
        };
        const auto emit = [&](const P3 &a, const P3 &b, const P3 &c, const P3 &inside) {
            // Each face outward: away from the prism's centre.
            const P3 n = cross(sub(b, a), sub(c, a));
            const P3 centre = {(a[0] + b[0] + c[0]) / 3.0f, (a[1] + b[1] + c[1]) / 3.0f, (a[2] + b[2] + c[2]) / 3.0f};
            const bool flip = dot(n, sub(centre, inside)) < 0.0f;
            for (const P3 *p : {&a, flip ? &c : &b, flip ? &b : &c})
            {
                ps2x::native::Vertex vertex;
                vertex.x = (*p)[0];
                vertex.y = (*p)[1];
                vertex.z = (*p)[2];
                vertex.rgba = 0x40000000u; // the darkening sprites' alpha
                out.push_back(vertex);
            }
        };
        for (int32_t s = 0; s < strips; ++s)
        {
            const uint32_t count = static_cast<uint32_t>(mem.i(lengths + static_cast<uint32_t>(s), 0)) & 0xFFFFu;
            for (uint32_t k = 0; k < count; ++k, v += 4u)
            {
                if (k < 2u)
                    continue;
                const P3 a = {mem.f(v - 5u, 0), mem.f(v - 5u, 1), mem.f(v - 5u, 2)};
                const P3 b = {mem.f(v - 1u, 0), mem.f(v - 1u, 1), mem.f(v - 1u, 2)};
                const P3 c = {mem.f(v + 3u, 0), mem.f(v + 3u, 1), mem.f(v + 3u, 2)};
                const P3 n = cross(sub(b, a), sub(c, a));
                if (dot(n, n) < 1e-12f) // the strips' padding
                    continue;
                const P3 a2 = flat(a), b2 = flat(b), c2 = flat(c);
                const P3 inside = {(a[0] + b[0] + c[0] + a2[0] + b2[0] + c2[0]) / 6.0f, (a[1] + b[1] + c[1] + a2[1] + b2[1] + c2[1]) / 6.0f,
                                   (a[2] + b[2] + c[2] + a2[2] + b2[2] + c2[2]) / 6.0f};
                emit(a, b, c, inside);
                emit(a2, b2, c2, inside);
                emit(a, b, b2, inside);
                emit(a, b2, a2, inside);
                emit(b, c, c2, inside);
                emit(b, c2, b2, inside);
                emit(c, a, a2, inside);
                emit(c, a2, c2, inside);
            }
        }
        return !out.empty();
    }

    bool runShadow(ps2x::native::Vu1HleRun &run)
    {
        thread_local std::vector<ps2x::native::Vertex> volume;
        float matrix[16];
        if (!vp2sNativeMeshEnabled() || !run.nativeDraw || !buildShadowVolume(run.code, run.data, run.top, volume, matrix))
            return false;
        ps2x::native::Material material;
        material.textured = false;
        material.blend = ps2x::native::Material::Blend::ShadowVolume;
        material.depthWrite = false;
        uint64_t group = 0;
        {
            std::lock_guard<std::mutex> lock(g_nativeMutex);
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            ensureNativeStarted();
            group = ++g_nativeGroup;
            renderer.beginGroup(group);
            renderer.draw(material, matrix, volume.data(), static_cast<uint32_t>(volume.size()));
        }
        run.nativeDraw(run.nativeUser, group, 0);
        return true;
    }

    // Program 0x0678 (every known build, checked by its code's hash): copies mem[96..100] to 0x3aa and kicks the
    // GS packet there (a register set-up between object batches), leaving vf01..vf05 = mem[96..100], vi01 = 0x3aa.
    bool runKickSetup(ps2x::native::Vu1HleRun &run)
    {
        constexpr uint64_t kCodeHash = 0x124eb493c8720819ull; // code 0x678..0x6e0
        if (fnv(run.code + 0x678u, 0x68u) != kCodeHash)
            return false;
        constexpr uint32_t kTo = 0x3aau;
        std::memmove(run.data + kTo * 16u, run.data + 96u * 16u, 5u * 16u);
        for (uint32_t r = 0; r < 5u; ++r)
            std::memcpy(run.state->vf[1 + r], run.data + (96u + r) * 16u, 16u);
        run.state->vi[1] = static_cast<int32_t>(kTo);
        const uint32_t bytes = gifPacketBytes(run.data, kTo);
        if (bytes != 0u)
        {
            noteRegisterWrites(run.data + kTo * 16u, bytes);
            run.kick(run.user, run.data + kTo * 16u, bytes);
        }
        return true;
    }
}

bool vp2sMeshHeader(const uint8_t *code, const uint8_t *data, std::vector<uint8_t> &header)
{
    // 0x06e0..0x0770: kicks mem[69] (a GIF tag) and mem[17..18] (its A+D register writes), then branches into 0x0770's
    // code past its own header reads.
    if (fnv(code + 0x6e0u, 0x90u) != 0x6be0046a54d4ce47ull) // the other programs builds load at 0x06e0 differ throughout
        return false;
    header.resize(3u * 16u);
    std::memcpy(header.data(), data + 69u * 16u, 16u);
    std::memcpy(header.data() + 16u, data + 17u * 16u, 32u);
    return true;
}

bool vp2sMeshPrefix(const uint8_t *code, const uint8_t *data, uint32_t top, std::vector<uint8_t> &prefix)
{
    // 0x0770..0x0880 but the pair at 0x07a0 (IADDIU vi01 to the set-up dispatcher, which differs by build), checked
    // against the dispatcher the mesh program's BAL at 0x08a8 reaches.
    std::array<uint8_t, 0x110u> bytes;
    std::memcpy(bytes.data(), code + 0x770u, bytes.size());
    std::memset(bytes.data() + 0x30u, 0, 8u);
    // The second build (forest, dungeons) differs only by the branch at 0x0870 into the vertex loop, which sits 16 bytes
    // further on.
    const uint64_t hash = fnv(bytes.data(), bytes.size());
    if (hash != 0x633d036a69d5ea3aull && hash != 0x8ae32d0e3bb604b6ull)
        return false;
    const uint32_t dispatcher = meshDispatcher(code);
    if (dispatcher == 0u)
        return false;
    const uint32_t iaddiu = lower(code, 0x07a0u);
    if ((((iaddiu >> 21) & 0xFu) << 11 | (iaddiu & 0x7FFu)) * 8u != dispatcher)
        return false;
    const auto word = [&](uint32_t qw, uint32_t lane) {
        int32_t value;
        std::memcpy(&value, data + (qw % 1024u) * 16u + lane * 4u, 4u);
        return value;
    };
    top %= 1024u;
    const uint32_t third = (word(15u, 1) | word(top + 8u, 3)) != 0 ? 66u : 65u;
    const uint32_t from[5] = {top + 7u, top + 8u, third, top + 9u, top + 10u};
    prefix.resize(5u * 16u);
    for (uint32_t q = 0; q < 5u; ++q)
        std::memcpy(prefix.data() + q * 16u, data + (from[q] % 1024u) * 16u, 16u);
    return true;
}

namespace
{
    // A run no native handler takes: the program as C++ (vp2s_vu1_programs.h) when its image was lifted, in place of
    // VU1. VP2S_VU1_IMAGES=<dir>: the images that have none saved there (<image>.bin, and <image>.txt with the start
    // pcs run) for tools/vu1_native.py. VP2S_VU1_PROGRAMS=0: VU1 runs them all (to compare).
    // A packet a program as C++ kicked, drawn by the native renderer: its triangles, strips, fans and sprites (PACKED,
    // RGBAQ / ST / UV / XYZ2 / XYZF2 / XYZ3 / XYZF3 / PRIM / A+D) in the GS's window coordinates, with the material
    // from the registers the game wrote in its packets (the texture cache's scan) and in this one. The packet's
    // register writes still go to the GS as a packet of their own, for the draws not native yet. False (the caller
    // kicks it whole): something else (points, lines, REGLIST / IMAGE, an unknown register), or VP2S_NATIVE_PROGRAMS=0.
    bool drawPacketNatively(ps2x::native::Vu1HleRun &run, const std::vector<uint8_t> &packet)
    {
        static const bool s_on = [] { const char *v = std::getenv("VP2S_NATIVE_PROGRAMS"); return !(v && v[0] == '0'); }();
        if (!s_on || !run.nativeDraw || !vp2sNativeMeshEnabled())
            return false;
        // VP2S_NATIVE_PROGRAMS_PCS=<pc>,<pc>... (diagnosis): only those programs' packets.
        static const std::vector<uint32_t> s_pcs = [] {
            std::vector<uint32_t> pcs;
            if (const char *v = std::getenv("VP2S_NATIVE_PROGRAMS_PCS"))
                for (const char *p = v; *p;)
                {
                    char *end = nullptr;
                    pcs.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 16)));
                    p = *end ? end + 1 : end;
                }
            return pcs;
        }();
        if (!s_pcs.empty() && std::find(s_pcs.begin(), s_pcs.end(), run.startPc) == s_pcs.end())
            return false;
        auto &cache = ps2x::native::textures();
        const std::array<uint64_t, 0x80> game = cache.gameRegisters();
        DrawRegisters regs[2];
        for (uint32_t c = 0; c < 2u; ++c)
        {
            regs[c].tex0 = game[0x06u + c];
            regs[c].clamp = game[0x08u + c];
            regs[c].alpha = game[0x42u + c];
            regs[c].test = game[0x47u + c];
            regs[c].fba = game[0x4Au + c];
            regs[c].zbuf = game[0x4Eu + c];
        }
        uint64_t prim = game[0x00u];
        const uint64_t prmodecont = game[0x1Au];
        if ((prmodecont & 1u) == 0u)
            return false; // PRMODE supplies the attributes: not seen in VP2's programs
        struct Batch
        {
            ps2x::native::Material material;
            std::vector<ps2x::native::Vertex> triangles;
            uint32_t context = 0;
            uint64_t key = 0; // what the material was made of
        };
        thread_local std::vector<Batch> batches;
        batches.clear();
        thread_local std::vector<uint8_t> registerWrites;
        registerWrites.clear();
        const auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, packet.data() + at, 4u); return v; };
        const auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, packet.data() + at, 8u); return v; };
        const auto f32 = [&](size_t at) { float v; std::memcpy(&v, packet.data() + at, 4u); return v; };

        ps2x::native::Vertex queue[3], centre;
        uint32_t queued = 0, kicks = 0;
        uint32_t rgba = 0x80808080u;
        float s = 0.0f, t = 0.0f, q = 1.0f;
        uint32_t uvU = 0, uvV = 0;
        bool haveUv = false;
        const auto texSize = [&](uint64_t tex0, uint32_t shift) {
            return static_cast<float>(1u << std::min<uint32_t>(10u, static_cast<uint32_t>((tex0 >> shift) & 0xFu)));
        };
        const auto startPrim = [&]() { queued = 0; kicks = 0; };
        const auto batchFor = [&](uint64_t p) -> Batch & {
            const uint32_t ctxt = static_cast<uint32_t>((p >> 9) & 1u);
            DrawRegisters &saved = g_drawRegisters[ctxt];
            const DrawRegisters old = saved;
            saved = regs[ctxt];
            const DrawRegisters &r = regs[ctxt];
            const uint64_t key = fnv(reinterpret_cast<const uint8_t *>(&r), sizeof(r)) ^ ((p & 0x7F8u) * 0x9E3779B97F4A7C15ull) ^ ctxt;
            if (!batches.empty() && batches.back().key == key)
            {
                saved = old;
                return batches.back();
            }
            ps2x::native::Material m = nativeMaterial(static_cast<uint32_t>(p));
            saved = old;
            batches.push_back({m, {}, ctxt, key});
            return batches.back();
        };
        const auto vertex = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t fog, bool drawing) {
            ps2x::native::Vertex v;
            v.x = static_cast<float>(x & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(y & 0xFFFFu) / 16.0f;
            v.z = static_cast<float>(z); // the GS's Z, as the native meshes give it
            v.rgba = rgba;
            v.fog = static_cast<float>(fog) / 255.0f;
            const bool fst = (prim >> 8) & 1u;
            const uint64_t tex0 = regs[(prim >> 9) & 1u].tex0;
            if (fst && haveUv)
            {
                v.u = static_cast<float>(uvU) / 16.0f / texSize(tex0, 26);
                v.v = static_cast<float>(uvV) / 16.0f / texSize(tex0, 30);
            }
            else
            {
                v.u = q != 0.0f ? s / q : s;
                v.v = q != 0.0f ? t / q : t;
            }
            const uint32_t type = static_cast<uint32_t>(prim & 7u);
            const bool flat = ((prim >> 3) & 1u) == 0u; // IIP 0: the last vertex's colour
            if (type == 6u) // sprite: two corners
            {
                queue[queued++ % 2u] = v;
                if (queued >= 2u && drawing)
                {
                    const ps2x::native::Vertex &a = queue[(queued - 2u) % 2u], &b = queue[(queued - 1u) % 2u];
                    ps2x::native::Vertex c00 = b, c10 = b, c01 = b, c11 = b;
                    c00.x = a.x, c00.y = a.y, c00.u = a.u, c00.v = a.v;
                    c10.y = a.y, c10.v = a.v;
                    c01.x = a.x, c01.u = a.u;
                    Batch &batch = batchFor(prim);
                    batch.triangles.insert(batch.triangles.end(), {c00, c10, c01, c10, c11, c01});
                    queued = 0;
                }
                else if (queued >= 2u)
                    queued = 0;
                return;
            }
            queue[0] = queue[1];
            queue[1] = queue[2];
            queue[2] = v;
            ++kicks;
            const bool fan = type == 5u;
            if (fan && kicks == 1u)
                centre = v; // the fan's first vertex stays

            if (type == 3u && kicks % 3u != 0u)
                return; // a triangle list draws every third vertex
            if (kicks < 3u || !drawing)
                return;
            ps2x::native::Vertex tri[3] = {fan ? centre : queue[0], queue[1], queue[2]};
            if (flat)
                tri[0].rgba = tri[1].rgba = tri[2].rgba;
            Batch &batch = batchFor(prim);
            batch.triangles.insert(batch.triangles.end(), tri, tri + 3);
        };

        size_t at = 0;
        while (at + 16u <= packet.size())
        {
            const uint64_t tag = u64(at), regList = u64(at + 8u);
            at += 16u;
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), flg = static_cast<uint32_t>((tag >> 58) & 3u);
            const uint32_t nreg = ((tag >> 60) & 15u) == 0u ? 16u : static_cast<uint32_t>((tag >> 60) & 15u);
            if (flg != 0u)
                return false;
            if ((tag >> 46) & 1u) // PRE
            {
                prim = (tag >> 47) & 0x7FFu;
                startPrim();
            }
            for (uint32_t n = 0; n < nloop; ++n)
                for (uint32_t r = 0; r < nreg; ++r, at += 16u)
                {
                    if (at + 16u > packet.size())
                        return false;
                    switch ((regList >> (4u * r)) & 15u)
                    {
                    case 0x0: prim = u64(at) & 0x7FFu; startPrim(); break;
                    case 0x1: rgba = (u32(at) & 0xFFu) | ((u32(at + 4u) & 0xFFu) << 8) | ((u32(at + 8u) & 0xFFu) << 16) |
                                     ((u32(at + 12u) & 0xFFu) << 24); break;
                    case 0x2: s = f32(at), t = f32(at + 4u), q = f32(at + 8u); break;
                    case 0x3: uvU = u32(at) & 0x3FFFu, uvV = u32(at + 4u) & 0x3FFFu, haveUv = true; break;
                    case 0x4: vertex(u32(at), u32(at + 4u), (u32(at + 8u) >> 4) & 0xFFFFFFu, (u32(at + 12u) >> 4) & 0xFFu,
                                     ((u32(at + 12u) >> 15) & 1u) == 0u); break;
                    case 0x5: vertex(u32(at), u32(at + 4u), u32(at + 8u), 255u, ((u32(at + 12u) >> 15) & 1u) == 0u); break;
                    case 0xC: vertex(u32(at), u32(at + 4u), (u32(at + 8u) >> 4) & 0xFFFFFFu, (u32(at + 12u) >> 4) & 0xFFu, false); break;
                    case 0xD: vertex(u32(at), u32(at + 4u), u32(at + 8u), 255u, false); break;
                    case 0xF: break; // NOP
                    case 0xE:
                    {
                        const uint64_t data = u64(at);
                        const uint32_t address = static_cast<uint32_t>(u64(at + 8u) & 0xFFu);
                        switch (address)
                        {
                        case 0x00: prim = data & 0x7FFu; startPrim(); break;
                        case 0x06: case 0x07: regs[address - 0x06u].tex0 = data; break;
                        case 0x08: case 0x09: regs[address - 0x08u].clamp = data; break;
                        case 0x42: case 0x43: regs[address - 0x42u].alpha = data; break;
                        case 0x47: case 0x48: regs[address - 0x47u].test = data; break;
                        case 0x4A: case 0x4B: regs[address - 0x4Au].fba = data; break;
                        case 0x4E: case 0x4F: regs[address - 0x4Eu].zbuf = data; break;
                        case 0x14: case 0x15: case 0x3B: case 0x3D: case 0x3F: case 0x1A: case 0x18: case 0x19:
                        case 0x40: case 0x41: case 0x4C: case 0x4D: case 0x44: case 0x45: case 0x46: case 0x49:
                            break; // passed on to the GS below; TEX1 / TEXA / FOGCOL / TEXFLUSH / PRMODECONT / XYOFFSET /
                                   // SCISSOR / FRAME / DIMX / DTHE / COLCLAMP / PABE as the GS keeps them
                        default:
                            return false;
                        }
                        if (address == 0x1Au && (data & 1u) == 0u)
                            return false;
                        registerWrites.insert(registerWrites.end(), packet.begin() + static_cast<std::ptrdiff_t>(at),
                                              packet.begin() + static_cast<std::ptrdiff_t>(at + 16u));
                        break;
                    }
                    default:
                        return false;
                    }
                    const uint32_t type = static_cast<uint32_t>(prim & 7u);
                    if (type < 3u || type == 7u)
                        return false; // points, lines
                }
            if ((tag >> 15) & 1u)
                break;
        }

        // The register writes for the GS (its other draws), then the triangles where the packet would have gone.
        if (!registerWrites.empty())
        {
            thread_local std::vector<uint8_t> writes;
            const uint64_t adTag = (registerWrites.size() / 16u) | (uint64_t{1} << 15) | (uint64_t{1} << 60);
            const uint64_t adRegs = 0xEu;
            writes.resize(16u + registerWrites.size());
            std::memcpy(writes.data(), &adTag, 8u);
            std::memcpy(writes.data() + 8u, &adRegs, 8u);
            std::memcpy(writes.data() + 16u, registerWrites.data(), registerWrites.size());
            run.kick(run.user, writes.data(), static_cast<uint32_t>(writes.size()));
        }
        // A texture no upload of the game makes (the frame itself, sampled by the full-screen effects): not native yet.
        for (const Batch &batch : batches)
            if (batch.material.textured && !batch.triangles.empty() && !cache.texture(batch.material.tex0, batch.material.texa))
                return false;
        // VP2S_NATIVE_PROGRAMS_LOG=<n>: each distinct (program, texture, blend, context) drawn this way, once, up to n.
        static int s_logLeft = [] { const char *v = std::getenv("VP2S_NATIVE_PROGRAMS_LOG"); return v ? std::atoi(v) : 0; }();
        static std::unordered_map<uint64_t, bool> s_logged;
        for (const Batch &batch : batches)
            if (s_logLeft > 0 && !batch.triangles.empty() &&
                s_logged.emplace(batch.material.tex0 * 31u ^ regs[batch.context].alpha * 7u ^ (uint64_t{run.startPc} << 48) ^ batch.context, true).second)
            {
                --s_logLeft;
                const ps2x::native::Vertex &v0 = batch.triangles.front();
                std::fprintf(stderr, "[vp2s-programs] pc %04x ctxt %u tex0 %016llx alpha %016llx test %llx blend %d textured %d tris %zu first (%.1f %.1f %.0f) rgba %08x uv %.3f %.3f\n",
                             run.startPc, batch.context, static_cast<unsigned long long>(batch.material.tex0),
                             static_cast<unsigned long long>(regs[batch.context].alpha), static_cast<unsigned long long>(regs[batch.context].test),
                             static_cast<int>(batch.material.blend), batch.material.textured ? 1 : 0, batch.triangles.size() / 3u, v0.x, v0.y,
                             v0.z, v0.rgba, v0.u, v0.v);
            }
        static const float s_identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (const Batch &batch : batches)
        {
            if (batch.triangles.empty())
                continue;
            uint64_t group = 0;
            {
                std::lock_guard<std::mutex> lock(g_nativeMutex);
                ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
                ensureNativeStarted();
                group = ++g_nativeGroup;
                renderer.beginGroup(group);
                renderer.draw(batch.material, s_identity, batch.triangles.data(), static_cast<uint32_t>(batch.triangles.size()));
            }
            run.nativeDraw(run.nativeUser, group, static_cast<int>(batch.context));
        }
        return true;
    }

    bool runLifted(ps2x::native::Vu1HleRun &run)
    {
        static const bool s_on = [] { const char *v = std::getenv("VP2S_VU1_PROGRAMS"); return !(v && v[0] == '0'); }();
        static const char *s_images = std::getenv("VP2S_VU1_IMAGES");
        static const bool s_log = std::getenv("VP2S_NATIVE_MESH_LOG") != nullptr;
        if (!run.code || !run.data || !run.state || !run.kick)
            return false;
        const Vp2sVu1Program program = s_on ? vp2sVu1Program(run.code, run.startPc) : nullptr;
        const uint32_t image = (!program && (s_log || s_images)) || s_log ? vp2sVu1ImageHash(run.code) : 0u;
        if (!program)
        {
            if (s_log)
                noteOtherProgram(run.startPc, image);
            if (s_images)
            {
                static std::mutex s_mutex;
                static std::unordered_map<uint64_t, bool> s_saved;
                std::lock_guard<std::mutex> lock(s_mutex);
                if (s_saved.emplace((static_cast<uint64_t>(image) << 32) | run.startPc, true).second)
                {
                    char path[600];
                    std::snprintf(path, sizeof(path), "%s/%08x.bin", s_images, image);
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(run.code, 1, 16384u, f);
                        std::fclose(f);
                    }
                    std::snprintf(path, sizeof(path), "%s/%08x.txt", s_images, image);
                    if (FILE *f = std::fopen(path, "a"))
                    {
                        std::fprintf(f, "%x\n", run.startPc);
                        std::fclose(f);
                    }
                }
            }
            return false;
        }
        // A program that reaches a jump the lifting did not see coming stops there: VU1 then runs it from the start.
        thread_local std::array<uint8_t, 16384> data;
        std::memcpy(data.data(), run.data, data.size());
        const VU1State state = *run.state;
        thread_local std::vector<std::vector<uint8_t>> kicks;
        kicks.clear();
        const auto keep = [](void *user, const uint8_t *packet, uint32_t bytes) {
            static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(packet, packet + bytes);
        };
        if (program(run.startPc, *run.state, run.data, run.top, keep, &kicks) == ~0ull)
        {
            std::memcpy(run.data, data.data(), data.size());
            *run.state = state;
            if (s_log)
                noteOtherProgram(run.startPc, image);
            return false;
        }
        for (const std::vector<uint8_t> &packet : kicks)
            if (!drawPacketNatively(run, packet))
                run.kick(run.user, packet.data(), static_cast<uint32_t>(packet.size()));
        countHle(17);
        return true;
    }
}

bool vp2sVu1Hle(ps2x::native::Vu1HleRun &run)
{
    if (run.startPc == 0x0678u && run.code && run.data && run.state && run.kick && runKickSetup(run))
    {
        countHle(15);
        return true;
    }
    if (run.startPc == 0x0238u && run.code && run.data && run.state && run.kick && runObjectSetup(run))
    {
        countHle(16);
        return true;
    }
    // VP2S_NATIVE_SHADOW=0: the shadow program on VU1 (its volume counted in the frame's alpha, as on the PS2).
    static const bool s_nativeShadow = [] { const char *v = std::getenv("VP2S_NATIVE_SHADOW"); return !(v && v[0] == '0'); }();
    if (run.startPc == 0x06f0u && s_nativeShadow && run.code && run.data && runShadow(run))
    {
        countHle(16);
        return true;
    }
    thread_local std::vector<uint8_t> prefix, header;
    prefix.clear();
    header.clear();
    const bool headed = run.startPc == 0x06e0u && run.code && run.data && vp2sMeshHeader(run.code, run.data, header);
    const bool prefixed = (run.startPc == 0x0770u || headed) && run.code && run.data &&
                          vp2sMeshPrefix(run.code, run.data, run.top, prefix);
    if ((run.startPc != 0x0880u && !prefixed) || !run.code || !run.data || !run.state || !run.kick)
    {
        if (runLifted(run))
            return true;
        countHle(10);
        return false;
    }
    thread_local std::vector<uint8_t> packet;
    thread_local Vp2sNativeMesh mesh;
    const bool native = vp2sNativeMeshEnabled() && run.nativeDraw;
    if (!vp2sBuildMeshPacket(run.code, run.data, *run.state, run.top, packet, native ? &mesh : nullptr))
    {
        if (runLifted(run))
            return true;
        countHle(g_rejected);
        return false;
    }
    // Program 0x06e0 kicks the frame's register header first, then runs as 0x0770: its register prefix (XGKICK vi07),
    // then the object. Their output buffer flag (mem[81].w) moves as theirs does: 0x06e0 sets mem[36].x then 0x0770's
    // part swaps it, so 0x06e0 leaves mem[36].y; 0x0770 swaps between mem[36].x and .y.
    if (headed)
    {
        noteRegisterWrites(header.data(), static_cast<uint32_t>(header.size()));
        run.kick(run.user, header.data(), static_cast<uint32_t>(header.size()));
    }
    if (prefixed)
    {
        noteRegisterWrites(prefix.data(), static_cast<uint32_t>(prefix.size()));
        run.kick(run.user, prefix.data(), static_cast<uint32_t>(prefix.size()));
        int32_t flag, x, y;
        std::memcpy(&flag, run.data + 81u * 16u + 12u, 4u);
        std::memcpy(&x, run.data + 36u * 16u, 4u);
        std::memcpy(&y, run.data + 36u * 16u + 4u, 4u);
        const int32_t next = headed || flag == x ? y : x;
        std::memcpy(run.data + 81u * 16u + 12u, &next, 4u);
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
    // The object's material from the registers its packets set, drawn by the native renderer. VP2S_NATIVE_MATERIAL=0
    // (comparison only): the GS state where the object sits, drawn by the GS front end as GS primitives.
    static const bool s_ownMaterial = [] { const char *v = std::getenv("VP2S_NATIVE_MATERIAL"); return !(v && v[0] == '0'); }();
    ps2x::native::Material material = nativeMaterial(mesh.prim);
    if (!s_ownMaterial)
    {
        material = ps2x::native::Material{};
        material.fromGsContext = true;
        material.textured = (mesh.prim >> 4) & 1u;
        material.fog = (mesh.prim >> 5) & 1u;
        material.alphaBlend = (mesh.prim >> 6) & 1u;
        material.context = static_cast<uint8_t>((mesh.prim >> 9) & 1u);
    }
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
