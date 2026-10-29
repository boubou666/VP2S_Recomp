// Checks the native version of VP2's mesh microprogram (vp2s_vu1_hle.cpp) against recorded VU1 runs:
//   vp2s_hle_check <trace.vu1rec> [repeat]
// For each run of program 0x0880 it builds the PATH1 packet natively from the run's VU1 data memory and registers and
// compares every drawn vertex with the packet the microprogram sent: ST (relative 1e-3), RGBA (exact), XYZ (1/16
// pixel), fog, and ADC from a strip's third vertex. repeat > 1 also times the native build.
#include "vp2s_vu1_hle.h"
#include "runtime/ps2_vu1_trace.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ps2_runtime expects the generated runner to provide its function table; this tool runs no guest code.
struct R5900Context;
class PS2Runtime;
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
void (*g_ps2RecompiledFunctionTable[1])(uint8_t *, R5900Context *, PS2Runtime *) = {nullptr};

namespace
{
    struct Vertex
    {
        float st[3] = {};
        uint32_t rgba[4] = {};
        uint32_t xyz[4] = {};
        bool first = false;
        uint32_t k = 0;
    };

    std::vector<Vertex> parse(const std::vector<uint8_t> &packet)
    {
        std::vector<Vertex> out;
        size_t p = 0;
        while (p + 16u <= packet.size())
        {
            uint64_t lo, hi;
            std::memcpy(&lo, packet.data() + p, 8u);
            std::memcpy(&hi, packet.data() + p + 8u, 8u);
            p += 16u;
            const uint32_t nloop = lo & 0x7FFFu;
            const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            if (((lo >> 58) & 3u) != 0u)
                break;
            for (uint32_t n = 0; n < nloop; ++n)
            {
                Vertex v;
                v.k = n;
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (p + 16u > packet.size())
                        return out;
                    const uint32_t reg = (hi >> (4u * r)) & 0xFu;
                    if (reg == 0x2u)
                        std::memcpy(v.st, packet.data() + p, 12u);
                    else if (reg == 0x1u)
                        std::memcpy(v.rgba, packet.data() + p, 16u);
                    else if (reg == 0x4u || reg == 0x5u)
                        std::memcpy(v.xyz, packet.data() + p, 16u);
                    p += 16u;
                }
                out.push_back(v);
            }
            if ((lo >> 15) & 1u)
                break;
        }
        return out;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <trace.vu1rec> [repeat]\n", argv[0]);
        return 2;
    }
    const int repeat = argc > 2 ? std::max(1, std::atoi(argv[2])) : 1;
    FILE *in = std::fopen(argv[1], "rb");
    if (!in)
        return 2;
    std::vector<uint8_t> code(Vu1TraceRecord::kMemorySize, 0u);
    Vu1TraceRecord record;
    size_t runs = 0, built = 0, vertices = 0, badSt = 0, badRgba = 0, badXyz = 0, badAdc = 0, badFog = 0, badCount = 0;
    double seconds = 0.0;
    std::vector<uint8_t> packet;
    while (readVu1TraceRecord(in, record))
    {
        if (!record.code.empty())
            code = record.code;
        if (record.startPC == 0x0678u && record.resume == 0u)
        {
            // Program 0x0678 (vp2sVu1Hle's kick set-up): the packet kicked, the memory and registers it leaves.
            static size_t s_kickRuns = 0, s_kickBad = 0;
            std::vector<uint8_t> data = record.dataBefore;
            VU1State state = record.stateBefore;
            std::vector<uint8_t> kicked;
            ps2x::native::Vu1HleRun run;
            run.code = code.data();
            run.data = data.data();
            run.state = &state;
            run.startPc = record.startPC;
            run.top = record.top;
            run.user = &kicked;
            run.kick = [](void *user, const uint8_t *bytes, uint32_t size)
            { auto &out = *static_cast<std::vector<uint8_t> *>(user); out.insert(out.end(), bytes, bytes + size); };
            const bool handled = vp2sVu1Hle(run);
            ++s_kickRuns;
            const bool samePacket = !record.packets.empty() && kicked == record.packets[0];
            const bool sameData = std::memcmp(data.data() + 0x3aau * 16u, record.dataAfter.data() + 0x3aau * 16u, 5u * 16u) == 0;
            const bool sameRegs = std::memcmp(state.vf[1], record.stateAfter.vf[1], 5u * 16u) == 0 && state.vi[1] == record.stateAfter.vi[1];
            if (!handled || !samePacket || !sameData || !sameRegs)
            {
                if (s_kickBad++ < 5u)
                    std::printf("kick setup run %zu: handled %d packet %d (%zu vs %zu bytes) data %d regs %d\n", s_kickRuns, handled ? 1 : 0,
                                samePacket ? 1 : 0, kicked.size(), record.packets.empty() ? size_t{0} : record.packets[0].size(),
                                sameData ? 1 : 0, sameRegs ? 1 : 0);
            }
            if ((s_kickRuns % 500u) == 0u)
                std::printf("kick setup: %zu runs, %zu differ\n", s_kickRuns, s_kickBad);
            continue;
        }
        if ((record.startPC != 0x0880u && record.startPC != 0x0770u) || record.resume != 0u)
            continue;
        // Program 0x0770: its register prefix is the first packet it kicks, the object the second.
        size_t meshPacket = 0;
        if (record.startPC == 0x0770u)
        {
            static size_t s_prefixRuns = 0, s_prefixBad = 0;
            std::vector<uint8_t> prefix;
            const bool known = vp2sMeshPrefix(code.data(), record.dataBefore.data(), record.top, prefix);
            ++s_prefixRuns;
            if (!known || record.packets.size() < 2u || prefix != record.packets[0])
            {
                if (s_prefixBad++ < 5u)
                    std::printf("prefix run %zu: known %d packets %zu first %zu bytes\n", s_prefixRuns, known ? 1 : 0, record.packets.size(),
                                record.packets.empty() ? size_t{0} : record.packets[0].size());
                continue;
            }
            meshPacket = 1;
            if ((s_prefixRuns % 500u) == 0u)
                std::printf("prefix: %zu runs, %zu differ\n", s_prefixRuns, s_prefixBad);
        }
        ++runs;
        const auto start = std::chrono::steady_clock::now();
        bool ok = false;
        for (int i = 0; i < repeat; ++i)
            ok = vp2sBuildMeshPacket(code.data(), record.dataBefore.data(), record.stateBefore, record.top, packet);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / repeat;
        if (!ok)
            continue;
        ++built;
        const std::vector<Vertex> got = parse(packet);
        const std::vector<Vertex> want = record.packets.size() <= meshPacket ? std::vector<Vertex>{} : parse(record.packets[meshPacket]);
        if (got.size() != want.size())
            ++badCount;
        for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        {
            const Vertex &g = got[i], &w = want[i];
            const bool wantAdc = (w.xyz[3] & 0x8000u) != 0u;
            if (g.k >= 2u && (((g.xyz[3] & 0x8000u) != 0u) != wantAdc))
            {
                if (badAdc++ < 20u)
                    std::printf("adc mismatch: run %zu vertex %zu (strip index %u) got %d want %d\n", runs, i, g.k,
                                (g.xyz[3] & 0x8000u) != 0u ? 1 : 0, wantAdc ? 1 : 0);
            }
            if (wantAdc)
                continue;
            ++vertices;
            for (int c = 0; c < 3; ++c)
                if (std::fabs(g.st[c] - w.st[c]) > 1e-3f * std::max(1.0f, std::fabs(w.st[c])))
                {
                    ++badSt;
                    break;
                }
            if (std::memcmp(g.rgba, w.rgba, 16u) != 0 && badRgba++ < 12u)
            {
                // The run's lighting: header 8.y (mode) at TOP, mem[14] (tables).
                const auto word = [&](uint32_t qw, uint32_t lane) {
                    int32_t value;
                    std::memcpy(&value, record.dataBefore.data() + ((qw % 1024u) * 16u + lane * 4u), 4u);
                    return value;
                };
                const uint32_t top = record.top % 1024u;
                std::printf("rgba: run %zu vertex %zu mode %d mem14 %d,%d,%d,%d got %u,%u,%u,%u want %u,%u,%u,%u\n", runs, i,
                            word(top + 8u, 1), word(14u, 0), word(14u, 1), word(14u, 2), word(14u, 3), g.rgba[0], g.rgba[1],
                            g.rgba[2], g.rgba[3], w.rgba[0], w.rgba[1], w.rgba[2], w.rgba[3]);
                // The microprogram's lighting factors (its table at 0x380, after the run) and its inputs.
                const auto after = [&](uint32_t qw, uint32_t lane) {
                    float value;
                    std::memcpy(&value, record.dataAfter.data() + ((qw % 1024u) * 16u + lane * 4u), 4u);
                    return value;
                };
                const auto before = [&](uint32_t qw, uint32_t lane) {
                    float value;
                    std::memcpy(&value, record.dataBefore.data() + ((qw % 1024u) * 16u + lane * 4u), 4u);
                    return value;
                };
                if (badRgba == 1u)
                {
                    for (uint32_t q = 0x380u; q < 0x380u + 16u; ++q)
                        std::printf("  table[%#x] = %g %g %g %g\n", q, after(q, 0), after(q, 1), after(q, 2), after(q, 3));
                    for (uint32_t q = 5u; q <= 8u; ++q)
                        std::printf("  mem[%u] = %g %g %g %g\n", q, before(q, 0), before(q, 1), before(q, 2), before(q, 3));
                    for (int r = 5; r <= 8; ++r)
                        std::printf("  vf%02d = %g %g %g %g\n", r, record.stateBefore.vf[r][0], record.stateBefore.vf[r][1],
                                    record.stateBefore.vf[r][2], record.stateBefore.vf[r][3]);
                    std::printf("  header 9 = %d %d %d %d, 12 = %g %g %g %g\n", word(top + 9u, 0), word(top + 9u, 1), word(top + 9u, 2),
                                word(top + 9u, 3), before(top + 12u, 0), before(top + 12u, 1), before(top + 12u, 2), before(top + 12u, 3));
                }
            }
            const auto near16 = [](uint32_t a, uint32_t b) { return std::abs(static_cast<int>((a - b) & 0xFFFFu) << 16 >> 16) <= 1; };
            if (!near16(g.xyz[0], w.xyz[0]) || !near16(g.xyz[1], w.xyz[1]))
                ++badXyz;
            if (((g.xyz[3] >> 4) & 0xFFu) != ((w.xyz[3] >> 4) & 0xFFu))
            {
                if (badFog++ < 5u)
                    std::printf("fog: got w=%08x want w=%08x (vertex %u)\n", g.xyz[3], w.xyz[3], g.k);
            }
        }
    }
    std::fclose(in);
    std::printf("runs=%zu built natively=%zu (%.1f%%) drawn vertices=%zu\n", runs, built, runs ? 100.0 * built / runs : 0.0,
                vertices);
    std::printf("mismatches: count=%zu st=%zu rgba=%zu xyz=%zu fog=%zu adc=%zu\n", badCount, badSt, badRgba, badXyz,
                badFog, badAdc);
    std::printf("native build: %.3f ms total, %.2f us per run\n", seconds * 1000.0, runs ? seconds * 1e6 / runs : 0.0);
    return 0;
}
