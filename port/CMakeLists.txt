cmake_minimum_required(VERSION 3.21)
project(VP2S LANGUAGES C CXX)

# Native port of Valkyrie Profile 2: Silmeria. Links the code recompiled by
# tools/pipeline.py (build/recomp/<input>/) with the PS2Recomp fork runtime.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VP2S_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VP2S_FORK "${VP2S_ROOT}/external/PS2Recomp")
set(VP2S_RECOMP_DIR "${VP2S_ROOT}/build/recomp" CACHE PATH "Recompiled C++ (tools/pipeline.py output)")
set(VP2S_UNITY_BATCH_SIZE 64 CACHE STRING "Generated sources merged per unity-build translation unit")
set(VP2S_GENERATED_OPT "/O2 /Ob1" CACHE STRING "MSVC optimization flags for the generated code (Release)")

# PS2Recomp fork: only the runtime and IOP libraries are used.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)
# Defaults for this port; override on the cmake command line when needed.
set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "Enable FFmpeg-backed MPEG video decoding")
option(VP2S_DEBUG_UI "Show the runtime debugger (ImGui, F1) in vp2s" ON)
set(PS2X_ENABLE_DEBUG_UI ${VP2S_DEBUG_UI} CACHE BOOL "Build the desktop runtime debug UI" FORCE)
# Aggressive logs make every generated function log its entry.
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "Enable very verbose/agressive PS2 runtime logs")
add_subdirectory("${VP2S_FORK}" ps2recomp EXCLUDE_FROM_ALL)

# Generated code: one directory per recompiled input (main ELF + overlay modules).
file(GLOB VP2S_INPUT_DIRS LIST_DIRECTORIES true "${VP2S_RECOMP_DIR}/*")
set(VP2S_GENERATED_DIRS "")
set(VP2S_GENERATED_SOURCES "")
foreach(dir IN LISTS VP2S_INPUT_DIRS)
    if(IS_DIRECTORY "${dir}" AND EXISTS "${dir}/.done")
        list(APPEND VP2S_GENERATED_DIRS "${dir}")
        file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
        list(APPEND VP2S_GENERATED_SOURCES ${sources})
    endif()
endforeach()
if(NOT EXISTS "${VP2S_RECOMP_DIR}/main/register_functions.cpp")
    message(FATAL_ERROR "No recompiled main ELF in ${VP2S_RECOMP_DIR}: run tools/pipeline.py first")
endif()
list(LENGTH VP2S_GENERATED_DIRS input_count)
list(LENGTH VP2S_GENERATED_SOURCES source_count)
message(STATUS "VP2S: ${input_count} recompiled inputs, ${source_count} generated sources")

# The flags below apply to this directory only: the fork keeps its own settings.
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${VP2S_GENERATED_OPT} /DNDEBUG")
endif()

# Port front end (main, game override, diagnostics, optional debugger) in its own object
# library: its includes, defines and ImGui never reach the generated sources' command line,
# so toggling them does not rebuild the whole game.
add_library(vp2s_frontend OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vp2s_overrides.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vp2s_diag_iop.cpp"
)
target_link_libraries(vp2s_frontend PRIVATE ps2_runtime)
if(VP2S_DEBUG_UI)
    target_sources(vp2s_frontend PRIVATE "${VP2S_FORK}/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    # The debug panel reads kernel HLE state through Kernel/... includes.
    target_include_directories(vp2s_frontend PRIVATE "${VP2S_FORK}/ps2xRuntime/src/lib")
    target_link_libraries(vp2s_frontend PRIVATE rlImGui imgui)
    target_compile_definitions(vp2s_frontend PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

add_executable(vp2s ${VP2S_GENERATED_SOURCES})
target_include_directories(vp2s PRIVATE ${VP2S_GENERATED_DIRS})
target_link_libraries(vp2s PRIVATE vp2s_frontend ps2_runtime)

set_target_properties(vp2s PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE "${VP2S_UNITY_BATCH_SIZE}"
)

set(VP2S_RUNTIME_INCLUDE "${VP2S_FORK}/ps2xRuntime/include")
target_precompile_headers(vp2s PRIVATE
    "${VP2S_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_runtime.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_syscalls.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_stubs.h"
)

if(MSVC)
    target_compile_options(vp2s_frontend PRIVATE /FS)
    target_compile_options(vp2s PRIVATE /bigobj /FS)
    # WinAPI vs raylib CloseWindow clash, as for the upstream ps2EntryRunner.
    target_link_options(vp2s PRIVATE /FORCE:MULTIPLE)
endif()
