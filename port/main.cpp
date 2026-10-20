// Entry point of the Valkyrie Profile 2: Silmeria native port.
//
// Usage: vp2s.exe <path to SLES_546.45> (the boot ELF extracted by tools/pipeline.py).
// The disc image is found by the game override (vp2s_overrides.cpp, VP2S_ISO).
// VP2S_DIAG=1 prints the resident overlay modules, guest threads and IOP/RPC state and saves
// a screenshot every few seconds. Builds with VP2S_DEBUG_UI show the runtime debugger (F1).

#include "ps2_overlay_modules.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include "raylib.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

void printIopDiagnostics(PS2Runtime &runtime); // vp2s_diag_iop.cpp

namespace
{
    constexpr int kDiagIntervalFrames = 300; // 5 s at 60 fps

    // Overlay slots of the game (slot table at 0x1c8a30 in the ELF, docs/AUDIT.md).
    struct Slot
    {
        const char *role;
        uint32_t address;
    };
    constexpr Slot kSlots[] = {
        {"core", 0x001e7d80u},
        {"title/field", 0x0035ec80u},
        {"scene", 0x0048ea00u},
        {"camp", 0x00495000u},
    };

    struct Diagnostics
    {
        bool enabled = false;
        int frame = 0;
        int shot = 0;
#if defined(PS2X_ENABLE_DEBUG_UI)
        PS2DebugPanel panel;
#endif
    };

    const char *statusName(EeThreadStatus status)
    {
        static const char *names[] = {"run", "ready", "wait", "wait+susp", "susp", "dormant"};
        const auto index = static_cast<size_t>(status);
        return index < std::size(names) ? names[index] : "?";
    }

    const char *waitName(EeWaitReason reason)
    {
        static const char *names[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
        const auto index = static_cast<size_t>(reason);
        return index < std::size(names) ? names[index] : "?";
    }

    // Every guest thread with where it stands, and semaphores with waiters.
    void printScheduler(PS2Runtime &runtime)
    {
        const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
        for (const EeThreadSnapshot &thread : snap.threads)
        {
            std::cout << "[vp2s-diag]   thread " << thread.id << (thread.id == snap.runningThreadId ? "*" : "")
                      << " " << statusName(thread.status) << "/" << waitName(thread.waitReason);
            if (thread.waitReason != EeWaitReason::None)
            {
                std::cout << "#" << thread.waitId;
            }
            std::cout << std::hex << " pc=0x" << thread.pc << " ra=0x" << thread.ra << " entry=0x" << thread.entry
                      << std::dec << " prio=" << thread.currentPriority << std::endl;
        }
        for (const EeSemaphoreSnapshot &sema : snap.semaphores)
        {
            if (sema.waiters != 0u)
            {
                std::cout << "[vp2s-diag]   sema " << sema.id << " count=" << sema.count << "/" << sema.maxCount
                          << " waiters=" << sema.waiters << std::endl;
            }
        }
    }

    void drawDiagnostics(PS2Runtime &runtime, void *userData)
    {
        auto &diag = *static_cast<Diagnostics *>(userData);
#if defined(PS2X_ENABLE_DEBUG_UI)
        diag.panel.draw(runtime);
#endif
        if (!diag.enabled || ++diag.frame % kDiagIntervalFrames != 0)
        {
            return;
        }
        const uint8_t *rdram = runtime.memory().getRDRAM();
        const R5900Context &cpu = runtime.cpu();
        std::cout << "[vp2s-diag] frame " << diag.frame << std::hex << ": pc=0x" << cpu.pc
                  << " ra=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[31]))
                  << " sp=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[29])) << std::dec;
        for (const Slot &slot : kSlots)
        {
            // Module code starts 0x80 bytes after its in-memory header.
            const ps2_overlays::Module *module = ps2_overlays::residentModuleFor(rdram, slot.address + 0x80u);
            std::cout << " " << slot.role << "=" << (module ? module->name : "-");
        }
        const std::string file = "vp2s_diag_" + std::to_string(diag.shot++) + ".png";
        TakeScreenshot(file.c_str());
        std::cout << " screenshot=" << file << std::endl;
        printScheduler(runtime);

        // CD manager waited on by FUN_0011ebc0 (pointer in the main thread's s1).
        const uint32_t manager = static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[17]));
        if (manager >= 0x100000u && manager + 0x9200u < PS2_RAM_SIZE)
        {
            auto u32 = [&](uint32_t off) { uint32_t v; std::memcpy(&v, rdram + manager + off, 4); return v; };
            std::cout << "[vp2s-diag]   cd manager 0x" << std::hex << manager << std::dec
                      << " sema+10=" << u32(0x10) << " sema+18=" << u32(0x18) << " sema+28=" << u32(0x28)
                      << " pending+5c=" << int(rdram[manager + 0x5c]) << " enabled+9181=" << int(rdram[manager + 0x9181])
                      << " busy+9183=" << int(rdram[manager + 0x9183]) << " lba+17c=0x" << std::hex << u32(0x17c)
                      << std::dec << std::endl;
        }
        // Sound RPC worker object (thread FUN_001086a0): request queue and its semaphore.
        constexpr uint32_t kSound = 0x001d5990u;
        {
            auto u32 = [&](uint32_t off) { uint32_t v; std::memcpy(&v, rdram + kSound + off, 4); return v; };
            const int semaId = static_cast<int>(u32(0x340));
            std::cout << "[vp2s-diag]   sound worker read=" << u32(0x18) << " write=" << u32(0x1c)
                      << " idle+2dc=" << int(rdram[kSound + 0x2dc]) << " sema+340=" << semaId;
            if (const EeSemaphore *sema = runtime.eeScheduler().semaphore(semaId))
            {
                std::cout << " count=" << sema->count << "/" << sema->maxCount << " init=" << sema->initCount;
            }
            else
            {
                std::cout << " (no such semaphore)";
            }
            std::cout << std::endl;
        }
        printIopDiagnostics(runtime);
    }

    void initDiagnostics(PS2Runtime &, void *userData)
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        auto &panel = static_cast<Diagnostics *>(userData)->panel;
        panel.initialize();
        panel.setVisible(false); // F1 shows it; keeps the game screen (and diagnostic screenshots) clear
#else
        (void)userData;
#endif
    }

    void shutdownDiagnostics(PS2Runtime &, void *userData)
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        static_cast<Diagnostics *>(userData)->panel.shutdown();
#else
        (void)userData;
#endif
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argv[1][0] == '\0')
    {
        std::cerr << "usage: vp2s <path to SLES_546.45>" << std::endl;
        return 2;
    }
    try
    {
        const std::filesystem::path elf = argv[1];
        PS2Runtime runtime;

        Diagnostics diagnostics;
        const char *diag = std::getenv("VP2S_DIAG");
        diagnostics.enabled = diag && std::strcmp(diag, "0") != 0;
#if defined(PS2X_ENABLE_DEBUG_UI)
        constexpr bool kDebugUi = true;
#else
        constexpr bool kDebugUi = false;
#endif
        if (diagnostics.enabled || kDebugUi)
        {
            // The init callback must be set before initialize() for the draw callback to run.
            runtime.setDebugUiCallbacks(&initDiagnostics, &drawDiagnostics, &shutdownDiagnostics, &diagnostics);
        }

        if (!runtime.initialize("Valkyrie Profile 2: Silmeria | VP2S_Recomp"))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "Failed to load ELF file: " << elf.string() << std::endl;
            return 1;
        }
        runtime.run();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
