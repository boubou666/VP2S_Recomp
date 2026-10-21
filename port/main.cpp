// Entry point of the Valkyrie Profile 2: Silmeria native port.
//
// Usage: vp2s.exe <path to SLES_546.45> (the boot ELF extracted by tools/pipeline.py).
// The disc image is found by the game override (vp2s_overrides.cpp, VP2S_ISO).
// VP2S_DIAG=1 prints the resident overlay modules, guest threads, IOP/RPC and GS state and saves
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
void printGsDiagnostics(PS2Runtime &runtime);  // vp2s_diag_gs.cpp

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

        // CD manager (global 0x1c8c80 + 0x17c0, see FUN_00101050). FUN_0011dfe0 starts a read:
        // busy+9183=1, pending+5c=1, then signals the CD thread through the semaphore at +0x10.
        constexpr uint32_t kCdManager = 0x001ca440u;
        {
            auto u32 = [&](uint32_t off) { uint32_t v; std::memcpy(&v, rdram + kCdManager + off, 4); return v; };
            auto sema = [&](uint32_t off)
            {
                const int id = static_cast<int>(u32(off));
                std::string text = std::to_string(id);
                if (const EeSemaphore *s = runtime.eeScheduler().semaphore(id))
                {
                    text += "(" + std::to_string(s->count) + "/" + std::to_string(s->maxCount) + " w" +
                            std::to_string(s->waiters.size()) + ")";
                }
                return text;
            };
            std::cout << "[vp2s-diag]   cd manager sema+10=" << sema(0x10) << " sema+18=" << sema(0x18)
                      << " sema+28=" << sema(0x28) << " pending+5c=" << int(rdram[kCdManager + 0x5c])
                      << " +5e=" << int(rdram[kCdManager + 0x5e]) << " +68/69/6a=" << int(rdram[kCdManager + 0x68])
                      << "/" << int(rdram[kCdManager + 0x69]) << "/" << int(rdram[kCdManager + 0x6a])
                      << " enabled+9181=" << int(rdram[kCdManager + 0x9181])
                      << " busy+9183=" << int(rdram[kCdManager + 0x9183]) << std::hex << " lba+84=0x" << u32(0x84)
                      << " pos+88=0x" << u32(0x88) << " end+8c=0x" << u32(0x8c) << " start+9194=0x" << u32(0x9194)
                      << " count+91a0=0x" << u32(0x91a0) << " c4=0x" << u32(0xc4) << std::dec
                      << " stream+c8/c9/9186=" << int(rdram[kCdManager + 0xc8]) << "/" << int(rdram[kCdManager + 0xc9])
                      << "/" << int(rdram[kCdManager + 0x9186]) << std::endl;
        }

        // File request manager (pointer at global+8): request queue pumped by FUN_00102e40 and
        // FUN_001032a0. Entries are 0x2c bytes; entry 0 heads a list linked through +0x14.
        constexpr uint32_t kGlobal = 0x001c8c80u;
        {
            auto rd32 = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x1ffffffu), 4); return v; };
            const uint32_t files = rd32(kGlobal + 8);
            std::cout << "[vp2s-diag]   boot state+110=" << int(rdram[kGlobal + 0x110]) << std::hex
                      << " file manager 0x" << files << std::dec;
            if (files >= 0x100000u && files < PS2_RAM_SIZE)
            {
                const uint32_t table = rd32(files + 0x14);
                std::cout << " count=" << rd32(files + 0x18) << " current=" << rd32(files + 0x1c)
                          << " +2c..30=" << int(rdram[files + 0x2c]) << "/" << int(rdram[files + 0x2d]) << "/"
                          << int(rdram[files + 0x2e]) << "/" << int(rdram[files + 0x2f]) << "/" << int(rdram[files + 0x30])
                          << " queue:";
                uint32_t index = rd32(table + 0x14);
                for (int guard = 0; index != 0u && guard < 16; ++guard)
                {
                    const uint32_t entry = table + index * 0x2cu;
                    std::cout << " [#" << index << " type=" << rd32(entry) << " file=" << rd32(entry + 0x10)
                              << " kind=" << int(rdram[entry + 0x2a]) << " used=" << int(rdram[entry + 0x28])
                              << " started=" << int(rdram[entry + 0x29]) << std::hex << " buf=0x" << rd32(entry + 4)
                              << std::dec << "]";
                    index = rd32(entry + 0x14);
                }
            }
            std::cout << std::endl;
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

            // Sound bank transfer queue of the sound manager (*global): ring +0x194..+0x1a0, read
            // +0x198, write +0x19c, sub-state +0x148 (FUN_001091a0; 5 waits for the IOP's SIF command).
            auto rd32 = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x1ffffffu), 4); return v; };
            const uint32_t sound = rd32(kGlobal);
            if (sound >= 0x100000u && sound < PS2_RAM_SIZE)
            {
                std::cout << std::hex << "[vp2s-diag]   sound manager 0x" << sound << " transfer state+148=" << std::dec
                          << int(rdram[sound + 0x148]) << std::hex << " handle+150=0x" << rd32(sound + 0x150)
                          << " iop+14c=0x" << rd32(sound + 0x14c) << " read=0x" << rd32(sound + 0x198) << " write=0x"
                          << rd32(sound + 0x19c);
                const uint32_t head = rd32(sound + 0x198);
                if (head != rd32(sound + 0x19c) && head >= 0x100000u && head < PS2_RAM_SIZE)
                {
                    std::cout << " head{left=0x" << rd32(head) << " src=0x" << rd32(head + 4) << " buf=0x" << rd32(head + 8) << "}";
                }
                std::cout << std::dec << std::endl;
            }
        }
        printIopDiagnostics(runtime);
        printGsDiagnostics(runtime);

        // VP2S_DUMP=<address>:<size> saves that guest memory to vp2s_dump.bin (overwritten each time).
        if (const char *dump = std::getenv("VP2S_DUMP"))
        {
            const unsigned long address = std::strtoul(dump, nullptr, 0) & PS2_RAM_MASK;
            const char *colon = std::strchr(dump, ':');
            const unsigned long size = colon ? std::strtoul(colon + 1, nullptr, 0) : 0u;
            if (size != 0u && address + size <= PS2_RAM_SIZE)
            {
                if (std::FILE *out = std::fopen("vp2s_dump.bin", "wb"))
                {
                    std::fwrite(rdram + address, 1, size, out);
                    std::fclose(out);
                }
            }
        }
    }

    void initDiagnostics(PS2Runtime &runtime, void *userData)
    {
        auto &diag = *static_cast<Diagnostics *>(userData);
        if (diag.enabled)
        {
            runtime.gs().setDebugHistoryPaused(false); // GS event history for [vp2s-gs]
        }
#if defined(PS2X_ENABLE_DEBUG_UI)
        diag.panel.initialize();
        diag.panel.setVisible(false); // F1 shows it; keeps the game screen (and diagnostic screenshots) clear
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
