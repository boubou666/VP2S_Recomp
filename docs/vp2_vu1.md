# VP2's VU1 microcode (native mesh path)

Notes for drawing VP2's 3D natively (TMB's mesh API: `setVif1Observer`, `setVu1Gate`, `Renderer`) instead of
running its VU1 microprograms and translating their GS packets. Read the code with `tools/vu_disasm.py` on images
dumped by `vp2s_vu1_replay <trace> dumpcode <dir>`.

## Which microprograms run (field, `build/vu1_traces/vp2_field_heavy.vu1rec`)

`vp2s_vu1_replay <trace> stats`: 18 (code image, start pc) pairs over 4000 runs.

| code image | pc | share of VU1 cycles |
|---|---|---|
| 02a71b3ea5e1c32f | 0x0880 | 70.4% |
| eff679a29e8d77d2 | 0x0880 | 14.7% |
| 02a71b3ea5e1c32f | 0x0770 | 4.2% |
| 5ee6d921ed2641b9 | 0x1690 | 2.8% (4 runs, 40k cycles each) |
| others | 0x06e0, 0x0238, 0x0678, 0x1410 | the rest |

The program at 0x0880 (two code images, likely two builds of the same library) is the mesh renderer.

## Program 0x0880 (code 02a71b3ea5e1c32f)

- `xtop vi14`: the object's block in the double buffer. Header words (qword index from vi14):
  - `0.y` vertex data offset (vi13), `0.w` strip-length list offset (vi12), `0.z` strip count (vi11)
  - `4.xyzw` / `5.xyzw`: up to four set-up routines (5.n = routine index, 4.n = its data offset), run by the
    subroutine at 0x2818 through `jalr` to instruction index 0x499 + n; then more at 0x54a + 2 * `8.y`
  - `13`: a qword copied into every strip's packet (vf30, written as `.yzw`: the GIF tag / register descriptor)
  - `15.x`: added to a counter (vi04), `mem[15].w` (global) selects the path at 0x0c38 (likely lit / different format)
- Globals (VU memory, absolute): `36.x/.y` the two output buffers, `41.z`, `68` (vf13), `81.w` the current output
  buffer, `85..88` a second matrix (vf09, vf27, vf29, vf12) used only for the clip test (`clipw`), `16.z` a sign used
  for back-face culling (cross product of successive edges, `opmula`/`opmsub`).
- Vertex: 4 qwords, stride 4 (`iaddiu vi13, vi13, 4`):
  - `+1` texture coordinates as 12-bit fixed point (`itof12`), multiplied by Q (1/w) → STQ
  - `+2` colour, stored as is (RGBAQ)
  - `+3` position, float xyz; transformed by vf01..vf04 (`mulax/madday/maddaz/maddw`), divided by w, `ftoi4` → XYZ
    12.4 (the matrix includes projection and viewport)
  - `+0` not read on this path
- Strips: the list at vi12 gives each strip's vertex count; a culled or clipped vertex gets the ADC bit (0x8000 in the
  XYZ word's w) so the GS skips its triangle.
- Output: per strip a GIF packet at vi10 (count, tag from header 13, then ST, RGBA, XYZ per vertex), sent with
  `xgkick`; the per-object state packet is kicked from vi07/vi08.

## Set-up routine 1 (0x3b58): rigid skinning

Each vertex's `+3.w` holds the VU address of a 4x4 bone matrix; the routine transforms the normal (`+0.xyz`, 3x3)
and the position (`+3.xyz`, 4x4) in place, for `2.y + 1` vertices. Routines 2-4 (0x3c78, 0x3d80, 0x3ec0) are
probably blends over more bones; the table at instruction 0x54a + 2n (0x2a50...) selects lighting modes, hence the
normal at `+0`.

## Which set-up routines objects use (field trace, 3000 runs)

Header `5.n` is an offset into the jump table at instruction 0x499 (two instructions an entry): 2 → 0x3b58 (rigid
skinning), 4 → 0x3c78, 6 → 0x3d80, 8 → 0x3ec0, 36, 46, 48 → further entries. Most common header 5 values:
(8, 4, 46, 0) 1180 + 149 runs, (8, 0, 0, 0) 1054, none 325, (8, 2, 0, 0) 99, (8, 4, 36, 46) 63, (6, 48, 0, 0) 54.
`8.y` (lighting table) is 0, 1 or 3; `mem[15].w` is 1 for almost every run.

The matrix vf01..vf04 is never changed by these runs: another microprogram loads it (likely the short ones at 0x0238
or 0x06e0 at the start of a frame), so the adapter reads it where that program does.

## Prototype check

`tools/vp2_mesh_proto.py <trace.vu1rec>` decodes every 0x0880 run from VU memory (positions after the set-up
routines, matrix vf01..vf04) and compares the screen positions with the XYZ the microprogram sent: field 2620 of
3000 runs match exactly (to the 1/16 pixel, z too), town 2982 of 3000. The rest is probably the 0x0c38 path.

## Output details (checked on all three traces)

- Set-up routines done natively: 8 (int normal 12-bit / position 12.4 to float), 6 (normals only), 2 (rigid
  skinning, count + 1 vertices), 4 (two-bone blend: per vertex x = matrix A, z = matrix B, y / w weights),
  48 (texture scroll: offset .zw at mem[38 + data.x], added to the 12-bit UV), 46 none. Not yet: 36, lighting mode 3,
  `mem[14].w != 0` paths.
- Lighting (table D, `mem[14].w == 0`): mode 0 colour times mem[13] (RGB to 0 without `mem[14].x`); mode 1 four
  directional lights: directions vf05..vf07 by component, colours mem[5..8], ambient mem[19], alpha mem[13].w,
  colour = clamp(vertex colour times light, 0, mem[0]), truncated. Some builds do not clamp the fourth light at 0
  (`maxx.xyz` + `move.w`): read from the code.
- Builds: the library exists at two layouts (set-up table 0x499 / 0x483, lighting table D 0x562 / 0x54c), found by
  following the BAL at 0x08a8 and its IADDIUs.
- XYZF2 w: ADC for a strip's first two vertices (don't care), any of the last three vertices outside the clip volume
  of mem[85..88], and back faces when the object culls (header `15.x + mem[41].z == 0`: cross product of the last
  three screen positions, sign alternating along the strip, positive = back); fog (`mem[15].w != 0`) =
  clamp(mem[68].z + mem[68].w * Q, 0, 255) in bits 4..11; an ADC vertex's w is the constant 0x8ff0.
- Packet: per strip a tag (x = vertex count, EOP on the last strip; y..w from header 13: PRIM 0x3C tri strip with
  IIP, TME, FGE, NREG 3, regs ST RGBAQ XYZF2), then ST (S, T, Q floats), RGBA (ints), XYZF2 per vertex.

`tools/vp2_mesh_proto.py` reproduces 180704 drawn vertices exactly (position to 1/16 pixel in 99.6% of the 32-bit
words, the rest 1 unit off; UV, colour, fog and ADC exact).

## Next

1. C++ high-level emulation of program 0x0880 in the port: at MSCAL, build the PATH1 packets natively and skip VU1
   (falling back to VU1 for what is not done yet), checked against the real VU1 run in a shadow mode.
2. Routine 36, lighting mode 3, the `mem[14].w != 0` paths.
