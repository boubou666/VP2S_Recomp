// VP2's textures for the native renderer from the game's own texture code, not from GS transfers: the game's VRAM
// texture cache (mod001 FUN_0029d8f0, a0 its object, a2 / a3 the texture's key: file tag and index, v0 the VRAM
// address it gives, TBP = v0 >> 6) is hooked to note which texture the game puts where; a draw's TEX0 then names its
// texture by TBP, and the picture comes from the offline conversion of the game's files (tools/vp2_textures.py:
// <dir>/<tag>_<index>.tex), with the CSA the TEX0 picks. Nothing reads GS packets or transfer blocks while the game
// runs. VP2S_TEXTURES=<dir> (default build/vp2_textures next to the executable's build tree); VP2S_GAME_TEXTURES=1
// turns it on (not yet the default, see vp2sInstallGameTextures).

#include "vp2s_textures.h"

#include "ps2_overlay_modules.h"
#include "ps2_runtime.h"
#include "runtime/native/native_texture_cache.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t kTextureCache = 0x0029D8F0u; // mod001

    struct Key
    {
        uint32_t tag = 0, index = 0;
    };

    struct Converted
    {
        uint32_t width = 0, height = 0, psm = 0;
        std::vector<uint8_t> texels;      // indices (T4 / T8) or RGBA8 (CT32)
        std::array<uint32_t, 256> palette{}; // RGBA8, CSM1 resolved
    };

    std::mutex g_mutex;
    std::unordered_map<uint32_t, Key> g_placed;                          // TBP -> the texture the game put there
    std::unordered_map<uint64_t, std::shared_ptr<Converted>> g_files;    // (tag << 32 | index) -> its conversion
    std::unordered_map<uint64_t, std::shared_ptr<const ps2x::native::TextureCache::Texture>> g_textures; // + TEX0 bits
    uint64_t g_version = 0;
    PS2Runtime::RecompiledFunction g_original = nullptr;

    std::string directory()
    {
        if (const char *dir = std::getenv("VP2S_TEXTURES"))
            return dir;
        return "../vp2_textures";
    }

    // The game's texture cache: the texture with key (a2, a3) is at the VRAM address it returns.
    void textureCacheHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t tag = getRegU32(ctx, 6), index = getRegU32(ctx, 7);
        g_original(rdram, ctx, runtime);
        const uint32_t address = getRegU32(ctx, 2);
        if (address == 0u)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_placed[(address >> 6) & 0x3FFFu] = {tag, index};
    }

    // The game's other texture allocation (mod001 FUN_0023d810, called by FUN_0023cb20 for the textures of loaded
    // files): a1 the texture's info block in EE RAM (+4 file tag, +8 index: the same key), v0 the VRAM address.
    constexpr uint32_t kTextureAllocate = 0x0023D810u;
    PS2Runtime::RecompiledFunction g_allocate = nullptr;

    void textureAllocateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t info = getRegU32(ctx, 5) & 0x1FFFFFFu;
        g_allocate(rdram, ctx, runtime);
        const uint32_t address = getRegU32(ctx, 2);
        if (address == 0u || info + 12u > PS2_RAM_SIZE)
            return;
        uint32_t tag, index;
        std::memcpy(&tag, rdram + info + 4u, 4u);
        std::memcpy(&index, rdram + info + 8u, 4u);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_placed[(address >> 6) & 0x3FFFu] = {tag, index};
    }

    std::shared_ptr<Converted> load(const Key &key)
    {
        const uint64_t id = (uint64_t{key.tag} << 32) | key.index;
        if (const auto it = g_files.find(id); it != g_files.end())
            return it->second;
        char name[64];
        std::snprintf(name, sizeof(name), "/%08x_%x.tex", key.tag, key.index);
        std::shared_ptr<Converted> converted;
        if (FILE *f = std::fopen((directory() + name).c_str(), "rb"))
        {
            converted = std::make_shared<Converted>();
            uint32_t head[3] = {};
            if (std::fread(head, 4, 3, f) == 3)
            {
                converted->width = head[0];
                converted->height = head[1];
                converted->psm = head[2];
                converted->texels.resize(static_cast<size_t>(head[0]) * head[1] * (head[2] == 0u ? 4u : 1u));
                if (std::fread(converted->texels.data(), 1, converted->texels.size(), f) != converted->texels.size() ||
                    std::fread(converted->palette.data(), 4, 256, f) != 256)
                    converted.reset();
            }
            else
                converted.reset();
            std::fclose(f);
        }
        g_files[id] = converted; // a missing file is remembered too
        return converted;
    }

    std::shared_ptr<const ps2x::native::TextureCache::Texture> provide(uint64_t tex0, uint64_t)
    {
        const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
        std::lock_guard<std::mutex> lock(g_mutex);
        // VP2S_GAME_TEXTURES_LOG: every 100000 lookups, how many found a placed texture with a converted file.
        static const bool s_log = std::getenv("VP2S_GAME_TEXTURES_LOG") != nullptr;
        static uint64_t s_lookups = 0, s_unplaced = 0, s_unconverted = 0;
        if (s_log && ++s_lookups % 100000u == 0u)
            std::printf("[vp2s-textures] %llu lookups: %llu at a TBP the game's cache did not give, %llu not converted, %zu placed, %zu files\n",
                        static_cast<unsigned long long>(s_lookups), static_cast<unsigned long long>(s_unplaced),
                        static_cast<unsigned long long>(s_unconverted), g_placed.size(), g_files.size());
        const auto placed = g_placed.find(tbp);
        if (placed == g_placed.end())
        {
            ++s_unplaced;
            return nullptr;
        }
        const uint32_t csa = static_cast<uint32_t>((tex0 >> 56) & 0x1Fu);
        const uint32_t tw = static_cast<uint32_t>((tex0 >> 26) & 0xFu), th = static_cast<uint32_t>((tex0 >> 30) & 0xFu);
        const uint64_t id = ((uint64_t{placed->second.tag} << 32) | placed->second.index) * 0x9E3779B97F4A7C15ull ^
                            (uint64_t{csa} << 8 | tw << 4 | th);
        if (const auto it = g_textures.find(id); it != g_textures.end())
            return it->second;
        const std::shared_ptr<Converted> converted = load(placed->second);
        if (!converted)
        {
            ++s_unconverted;
            g_textures[id] = nullptr;
            return nullptr;
        }
        ps2x::native::TextureCache::Texture texture;
        texture.width = 1u << std::min<uint32_t>(10u, tw);
        texture.height = 1u << std::min<uint32_t>(10u, th);
        texture.key = id;
        texture.version = ++g_version;
        texture.rgba.assign(static_cast<size_t>(texture.width) * texture.height, 0u);
        for (uint32_t y = 0; y < texture.height && y < converted->height; ++y)
            for (uint32_t x = 0; x < texture.width && x < converted->width; ++x)
            {
                const size_t k = static_cast<size_t>(y) * converted->width + x;
                uint32_t rgba;
                if (converted->psm == 0u)
                    std::memcpy(&rgba, converted->texels.data() + k * 4u, 4u);
                else
                {
                    const uint32_t index = converted->texels[k];
                    rgba = converted->palette[(converted->psm == 0x14u ? csa * 16u + index : index) & 0xFFu];
                }
                texture.rgba[static_cast<size_t>(y) * texture.width + x] = rgba;
            }
        auto shared = std::make_shared<const ps2x::native::TextureCache::Texture>(std::move(texture));
        g_textures[id] = shared;
        // VP2S_GAME_TEXTURES_COMPARE: each new texture against the one the uploads made (diagnosis only): the keys
        // whose conversion differs, with how much.
        static const bool s_compare = std::getenv("VP2S_GAME_TEXTURES_COMPARE") != nullptr;
        if (s_compare)
            if (const auto reference = ps2x::native::textures().uploadedTexture(tex0, 0); reference && reference->rgba.size() == shared->rgba.size())
            {
                size_t wrong = 0;
                for (size_t i = 0; i < shared->rgba.size(); ++i)
                    wrong += reference->rgba[i] != shared->rgba[i];
                std::printf("[vp2s-textures] %08x_%x tex0 %016llx: %zu of %zu texels differ%s\n", placed->second.tag, placed->second.index,
                            static_cast<unsigned long long>(tex0), wrong, shared->rgba.size(), wrong ? "" : " (same)");
                // VP2S_GAME_TEXTURES_COMPARE=<dir>: the first 20 that differ written there as <n>_ours.rgba / <n>_uploads.rgba
                // (u32 width, height, then RGBA8).
                static int s_written = 0;
                const char *dir = std::getenv("VP2S_GAME_TEXTURES_COMPARE");
                if (wrong && dir[0] != '1' && s_written < 20)
                {
                    const int n = s_written++;
                    for (int which = 0; which < 2; ++which)
                    {
                        char path[600];
                        std::snprintf(path, sizeof(path), "%s/%02d_%08x_%x_%s.rgba", dir, n, placed->second.tag, placed->second.index,
                                      which ? "uploads" : "ours");
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            const auto &t = which ? *reference : *shared;
                            std::fwrite(&t.width, 4, 1, f);
                            std::fwrite(&t.height, 4, 1, f);
                            std::fwrite(t.rgba.data(), 4, t.rgba.size(), f);
                            std::fclose(f);
                        }
                    }
                }
            }
        return shared;
    }
}

void vp2sInstallGameTextures()
{
    // Not yet the default: the field's map textures come by a path neither hook sees, so the field would get the
    // textures placed there before. VP2S_GAME_TEXTURES=1 turns it on (to test).
    if (const char *v = std::getenv("VP2S_GAME_TEXTURES"); !v || v[0] != '1')
        return;
    const ps2_overlays::Module *module = ps2_overlays::findModule("mod001");
    if (!module || kTextureCache < module->tableBase || kTextureCache >= module->tableEnd)
        return;
    g_original = module->table[(kTextureCache - module->tableBase) >> 2];
    if (!g_original || !ps2_overlays::replaceModuleFunction("mod001", kTextureCache, &textureCacheHook))
        return;
    g_allocate = module->table[(kTextureAllocate - module->tableBase) >> 2];
    if (g_allocate)
        ps2_overlays::replaceModuleFunction("mod001", kTextureAllocate, &textureAllocateHook);
    ps2x::native::textures().setProvider(&provide, false);
    std::printf("[vp2s] native textures from the game's texture cache and the offline conversion (%s)\n", directory().c_str());
}
