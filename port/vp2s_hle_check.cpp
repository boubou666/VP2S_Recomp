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
        if (record.startPC != 0x0880u || record.resume != 0u)
            continue;
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
        const std::vector<Vertex> want = record.packets.empty() ? std::vector<Vertex>{} : parse(record.packets[0]);
        if (got.size() != want.size())
            ++badCount;
        for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        {
            const Vertex &g = got[i], &w = want[i];
            const bool wantAdc = (w.xyz[3] & 0x8000u) != 0u;
            if (g.k >= 2u && (((g.xyz[3] & 0x8000u) != 0u) != wantAdc))
                ++badAdc;
            if (wantAdc)
                continue;
            ++vertices;
            for (int c = 0; c < 3; ++c)
                if (std::fabs(g.st[c] - w.st[c]) > 1e-3f * std::max(1.0f, std::fabs(w.st[c])))
                {
                    ++badSt;
                    break;
                }
            if (std::memcmp(g.rgba, w.rgba, 16u) != 0)
                ++badRgba;
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
