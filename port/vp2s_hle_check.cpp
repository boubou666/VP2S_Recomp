// Checks the native version of VP2's mesh microprogram (vp2s_vu1_hle.cpp) against recorded VU1 runs:
//   vp2s_hle_check <trace.vu1rec> [repeat]
// For each run of program 0x0880 it builds the PATH1 packet natively from the run's VU1 data memory and registers and
// compares every drawn vertex with the packet the microprogram sent: ST (relative 1e-3), RGBA (exact), XYZ (1/16
// pixel), fog, and ADC from a strip's third vertex. repeat > 1 also times the native build.
#include "vp2s_vu1_hle.h"
#include "runtime/ps2_vu1_trace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
    std::map<int, size_t> rejections; // vp2sMeshRejection() of the runs not built natively
    std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, size_t>> images; // (code image, start): runs, built
    uint32_t image = 0;
    double seconds = 0.0;
    std::vector<uint8_t> packet;
    while (readVu1TraceRecord(in, record))
    {
        if (!record.code.empty())
        {
            code = record.code;
            // FNV-1a 32 of the 16 KiB, as vp2s_vu1_hle.cpp's program log names images.
            image = 2166136261u;
            for (size_t i = 0; i < 16384u && i < code.size(); ++i)
                image = (image ^ code[i]) * 16777619u;
        }
        // VP2S_CHECK_EXTRACT=<image hex>:<start hex>:<dir>:<max>: the first runs of that program as files for offline
        // study: memory and registers before and after, and each packet it kicked.
        static const char *s_extract = std::getenv("VP2S_CHECK_EXTRACT");
        if (s_extract)
        {
            static uint32_t s_image = 0, s_start = 0;
            static unsigned s_max = 0, s_done = 0;
            static char s_dir[512] = {};
            static const bool s_parsed = std::sscanf(s_extract, "%x:%x:%511[^:]:%u", &s_image, &s_start, s_dir, &s_max) == 4;
            if (s_parsed && image == s_image && record.startPC == s_start && record.resume == 0u && s_done < s_max)
            {
                char path[600];
                const auto save = [&](const char *what, const void *bytes, size_t size) {
                    std::snprintf(path, sizeof(path), "%s/run%03u_%s.bin", s_dir, s_done, what);
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(bytes, 1, size, f);
                        std::fclose(f);
                    }
                };
                save("before", record.dataBefore.data(), record.dataBefore.size());
                save("after", record.dataAfter.data(), record.dataAfter.size());
                save("state_before", &record.stateBefore, sizeof(record.stateBefore));
                save("state_after", &record.stateAfter, sizeof(record.stateAfter));
                save("code", code.data(), code.size());
                for (size_t k = 0; k < record.packets.size(); ++k)
                {
                    char what[32];
                    std::snprintf(what, sizeof(what), "packet%02zu", k);
                    save(what, record.packets[k].data(), record.packets[k].size());
                }
                std::snprintf(path, sizeof(path), "%s/run%03u_info.txt", s_dir, s_done);
                if (FILE *f = std::fopen(path, "w"))
                {
                    std::fprintf(f, "top %u itop %u packets %zu\n", record.top, record.itop, record.packets.size());
                    std::fclose(f);
                }
                ++s_done;
            }
        }
        if (record.startPC == 0x0238u && record.resume == 0u)
        {
            // Program 0x0238 (vp2sVu1Hle's object set-up): the rows and registers it leaves, to float rounding.
            static size_t s_setupRuns = 0, s_setupBad = 0;
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
            ++s_setupRuns;
            // Relative to the vector's size: a far object's eye (1e7 away) or a light near its translation leaves float
            // cancellation in the small components, the same in either computation's rounding.
            const auto close = [](const float *got, const float *want, int lanes) {
                float size = 1.0f;
                for (int i = 0; i < lanes; ++i)
                    size = std::max(size, std::fabs(want[i]));
                for (int i = 0; i < lanes; ++i)
                    if (std::fabs(got[i] - want[i]) > 1e-4f * size)
                        return false;
                return true;
            };
            int badRow = -1, badReg = -1;
            for (uint32_t r : {20u, 21u, 22u, 28u, 29u, 30u, 31u, 37u, 73u, 74u, 75u, 77u, 78u, 79u, 80u, 85u, 86u, 87u, 88u, 89u,
                               90u, 91u, 92u})
                if (badRow < 0 && !close(reinterpret_cast<const float *>(data.data() + r * 16u),
                                         reinterpret_cast<const float *>(record.dataAfter.data() + r * 16u), 4))
                    badRow = static_cast<int>(r);
            if (badRow < 0 && !close(reinterpret_cast<const float *>(data.data() + 72u * 16u),
                                     reinterpret_cast<const float *>(record.dataAfter.data() + 72u * 16u), 3))
                badRow = 72;
            for (int r : {1, 2, 3, 4, 5, 6, 7})
                if (badReg < 0 && !close(state.vf[r], record.stateAfter.vf[r], 4))
                    badReg = r;
            if (badReg < 0 && !close(state.vf[8], record.stateAfter.vf[8], 3))
                badReg = 8;
            const bool samePacket = !record.packets.empty() && kicked == record.packets[0];
            if (!handled || badRow >= 0 || badReg >= 0 || !samePacket)
            {
                if (s_setupBad++ < 10u)
                {
                    std::printf("object set-up run %zu: handled %d row %d reg %d packet %d\n", s_setupRuns, handled ? 1 : 0, badRow,
                                badReg, samePacket ? 1 : 0);
                    const float *got = badReg >= 0 ? state.vf[badReg] : reinterpret_cast<const float *>(data.data() + badRow * 16u);
                    const float *want = badReg >= 0 ? record.stateAfter.vf[badReg]
                                                    : reinterpret_cast<const float *>(record.dataAfter.data() + badRow * 16u);
                    if (badReg >= 0 || badRow >= 0)
                        std::printf("  got %g %g %g %g want %g %g %g %g\n", got[0], got[1], got[2], got[3], want[0], want[1], want[2],
                                    want[3]);
                }
            }
            if ((s_setupRuns % 2000u) == 0u)
                std::printf("object set-up: %zu runs, %zu differ\n", s_setupRuns, s_setupBad);
            continue;
        }
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
        if ((record.startPC != 0x0880u && record.startPC != 0x0770u && record.startPC != 0x06e0u) || record.resume != 0u)
            continue;
        // Program 0x0770: its register prefix is the first packet it kicks, the object the second. Program 0x06e0: its
        // register header first, then as 0x0770.
        size_t meshPacket = 0;
        auto &imageCount = images[{image, record.startPC}];
        ++imageCount.first;
        if (record.startPC == 0x06e0u)
        {
            static size_t s_headerRuns = 0, s_headerBad = 0;
            std::vector<uint8_t> header;
            ++s_headerRuns;
            if (!vp2sMeshHeader(code.data(), record.dataBefore.data(), header))
                continue; // another program at 0x06e0 in this build
            if (record.packets.empty() || header != record.packets[0])
            {
                if (s_headerBad++ < 5u)
                    std::printf("header run %zu: packets %zu first %zu bytes\n", s_headerRuns, record.packets.size(),
                                record.packets.empty() ? size_t{0} : record.packets[0].size());
                continue;
            }
            meshPacket = 1;
        }
        if (record.startPC == 0x0770u || record.startPC == 0x06e0u)
        {
            static size_t s_prefixRuns = 0, s_prefixBad = 0;
            std::vector<uint8_t> prefix;
            const bool known = vp2sMeshPrefix(code.data(), record.dataBefore.data(), record.top, prefix);
            ++s_prefixRuns;
            if (!known || record.packets.size() < meshPacket + 2u || prefix != record.packets[meshPacket])
            {
                if (s_prefixBad++ < 5u)
                    std::printf("prefix run %zu: known %d packets %zu first %zu bytes\n", s_prefixRuns, known ? 1 : 0, record.packets.size(),
                                record.packets.size() <= meshPacket ? size_t{0} : record.packets[meshPacket].size());
                continue;
            }
            meshPacket += 1;
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
        {
            ++rejections[vp2sMeshRejection()];
            continue;
        }
        ++built;
        ++imageCount.second;
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
    for (const auto &[key, count] : images)
        std::printf("image %08x program %04x: %zu runs, %zu built\n", key.first, key.second, count.first, count.second);
    for (const auto &[reason, count] : rejections)
        std::printf("not built, reason %d: %zu runs\n", reason, count);
    return 0;
}
