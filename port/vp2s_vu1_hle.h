#pragma once

#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vu1_hle.h"

#include <cstdint>
#include <vector>

// VP2's mesh microprogram (pc 0x0880) done in native code: see docs/vp2_vu1.md. vp2sBuildMeshPacket builds the PATH1
// packet the microprogram would send from VU1 data memory and registers, or returns false for an object it does not
// handle yet (VU1 then runs it); it works on a copy of the object's data and leaves VU memory alone.
// The same object for the native renderer: its triangles in object space (the strips' kicked triangles, ADC ones
// left out), the screen matrix (columns, GS pixels and GS Z / 16 after the divide by w) and the strips' PRIM.
struct Vp2sNativeMesh
{
    std::vector<ps2x::native::Vertex> triangles;
    float matrix[16] = {};
    uint32_t prim = 0;
};

bool vp2sBuildMeshPacket(const uint8_t *code, const uint8_t *data, const VU1State &state, uint32_t top,
                         std::vector<uint8_t> &packet, Vp2sNativeMesh *native = nullptr);

// VP2S_NATIVE_MESH=1: the objects go to ps2x::native::Renderer, drawn in the GS stream at their place (native GS
// renderer), instead of their GS packets. Call at each game frame's end (the frame wait).
bool vp2sNativeMeshEnabled();
void vp2sNativeMeshFrameEnd();

// The ps2x::native::Vu1Hle handler (VP2S_VU1_HLE=1).
bool vp2sVu1Hle(ps2x::native::Vu1HleRun &run);
