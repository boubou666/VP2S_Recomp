// Entry point of the Valkyrie Profile 2: Silmeria native port.
//
// Usage: vp2s.exe <path to SLES_546.45> (the boot ELF extracted by tools/pipeline.py).
// The disc image is found by the game override (vp2s_overrides.cpp, VP2S_ISO).
// VP2S_DIAG=1 prints the resident overlay modules, guest threads, IOP/RPC and GS state and saves
// a screenshot of the game screen every few seconds; F9 saves one at any time. Builds with
// VP2S_DEBUG_UI show the runtime debugger (F1).

#include "ps2_overlay_modules.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include "raylib.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

void printIopDiagnostics(PS2Runtime &runtime); // vp2s_diag_iop.cpp
void printGsDiagnostics(PS2Runtime &runtime);  // vp2s_diag_gs.cpp
void startProfiler();                          // vp2s_profile.cpp
void startHangWatch(const std::atomic<uint64_t> &heartbeat); // vp2s_profile.cpp

namespace
{
    constexpr int kDiagIntervalFrames = 300; // 5 s at 60 fps
    std::atomic<uint64_t> g_hostFrames{0};   // heartbeat for VP2S_HANG_DUMP

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
        int manualShot = 0;
        // Distinct presented frames (the game's own frame rate: VP2 redraws one display buffer, so no flips).
        uint64_t distinctFrames = 0;
        uint64_t lastFrameHash = 0;
        std::vector<uint8_t> framePixels;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
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
                      << std::dec << " prio=" << thread.currentPriority << " wakeups=" << thread.wakeupCount;
            // Words on the thread's stack that look like return addresses into game code: a rough
            // call chain for a thread blocked in a kernel call.
            if (thread.sp >= 0x00100000u && thread.sp < 0x02000000u)
            {
                const uint8_t *rdram = runtime.memory().getRDRAM();
                std::cout << std::hex << " sp=0x" << thread.sp << " stack:";
                int shown = 0;
                for (uint32_t offset = 0; offset < 0x400u && shown < 10; offset += 4u)
                {
                    uint32_t word = 0;
                    std::memcpy(&word, rdram + ((thread.sp + offset) & PS2_RAM_MASK), sizeof(word));
                    if (word >= 0x00100008u && word < 0x00600000u && (word & 3u) == 0u)
                    {
                        std::cout << " 0x" << word;
                        ++shown;
                    }
                }
                std::cout << std::dec;
            }
            std::cout << std::endl;
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

    // Saves the last frame the runtime's GS presented (what the window shows, without the
    // debugger overlay), whatever the window state.
    bool saveGameFrame(PS2Runtime &runtime, const std::string &file)
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!runtime.gsForPresentation().copyLatchedHostPresentationFrame(pixels, width, height) ||
            pixels.size() < static_cast<size_t>(width) * height * 4u)
        {
            return false;
        }
        Image image{};
        image.data = pixels.data();
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        return ExportImage(image, file.c_str());
    }

    void drawDiagnostics(PS2Runtime &runtime, void *userData)
    {
        auto &diag = *static_cast<Diagnostics *>(userData);
        g_hostFrames.fetch_add(1, std::memory_order_relaxed);
#if defined(PS2X_ENABLE_DEBUG_UI)
        diag.panel.draw(runtime);
#endif
        if (IsKeyPressed(KEY_F9)) // F9: save the game screen (F12 is raylib's window capture)
        {
            const std::string file = "vp2s_shot_" + std::to_string(diag.manualShot++) + ".png";
            std::cout << "[vp2s] " << (saveGameFrame(runtime, file) ? "saved " : "no frame for ") << file << std::endl;
        }
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        if (runtime.gsForPresentation().copyLatchedHostPresentationFrame(diag.framePixels, frameWidth, frameHeight) &&
            diag.framePixels.size() >= 4u)
        {
            // FNV-1a over a sparse sample of pixels: enough to tell a redrawn frame from a repeated one.
            uint64_t hash = 1469598103934665603ull;
            const size_t words = diag.framePixels.size() / 4u;
            for (size_t index = 0; index < words; index += 61u)
            {
                uint32_t word = 0;
                std::memcpy(&word, diag.framePixels.data() + index * 4u, sizeof(word));
                hash = (hash ^ word) * 1099511628211ull;
            }
            if (hash != diag.lastFrameHash)
            {
                ++diag.distinctFrames;
                diag.lastFrameHash = hash;
                runtime.noteGameFrame(); // the fps overlay's frame count (PS2X_SHOW_FPS, F3)
            }
        }
        if (!diag.enabled)
        {
            return;
        }
        if (++diag.frame % kDiagIntervalFrames != 0)
        {
            return;
        }
        const uint8_t *rdram = runtime.memory().getRDRAM();
        const R5900Context &cpu = runtime.cpu();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - diag.start).count();
        const uint64_t vsyncs = runtime.eeScheduler().currentVSyncTick();
        std::cout << "[vp2s-diag] frame " << diag.frame << " t=" << static_cast<int>(seconds) << "s vsync=" << vsyncs
                  << " (" << static_cast<int>(vsyncs / std::max(seconds, 1.0)) << "/s) flips="
                  << runtime.gsForPresentation().displayFlips() << " frames=" << diag.distinctFrames << " latches=" << runtime.gsForPresentation().queuedLatchesDone()
                  << std::hex << ": pc=0x" << cpu.pc
                  << " ra=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[31]))
                  << " sp=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[29])) << std::dec;
        if (cpu.pc == 0u)
        {
            // The main thread jumped to 0: show the object of the failed virtual call ($s1/$a0,
            // vtable at +0) and what was loaded around it.
            auto reg = [&](int index) { return static_cast<uint32_t>(_mm_cvtsi128_si32(cpu.r[index])); };
            auto word = [&](uint32_t address) { uint32_t value = 0; std::memcpy(&value, rdram + (address & PS2_RAM_MASK), 4); return value; };
            std::cout << std::hex << " [null call: a0=0x" << reg(4) << " s0=0x" << reg(16) << " s1=0x" << reg(17) << " t9=0x" << reg(25);
            const uint32_t object = reg(17);
            std::cout << " obj:";
            for (uint32_t i = 0; i < 8u; ++i)
                std::cout << " " << word(object + i * 4u);
            const uint32_t vtable = word(object);
            std::cout << " vtable 0x" << vtable << ":";
            for (uint32_t i = 0; i < 28u; ++i)
                std::cout << " " << word(vtable + i * 4u);
            std::cout << "]" << std::dec;
        }
        for (const Slot &slot : kSlots)
        {
            // Module code starts 0x80 bytes after its in-memory header.
            const ps2_overlays::Module *module = ps2_overlays::residentModuleFor(rdram, slot.address + 0x80u);
            std::cout << " " << slot.role << "=" << (module ? module->name : "-");
            auto word = [&](uint32_t address) { uint32_t value = 0; std::memcpy(&value, rdram + (address & PS2_RAM_MASK), 4); return value; };
            if (!module && word(slot.address + 8u) == slot.address)
            {
                // A module the port does not know: its header (load, text, data, bss) and a hash of its code.
                const uint32_t text = word(slot.address + 12u);
                uint64_t hash = 1469598103934665603ull;
                for (uint32_t off = 0x80u; off < text && off < 0x100000u; ++off)
                    hash = (hash ^ rdram[(slot.address + off) & PS2_RAM_MASK]) * 1099511628211ull;
                const uint32_t data = word(slot.address + 16u);
                std::cout << std::hex << "(unknown: text=0x" << text << " data=0x" << data
                          << " bss=0x" << word(slot.address + 20u) << " fnv=" << hash << ")" << std::dec;
                // Saved once, in the extractor's module format (header included), to be wrapped and recompiled:
                // docs/AUDIT.md's slots declared but never found on the disc.
                static std::vector<uint64_t> saved;
                const uint32_t size = text + data;
                if (std::find(saved.begin(), saved.end(), hash) == saved.end() && size <= 0x200000u &&
                    (slot.address & PS2_RAM_MASK) + size <= PS2_RAM_SIZE)
                {
                    saved.push_back(hash);
                    char name[64];
                    std::snprintf(name, sizeof(name), "vp2s_module_%08x_%016llx.bin", slot.address,
                                  static_cast<unsigned long long>(hash));
                    if (std::FILE *out = std::fopen(name, "wb"))
                    {
                        std::fwrite(rdram + (slot.address & PS2_RAM_MASK), 1, size, out);
                        std::fclose(out);
                        std::cout << " saved=" << name;
                    }
                }
            }
        }
        const std::string file = "vp2s_diag_" + std::to_string(diag.shot++) + ".png";
        std::cout << " screenshot=" << (saveGameFrame(runtime, file) ? file : std::string("-")) << std::endl;
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

        // The game's user-mode timer counters (libkernel, on Timer 2 / INTC 11, handler 0x1420d0):
        // DelayThread and SetTimerAlarm queue counters on the list at +0x18; the handler fires those whose
        // target (+0x20 + +0x10 - +0x18) the extended count ((overflows << 16 | T2_COUNT) << 4*CLKS) passed.
        {
            constexpr uint32_t kTimerManager = 0x0014a1b8u;
            auto rd32 = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x1ffffffu), 4); return v; };
            auto rd64 = [&](uint32_t addr) { uint64_t v; std::memcpy(&v, rdram + (addr & 0x1ffffffu), 8); return v; };
            const PS2Memory::EeTimerState t2 = runtime.memory().eeTimerState(2);
            const EeKernelSnapshot kernel = runtime.eeScheduler().snapshot();
            std::cout << std::hex << "[vp2s-diag]   timer2 count=0x" << t2.count << " mode=0x" << t2.mode << " comp=0x"
                      << t2.compare << " backlog=" << t2.compareBacklog << "/" << t2.overflowBacklog << " intc enabled=0x"
                      << kernel.enabledIntcMask << " pending=0x" << kernel.pendingIntcMask << std::dec
                      << " invocations=" << kernel.pendingInvocations << (kernel.heldUntilEi ? " held-until-EI" : "")
                      << std::hex << " dmac enabled=0x" << kernel.enabledDmacMask << " pending=0x"
                      << kernel.pendingDmacMask << " handlers:";
            for (const EeIrqHandler &h : kernel.dmacHandlers)
                std::cout << " " << h.cause << "->0x" << h.handler << (h.enabled ? "" : "(off)");
            std::cout << std::dec
                      << std::hex << " | counters ovf=0x" << rd64(kTimerManager) << " +8=0x" << rd32(kTimerManager + 8)
                      << " +10=0x" << rd32(kTimerManager + 0x10) << " free=0x" << rd32(kTimerManager + 0x14)
                      << " current=0x" << rd32(kTimerManager + 0x1c) << " list:";
            uint32_t entry = rd32(kTimerManager + 0x18);
            for (int i = 0; i < 12 && entry >= 0x100000u && entry < PS2_RAM_SIZE; ++i)
            {
                std::cout << " [0x" << entry << " id=" << rd32(entry + 8) << " fl=" << rd32(entry + 0xc) << " base=" << rd64(entry + 0x10)
                          << " start=" << rd64(entry + 0x18) << " target=" << rd64(entry + 0x20) << " fn=" << rd32(entry + 0x28)
                          << " arg=" << rd32(entry + 0x30) << "]";
                entry = rd32(entry);
            }
            std::cout << std::dec << std::endl;
        }

        // File request manager (pointer at global+8): request queue pumped by FUN_00102e40 and
        // FUN_001032a0. Entries are 0x2c bytes; entry 0 heads a list linked through +0x14.
        constexpr uint32_t kGlobal = 0x001c8c80u;
        {
            auto rd32 = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x1ffffffu), 4); return v; };
            const uint32_t files = rd32(kGlobal + 8);
            std::cout << "[vp2s-diag]   boot state+110=" << int(rdram[kGlobal + 0x110])
                      << " bank+354=" << int(rdram[kGlobal + 0x354]) << " +371=" << int(rdram[kGlobal + 0x371])
                      << " sub+355=" << int(rdram[kGlobal + 0x355]) << " +358=0x" << std::hex << rd32(kGlobal + 0x358)
                      << " +378=0x" << rd32(kGlobal + 0x378) << " busy+4e4:";
            for (uint32_t i = 0; i < 0x3cu; ++i)
                if (rdram[kGlobal + 0x4e4 + i] != 0)
                    std::cout << ' ' << i << '=' << int(rdram[kGlobal + 0x4e4 + i]);
            std::cout << " file manager 0x" << files << std::dec;
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

        // VP2S_WATCH=<address>[,<address>...] prints those guest words (32-bit) each time.
        if (const char *watch = std::getenv("VP2S_WATCH"))
        {
            std::cout << "[vp2s-diag]   watch:";
            for (const char *cursor = watch; *cursor != '\0';)
            {
                char *end = nullptr;
                const unsigned long address = std::strtoul(cursor, &end, 0) & PS2_RAM_MASK & ~3ul;
                if (end == cursor)
                {
                    break;
                }
                uint32_t value = 0;
                std::memcpy(&value, rdram + address, sizeof(value));
                std::cout << std::hex << " [0x" << address << "]=0x" << value << std::dec;
                cursor = *end == ',' ? end + 1 : end;
            }
            std::cout << std::endl;
        }

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
        // The GS rasterizes on worker threads (VP2's title scene overdraws ~19 screens per
        // frame); nothing here reads VRAM behind the backend's back. VP2S_GS_SYNC=1 draws
        // immediately instead, to compare.
#if defined(VP2S_GS_ASYNC)
        const char *gsSync = std::getenv("VP2S_GS_SYNC");
        GSCpuBackend::SetAsyncRasterization(!gsSync || std::strcmp(gsSync, "0") == 0);
#endif
        // VIF1 (and the VU1 microprograms it starts) on a worker thread with its own clock, the EE
        // ahead of it while it is idle: about 12% more VSync/s on the field scenes, where the EE
        // thread is the bottleneck. PS2X_VIF1_THREAD=0 / PS2X_GS_LAG=0 turn it off.
        // The GS on the GPU (paraLLEl-GS, Vulkan compute; the CPU rasterizer if Vulkan is unavailable): the field's
        // post-processing passes kept the CPU rasterizer at about 24 VSync/s, 36 on the GPU. PS2X_GS_RENDERER=cpu
        // (any other value) keeps the CPU rasterizer.
        // The IOP on its own thread (the EE thread spent about a fifth of its time running the IOP): about 7% more
        // VSync/s on the field, no SPU2 underruns. PS2X_IOP_THREAD=0 runs it on the EE thread.
        const auto setDefault = [](const char *name, const char *value)
        {
            if (std::getenv(name))
                return;
#if defined(_WIN32)
            _putenv_s(name, value);
#else
            setenv(name, value, 0);
#endif
        };
        setDefault("PS2X_VIF1_THREAD", "1");
        setDefault("PS2X_GS_LAG", "1");
        setDefault("PS2X_GS_RENDERER", "parallel");
        setDefault("PS2X_IOP_THREAD", "1");
        // Relaxed GS timing: with the GS lag, SIGNAL/FINISH reach the EE once the modelled drawing time of what came
        // before has passed; VP2 polls FINISH every frame, so the model held the EE back for nothing on a GPU that draws
        // far faster. At 0 the events come as the worker submits the frame: Logun 57% -> 72% of real-time speed.
        // PS2X_GS_PIXEL_CYCLES / PS2X_GS_PRIM_CYCLES set the model back (256 / 32 by default in the runtime).
        setDefault("PS2X_GS_PIXEL_CYCLES", "0");
        setDefault("PS2X_GS_PRIM_CYCLES", "0");
        // Relaxed VIF1 timing: the EE no longer stops at the start of every microprogram the VIF1 worker runs, so the
        // two threads overlap; the worker's SIGNAL/FINISH may reach the EE later in EE time. Logun 72% -> 100% speed.
        // PS2X_VIF1_SLACK=0 brings back the lockstep, a smaller value bounds how far the EE may run ahead (EE cycles).
        setDefault("PS2X_VIF1_SLACK", "1000000000000");
        // The fps and EE-speed counter at the top right (F3 hides it; PS2X_SHOW_FPS=0 starts hidden). VP2 redraws
        // one display buffer, so the frames counted are the distinct pictures shown (drawDiagnostics).
        setDefault("PS2X_SHOW_FPS", "1");
        // Slower SPU2 sound RAM transfers. Entering the field sets up reverb (sound RPC 0x1b clears the effect area in
        // 192 transfers) and asks for its state a frame later (0x1c): 1 while the clear runs, 0 once done, and on 0 the
        // field starts over. At 1 IOP cycle per word the clear was always over by then on the native loop, which hung
        // behind a black fade; at 16 it is still running, as on the console.
        setDefault("PS2X_SPU2_DMA_CYCLES_PER_2BYTES", "16");
        // VP2's battles move the party with the left stick and the game never switches the pad to analog itself.
        setDefault("PS2X_PAD_START_ANALOG", "1");
        PS2Runtime runtime;

        Diagnostics diagnostics;
        const char *diag = std::getenv("VP2S_DIAG");
        diagnostics.enabled = diag && std::strcmp(diag, "0") != 0;
#if defined(PS2X_ENABLE_DEBUG_UI)
        constexpr bool kDebugUi = true;
#else
        constexpr bool kDebugUi = false;
#endif
        // Always: the draw callback counts the distinct frames shown for the fps overlay and handles F9; the
        // diagnostics proper only run with VP2S_DIAG. The init callback must be set before initialize().
        (void)kDebugUi;
        runtime.setDebugUiCallbacks(&initDiagnostics, &drawDiagnostics, &shutdownDiagnostics, &diagnostics);

        if (!runtime.initialize("Valkyrie Profile 2: Silmeria | VP2S_Recomp"))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        std::cout << "[vp2s] VIF1 worker thread " << (runtime.memory().vif1Threaded() ? "on" : "off") << ", GS lag "
                  << (runtime.memory().gsLag() ? "on" : "off") << ", GS renderer " << std::getenv("PS2X_GS_RENDERER")
                  << ", IOP thread " << (runtime.iopThreaded() ? "on" : "off") << std::endl;
        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "Failed to load ELF file: " << elf.string() << std::endl;
            return 1;
        }
        // VP2's music is decoded by an EE thread (VU1 synthesis) that the IOP consumes at its own
        // pace: fast-forwarding the EE to wall-clock time while it waits on the GS starves that
        // thread and the music drops out. Let emulated time slow down instead (the SPU2 output is
        // time-stretched). VP2S_WALLCLOCK_CATCHUP=1 restores the runtime default.
        const char *catchUp = std::getenv("VP2S_WALLCLOCK_CATCHUP");
        runtime.eeScheduler().setWallClockCatchUp(catchUp && std::strcmp(catchUp, "0") != 0);
        startProfiler();
        startHangWatch(g_hostFrames);
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
