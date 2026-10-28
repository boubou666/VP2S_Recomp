// GS side of the VP2S_DIAG diagnostics: display registers, host presentation and the recent
// GS activity (GIF tags, register writes, draws, transfers, presents) recorded by the runtime.

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"

#include <array>
#include <cstring>
#include <iostream>
#include <string>

namespace ps2_stubs
{
    std::string mpegDebugSummary(); // Kernel/Stubs/MPEG.cpp
}

void printGsDiagnostics(PS2Runtime &runtime)
{
    const GSRegisters &regs = runtime.memory().gs();
    std::cout << std::hex << "[vp2s-gs] PMODE=0x" << regs.pmode << " SMODE2=0x" << regs.smode2 << " DISPFB1=0x"
              << regs.dispfb1 << " DISPLAY1=0x" << regs.display1 << " DISPFB2=0x" << regs.dispfb2
              << " DISPLAY2=0x" << regs.display2 << " BGCOLOR=0x" << regs.bgcolor << " CSR=0x" << regs.csr.load()
              << std::dec << " gifCopySeen=" << runtime.memory().hasSeenGifCopy() << std::endl;

    // DMA channels the game's GIF queue depends on (GIF = ch2, VIF1 = ch1), and that queue itself (object at
    // *0x35dd78: +8 size, +A head, +C tail = head + 1 once the DMA end handler drained it, +E busy).
    PS2Memory &mem = runtime.memory();
    std::cout << std::hex << "[vp2s-gs]   dma: gif chcr=0x" << mem.readIORegister(0x1000A000u) << " madr=0x"
              << mem.readIORegister(0x1000A010u) << " qwc=0x" << mem.readIORegister(0x1000A020u) << " tadr=0x"
              << mem.readIORegister(0x1000A030u) << " | vif1 chcr=0x" << mem.readIORegister(0x10009000u)
              << " qwc=0x" << mem.readIORegister(0x10009020u) << " | d_stat=0x" << mem.readIORegister(0x1000E010u)
              << " d_pcr=0x" << mem.readIORegister(0x1000E020u) << std::dec;
    if (const uint8_t *rdram = mem.getRDRAM())
    {
        uint32_t queue = 0;
        std::memcpy(&queue, rdram + 0x35DD78u, 4u);
        if (queue >= 0x100000u && queue + 16u <= PS2_RAM_SIZE)
        {
            int16_t size, head, tail;
            std::memcpy(&size, rdram + queue + 8u, 2u);
            std::memcpy(&head, rdram + queue + 10u, 2u);
            std::memcpy(&tail, rdram + queue + 12u, 2u);
            std::cout << " | gif queue 0x" << std::hex << queue << std::dec << " size=" << size << " head=" << head
                      << " tail=" << tail << " busy=" << int(rdram[queue + 14u]) << " +f=" << int(rdram[queue + 15u]);
            // Entries (16 bytes at *(queue + 4)): +0 kind, +2 flags, +4 data, +C callback object.
            uint32_t entries = 0;
            std::memcpy(&entries, rdram + queue + 4u, 4u);
            for (int k = 0; k < 2 && size > 0 && entries >= 0x100000u && entries + 16u * size <= PS2_RAM_SIZE; ++k)
            {
                const uint32_t at = entries + 16u * static_cast<uint32_t>((head + k) % size);
                uint32_t w[4];
                std::memcpy(w, rdram + at, 16u);
                std::cout << std::hex << " [" << std::dec << (head + k) % size << std::hex << "]=" << w[0] << "," << w[1]
                          << "," << w[2] << "," << w[3] << std::dec;
            }
        }
    }
    std::cout << std::endl;
    // The last GIF events, oldest first (PS2Memory::noteGifEvent).
    uint32_t next = 0;
    const auto events = mem.gifEvents(next);
    std::cout << std::hex << "[vp2s-gs]   gif events (" << std::dec << next << "):" << std::hex;
    for (uint32_t k = next > PS2Memory::kGifEvents ? next - PS2Memory::kGifEvents : 0u; k < next; ++k)
    {
        const PS2Memory::GifEvent &e = events[k % PS2Memory::kGifEvents];
        std::cout << " " << static_cast<char>(e.kind);
        if (e.kind == 'S')
            std::cout << e.a << "/" << e.b << "/" << e.c;
        else if (e.kind == 'X' || e.kind == 'c')
            std::cout << e.a;
        else if (e.kind != 'Q')
            std::cout << e.a << "/" << e.b;
    }
    std::cout << std::dec << std::endl;

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

    const std::string mpeg = ps2_stubs::mpegDebugSummary();
    std::cout << "[vp2s-gs]   mpeg: " << mpeg << std::endl;
    // VP2's movie player object holds its sceMpeg at +0xce500 (FUN_002b94a0 in the core module):
    // state +0x15e648, ring bytes +0xc999c/+0xc9998, pictures shown +0x15e658, buffers +0x15e640.
    if (const size_t at = mpeg.find("[0x"); at != std::string::npos)
    {
        const uint32_t player = static_cast<uint32_t>(std::stoul(mpeg.substr(at + 3), nullptr, 16)) - 0xce500u;
        const uint8_t *rdram = runtime.memory().getRDRAM();
        auto word = [&](uint32_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((player + offset) & PS2_RAM_MASK), sizeof(value));
            return value;
        };
        const uint32_t buffers = word(0x15e640);
        uint32_t bufferCount = 0;
        if (buffers != 0u)
        {
            std::memcpy(&bufferCount, rdram + ((buffers + 0x8098u) & PS2_RAM_MASK), sizeof(bufferCount));
        }
        std::cout << std::hex << "[vp2s-gs]   movie player 0x" << player << ": state=" << word(0x15e648)
                  << " flag654=" << (word(0x15e654) & 0xFFu) << " shown=" << std::dec << word(0x15e658) << std::hex
                  << " ring=0x" << word(0xc999c) << "/0x" << word(0xc9998) << " buffers=0x" << buffers
                  << " count8098=" << std::dec << bufferCount << " waitVsync=" << ((word(0x15e644) >> 8) & 0xFFu)
                  << std::endl;
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
