// Game-specific runtime setup for Valkyrie Profile 2: Silmeria (SLES-54645).

#include "game_overrides.h"
#include "ps2_runtime.h"

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
    // below the ELF and let a rest-of-RAM heap reach the stack, as on the console.
    constexpr uint32_t kReservedBase = 0x00080000u;
    constexpr uint32_t kReservedTop = 0x00100000u;
    constexpr uint32_t kHeapLimit = 0x01FF0000u;

    void applyVp2s(PS2Runtime &runtime)
    {
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kHeapLimit);

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
