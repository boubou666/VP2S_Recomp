// Entry point of the Valkyrie Profile 2: Silmeria native port.
//
// Usage: vp2s.exe <path to SLES_546.45> (the boot ELF extracted by tools/pipeline.py).
// The disc image is found by the game override (vp2s_overrides.cpp, VP2S_ISO).
// VP2S_DIAG=1 prints the resident overlay modules and saves a screenshot every few seconds.

#include "ps2_overlay_modules.h"
#include "ps2_runtime.h"

#include "raylib.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

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
        int frame = 0;
        int shot = 0;
    };

    void drawDiagnostics(PS2Runtime &runtime, void *userData)
    {
        auto &diag = *static_cast<Diagnostics *>(userData);
        if (++diag.frame % kDiagIntervalFrames != 0)
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
        if (diag && std::strcmp(diag, "0") != 0)
        {
            // The init callback must be set before initialize() for the draw callback to run.
            runtime.setDebugUiCallbacks([](PS2Runtime &, void *) {}, &drawDiagnostics,
                                        [](PS2Runtime &, void *) {}, &diagnostics);
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
