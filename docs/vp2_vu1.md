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

## Prototype check

`tools/vp2_mesh_proto.py <trace.vu1rec>` decodes every 0x0880 run from VU memory (positions after the set-up
routines, matrix vf01..vf04) and compares the screen positions with the XYZ the microprogram sent: field 2620 of
3000 runs match exactly (to the 1/16 pixel, z too), town 2982 of 3000. The rest is probably the 0x0c38 path.

## Next

1. The set-up routines (0x2818 → 0x499 + n, 0x54a + ...): matrices, skinning or morphing, lighting.
2. The path at 0x0c38 (`mem[15].w != 0`).
3. A VIF1 stream of one object (UNPACK targets, MSCAL 0x0880, the state packet) to map header fields to EE data.
