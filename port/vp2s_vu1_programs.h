#pragma once
// VP2's VU1 microprograms as C++ (tools/vu1_native.py: each code image the game loads lifted by the fork's
// tools/vu1_lift.py into build/vu1_native, game code like the EE output, never committed). A generated file registers
// each program here by its start pc and the code it reaches; vp2sVu1Hle runs them in place of VU1 for the programs it
// does not draw natively.

#include "runtime/ps2_vu1.h"

#include <cstddef>
#include <cstdint>

using Vp2sVu1Program = uint64_t (*)(uint32_t entry, VU1State &state, uint8_t *data, uint32_t top,
                                    void (*kick)(void *user, const uint8_t *packet, uint32_t bytes), void *user);

// The name of a code image (its 16 KiB), for logs and the images a run saves.
uint32_t vp2sVu1ImageHash(const uint8_t *code);

// A program: its start pc, the instruction pairs it can reach (ranges of pairs: first pc / 8, count, ...) and their
// hash (vp2sVu1CodeHash), so a code memory holding the same program among other bytes still finds it.
bool vp2sRegisterVu1Program(uint32_t entry, const uint16_t *ranges, size_t rangeCount, uint64_t hash, Vp2sVu1Program program);
uint64_t vp2sVu1CodeHash(const uint8_t *code, const uint16_t *ranges, size_t rangeCount);

// The program for a run of `code` from `pc`, or nullptr.
Vp2sVu1Program vp2sVu1Program(const uint8_t *code, uint32_t pc);
