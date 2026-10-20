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
set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "Build the desktop runtime debug UI")
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

set(VP2S_PORT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vp2s_overrides.cpp"
)
add_executable(vp2s ${VP2S_PORT_SOURCES} ${VP2S_GENERATED_SOURCES})
target_include_directories(vp2s PRIVATE ${VP2S_GENERATED_DIRS})
target_link_libraries(vp2s PRIVATE ps2_runtime)

set_target_properties(vp2s PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE "${VP2S_UNITY_BATCH_SIZE}"
)
set_source_files_properties(${VP2S_PORT_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

set(VP2S_RUNTIME_INCLUDE "${VP2S_FORK}/ps2xRuntime/include")
target_precompile_headers(vp2s PRIVATE
    "${VP2S_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_runtime.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_syscalls.h"
    "${VP2S_RUNTIME_INCLUDE}/ps2_stubs.h"
)

if(MSVC)
    target_compile_options(vp2s PRIVATE /bigobj /FS)
    # WinAPI vs raylib CloseWindow clash, as for the upstream ps2EntryRunner.
    target_link_options(vp2s PRIVATE /FORCE:MULTIPLE)
endif()
