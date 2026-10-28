// Game-specific runtime setup for Valkyrie Profile 2: Silmeria (SLES-54645).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
    // The game reads its hidden archive by raw sector (sceCdRead), so the CD device
    // needs the disc image itself: VP2S_ISO, else the first .iso found walking up
    // from the ELF directory (build/extract/iso -> project root).
    std::filesystem::path findDiscImage(const std::filesystem::path &elfDirectory)
    {
        if (const char *configured = std::getenv("VP2S_ISO"); configured && configured[0] != '\0')
        {
            return configured;
        }
        std::filesystem::path dir = elfDirectory;
        for (int depth = 0; depth < 5 && !dir.empty(); ++depth, dir = dir.parent_path())
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".iso")
                {
                    return entry.path();
                }
            }
            if (dir == dir.root_path())
            {
                break;
            }
        }
        return {};
    }

    // The game uses nearly all user RAM: its heap runs from the end of the overlay area
    // (0x549100) up to the main thread stack (0x1FF0000), where the runtime keeps its HLE
    // pools and invocation stacks by default. Move those into the HLE-unused kernel memory
    // below the ELF and let a rest-of-RAM heap reach the stack, as on the console. The range starts at 64 KiB:
    // past the 256 KiB of HLE pools it holds the 16 KiB invocation stacks the scheduler keeps per thread and nesting
    // depth, and from 0x80000 it fitted only 16 of them (the scene after the first field skip ran out).
    constexpr uint32_t kReservedBase = 0x00010000u;
    constexpr uint32_t kReservedTop = 0x00100000u;
    constexpr uint32_t kHeapLimit = 0x01FF0000u;

    // Guest register and memory helpers for the native replacements below.
    uint32_t reg(const R5900Context *ctx, int index)
    {
        return getRegU32(ctx, index);
    }

    void setReg(R5900Context *ctx, int index, uint32_t value)
    {
        ctx->r[index] = _mm_insert_epi64(ctx->r[index], static_cast<int64_t>(static_cast<int32_t>(value)), 0);
    }

    uint8_t *guest(uint8_t *rdram, uint32_t address)
    {
        return rdram + (address & PS2_RAM_MASK);
    }

    // FUN_0011f520(object, count): zero the vblank flag at +0xC, spin until the vblank handler sets it (count 0) or
    // until the counter at +0x10 reaches count, return the field byte at +0x2FA; count -1 only returns the byte.
    constexpr uint32_t kFrameWait = 0x0011F520u;
    constexpr uint32_t kFrameWaitCount = 0x0011F538u; // the count > 0 path, left to the recompiled code

    // VP2's field loop (mod003 FUN_003d72e0, its object at *0x48DB48) draws one frame per 0x98 vblanks (2: 25 fps in
    // PAL) and scales its logic by the global step it derives each frame from 0xDC (also 2): dt = 0xDC * 1.2 *
    // 0xA0 * 0xA8 frames of 1/60 s, at 0x35DED8 (and /60 in seconds at 0x35DEE8). FUN_0011f510 returns the vblanks
    // elapsed since the last frame (read and cleared once per frame, called only by that loop).
    constexpr uint32_t kElapsedVBlanks = 0x0011F510u;
    constexpr uint32_t kFieldLoopObject = 0x0048DB48u;

    uint32_t load32(uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, guest(rdram, address), sizeof(value));
        return value;
    }

    void store32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(guest(rdram, address), &value, sizeof(value));
    }

    // Native loop (VP2S_NATIVE=1): no vblank in VP2's frame pacing. The flip's frame wait (FUN_0011f520 with count
    // 0) becomes the frame end: the runtime's FramePacer holds it to the frame cap (PS2X_FRAME_CAP, 0 = unlimited)
    // and the frame is shown at once. The field loop then counts one vblank per frame, and its step is the real
    // time the frame took: 0x35DED8 = seconds * 60 * 0xA0 * 0xA8 (1/60 s units, as the game's own 0xDC * 1.2 *
    // 0xA0 * 0xA8 with 0xDC PAL vblanks), 0x35DEE8 the same in seconds.
    constexpr uint32_t kStepFrames = 0x0035DED8u;
    constexpr uint32_t kStepSeconds = 0x0035DEE8u;
    constexpr double kEeClockHz = 294912000.0;
    double g_frameSeconds = 1.0 / 60.0;

    float loadFloat(uint8_t *rdram, uint32_t address)
    {
        float value = 0.0f;
        std::memcpy(&value, guest(rdram, address), sizeof(value));
        return value;
    }

    void storeFloat(uint8_t *rdram, uint32_t address, float value)
    {
        std::memcpy(guest(rdram, address), &value, sizeof(value));
    }

    void nativeFrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t object = reg(ctx, 4);
        const int32_t count = static_cast<int32_t>(reg(ctx, 5));
        if (count > 0)
        {
            ctx->pc = kFrameWaitCount;
            return;
        }
        std::memset(guest(rdram, 0x001D8E80u), 0, 4);
        if (count == 0)
        {
            g_frameSeconds = runtime->framePacer().frameEnd();
            // The game no longer spins its wait, so EE time would fall behind real time and the events it waits on
            // (vblank handlers, timers, the IOP's loads) would stall: charge what the frame did not account.
            EeScheduler &scheduler = runtime->eeScheduler();
            static uint64_t s_lastFrameCycle = scheduler.currentCycle();
            const uint64_t accounted = scheduler.currentCycle() - s_lastFrameCycle;
            const uint64_t realCycles = static_cast<uint64_t>(g_frameSeconds * kEeClockHz);
            if (realCycles > accounted)
                scheduler.accountCycles(static_cast<uint32_t>(std::min<uint64_t>(realCycles - accounted, 0xFFFFFFFFu)));
            s_lastFrameCycle = scheduler.currentCycle();
            scheduler.presentGuestFrame();
        }
        setReg(ctx, 2, *guest(rdram, object + 0x2FAu));
        ctx->pc = reg(ctx, 31);
    }

    void nativeElapsed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t counter = reg(ctx, 4);
        store32(rdram, counter + 0x10u, 0u);
        setReg(ctx, 2, 1u);
        ctx->pc = reg(ctx, 31);

        const uint32_t loop = load32(rdram, kFieldLoopObject);
        if (loop < 0x00100000u || loop >= 0x02000000u)
            return;
        const uint32_t interval = load32(rdram, loop + 0x98u);
        if (interval < 1u || interval > 4u)
            return;
        store32(rdram, loop + 0x98u, 1u);
        *guest(rdram, loop + 0xDCu) = 1u;
        const float scale = loadFloat(rdram, loop + 0xA0u) * loadFloat(rdram, loop + 0xA8u);
        const float seconds = static_cast<float>(g_frameSeconds) * scale;
        storeFloat(rdram, kStepFrames, seconds * 60.0f);
        storeFloat(rdram, kStepSeconds, seconds);
    }

    void applyVp2s(PS2Runtime &runtime)
    {
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kHeapLimit);

        // VP2 waits for its vblank by spinning on a counter in FUN_0011f520 (main thread, 0x11f520-0x11f5b4), never in
        // the kernel: tell the scheduler, or it takes the picture only every third vblank (16 fps shown in towns
        // where the game draws ~45). VP2S_VBLANK_HOLD=1 also holds a vblank while the frame is still being drawn.
        {
            const char *hold = std::getenv("VP2S_VBLANK_HOLD");
            runtime.eeScheduler().setGuestFrameWait(1, 0x0011F520u, 0x0011F5B4u, hold && hold[0] == '1');
            if (const char *limit = std::getenv("VP2S_VBLANK_HOLD_US"))
                runtime.eeScheduler().setVBlankHoldLimit(std::chrono::microseconds(std::strtoul(limit, nullptr, 10)));
        }

        if (const char *native = std::getenv("VP2S_NATIVE"); native && native[0] == '1')
        {
            runtime.eeScheduler().setNativeFrames(true);
            runtime.replaceFunction(kFrameWait, &nativeFrameEnd);
            runtime.replaceFunction(kElapsedVBlanks, &nativeElapsed);
            std::cout << "[vp2s] native loop: frames paced by the host (cap " << runtime.framePacer().cap()
                      << " fps, 0 = unlimited), real-time step" << std::endl;
        }

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = findDiscImage(paths.elfDirectory);
        if (paths.cdImage.empty())
        {
            std::cerr << "[vp2s] no disc image found: set VP2S_ISO to the game ISO" << std::endl;
            return;
        }
        std::cout << "[vp2s] disc image: " << paths.cdImage.string() << std::endl;
        PS2Runtime::setIoPaths(paths);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Valkyrie Profile 2: Silmeria (PAL)", "SLES_546.45", 0x00100008u, 0u, &applyVp2s)
