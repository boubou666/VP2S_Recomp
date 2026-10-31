#include "vp2s_vu1_programs.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    struct Registered
    {
        const uint16_t *ranges = nullptr;
        size_t rangeCount = 0;
        uint64_t hash = 0;
        Vp2sVu1Program program = nullptr;
    };

    std::unordered_map<uint32_t, std::vector<Registered>> &programs()
    {
        static std::unordered_map<uint32_t, std::vector<Registered>> table; // by start pc
        return table;
    }
}

uint32_t vp2sVu1ImageHash(const uint8_t *code)
{
    // FNV-1a 64 over the 2048 instruction pairs, folded to 32 bits: a couple of microseconds a run.
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < 16384u; i += 8u)
    {
        uint64_t pair;
        std::memcpy(&pair, code + i, 8u);
        hash = (hash ^ pair) * 1099511628211ull;
    }
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}

uint64_t vp2sVu1CodeHash(const uint8_t *code, const uint16_t *ranges, size_t rangeCount)
{
    // FNV-1a 64 over the reached pairs, in pc order.
    uint64_t hash = 1469598103934665603ull;
    for (size_t r = 0; r + 1u < rangeCount * 2u; r += 2u)
        for (uint32_t pair = ranges[r]; pair < static_cast<uint32_t>(ranges[r]) + ranges[r + 1u]; ++pair)
        {
            uint64_t value;
            std::memcpy(&value, code + (pair & 0x7FFu) * 8u, 8u);
            hash = (hash ^ value) * 1099511628211ull;
        }
    return hash;
}

bool vp2sRegisterVu1Program(uint32_t entry, const uint16_t *ranges, size_t rangeCount, uint64_t hash, Vp2sVu1Program program)
{
    programs()[entry].push_back({ranges, rangeCount, hash, program});
    return true;
}

Vp2sVu1Program vp2sVu1Program(const uint8_t *code, uint32_t pc)
{
    if (!code)
        return nullptr;
    // Filled before the game runs (static registration), read only afterwards.
    const auto found = programs().find(pc);
    if (found == programs().end())
        return nullptr;
    for (const Registered &candidate : found->second)
        if (vp2sVu1CodeHash(code, candidate.ranges, candidate.rangeCount) == candidate.hash)
            return candidate.program;
    return nullptr;
}
