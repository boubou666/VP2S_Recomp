// GS side of the VP2S_DIAG diagnostics: display registers, host presentation and the recent
// GS activity (GIF tags, register writes, draws, transfers, presents) recorded by the runtime.

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"

#include <array>
#include <iostream>

void printGsDiagnostics(PS2Runtime &runtime)
{
    const GSRegisters &regs = runtime.memory().gs();
    std::cout << std::hex << "[vp2s-gs] PMODE=0x" << regs.pmode << " SMODE2=0x" << regs.smode2 << " DISPFB1=0x"
              << regs.dispfb1 << " DISPLAY1=0x" << regs.display1 << " DISPFB2=0x" << regs.dispfb2
              << " DISPLAY2=0x" << regs.display2 << " BGCOLOR=0x" << regs.bgcolor << " CSR=0x" << regs.csr.load()
              << std::dec << " gifCopySeen=" << runtime.memory().hasSeenGifCopy() << std::endl;

    const GSDebugSnapshot gs = runtime.gs().getDebugSnapshot();
    std::cout << "[vp2s-gs]   present: has=" << gs.hasHostPresentationFrame << " " << gs.hostPresentationWidth << "x"
              << gs.hostPresentationHeight << " displayFbp=" << gs.hostPresentationDisplayFbp
              << " sourceFbp=" << gs.hostPresentationSourceFbp << " | frame0 fbp=" << gs.ctx[0].frame.fbp
              << " fbw=" << gs.ctx[0].frame.fbw << " psm=0x" << std::hex << int(gs.ctx[0].frame.psm) << std::dec
              << std::endl;

    // Non-black pixels per candidate buffer (640x256 32-bit = 160 KiB words from fbp * 2048 words;
    // the count does not depend on the swizzled layout).
    if (const uint8_t *vram = runtime.memory().getGSVRAM())
    {
        std::cout << "[vp2s-gs]   non-black:";
        for (const uint32_t fbp : {140u, 220u, 360u})
        {
            const uint32_t *words = reinterpret_cast<const uint32_t *>(vram + fbp * 2048u * 4u);
            uint32_t lit = 0;
            for (uint32_t i = 0; i < 640u * 256u; ++i)
            {
                lit += (words[i] & 0x00FFFFFFu) != 0u;
            }
            std::cout << " fbp" << fbp << "=" << (lit * 100u) / (640u * 256u) << "%";
        }
        std::cout << std::endl;
    }

    const std::vector<GSDebugHistoryEntry> history = runtime.gs().getDebugHistory();
    std::array<size_t, 5> counts{};
    uint64_t lastVsync = 0;
    for (const GSDebugHistoryEntry &entry : history)
    {
        const auto kind = static_cast<size_t>(entry.kind);
        if (kind < counts.size())
        {
            ++counts[kind];
        }
        lastVsync = entry.vsyncTick;
    }
    std::cout << "[vp2s-gs]   history " << history.size() << " events (last vsync " << lastVsync
              << "): gif=" << counts[0] << " reg=" << counts[1] << " draw=" << counts[2]
              << " transfer=" << counts[3] << " present=" << counts[4] << std::endl;
    int shown = 0;
    for (auto it = history.rbegin(); it != history.rend() && shown < 4; ++it)
    {
        if (it->kind != GSDebugEventKind::Draw)
        {
            continue;
        }
        ++shown;
        std::cout << "[vp2s-gs]   draw #" << it->seq << " prim=" << int(it->prim.type) << " verts=" << it->vertexCount
                  << " x=" << it->xMin << ".." << it->xMax << " y=" << it->yMin << ".." << it->yMax
                  << " fbp=" << it->frame.fbp << " fbw=" << it->frame.fbw << std::endl;
    }
}
