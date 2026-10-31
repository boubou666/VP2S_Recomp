// VP2's textures converted offline (tools/vp2_textures.py drives it): a texture stored in its file as a CT32 transfer
// block holding 4- or 8-bit texels in the GS's memory order, laid out and read back in its own format with the GS
// layout functions, here only: the game never runs them (the renderer loads the RGBA this writes by the game's key).
//   vp2s_texconv <block file> <out .rgba>
// Block file (written by tools/vp2_textures.py): u32 psm, u32 width, u32 height, u32 stored width, u32 stored height,
// u32 stored buffer width, then 256 or 16 RGBA8 palette entries (CSM1 order resolved, 1024 bytes), then the stored
// CT32 data (stored width x stored height words).
// Output: u32 width, u32 height, width * height RGBA8.

#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ps2_runtime expects the generated runner to provide its function table; this tool runs no guest code.
struct R5900Context;
class PS2Runtime;
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
void (*g_ps2RecompiledFunctionTable[1])(uint8_t *, R5900Context *, PS2Runtime *) = {nullptr};

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <block file> <out .rgba>\n", argv[0]);
        return 2;
    }
    FILE *in = std::fopen(argv[1], "rb");
    if (!in)
        return 2;
    uint32_t head[6];
    if (std::fread(head, 4, 6, in) != 6)
        return 2;
    const uint32_t psm = head[0], width = head[1], height = head[2], sw = head[3], sh = head[4], sbw = head[5];
    std::vector<uint32_t> palette(256);
    if (std::fread(palette.data(), 4, 256, in) != 256)
        return 2;
    std::vector<uint32_t> stored(static_cast<size_t>(sw) * sh);
    const size_t got = std::fread(stored.data(), 4, stored.size(), in);
    std::fclose(in);
    if (got != stored.size())
        return 2;

    GSMem::InitLookupTables();
    std::vector<uint8_t> memory(4u * 1024u * 1024u, 0u);
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x)
            GSMem::WriteCT32(memory.data(), 0, sbw ? sbw : 1u, x, y, stored[static_cast<size_t>(y) * sw + x]);
    // The texels as the game reads them: its buffer width in 64-pixel units (a texture is as wide as its pages).
    const uint32_t tbw = std::max<uint32_t>(2u, (width + 63u) / 64u);
    std::vector<uint32_t> rgba(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t index = psm == 0x13 ? GSMem::ReadP8(memory.data(), 0, tbw, x, y) : GSMem::ReadP4(memory.data(), 0, tbw, x, y);
            rgba[static_cast<size_t>(y) * width + x] = palette[index & 0xFFu];
        }
    FILE *out = std::fopen(argv[2], "wb");
    if (!out)
        return 2;
    const uint32_t size[2] = {width, height};
    std::fwrite(size, 4, 2, out);
    std::fwrite(rgba.data(), 4, rgba.size(), out);
    std::fclose(out);
    return 0;
}
