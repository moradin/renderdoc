# D3D9 step 8: vs_3_0 outputs packed into one register

Tested on the working tree on top of 94a3a564b, which was committed as "D3D9: show each semantic
of vs_3_0 outputs packed into one register". Machine: Windows 11, RTX 4070 Ti. Files are in
`d3d9_step8_packed_outputs/`.

## What changed

A vs_3_0 shader can declare two or more semantics in one output register, each with its own
components (`dcl_texcoord0 o1.xy`, `dcl_texcoord1 o1.zw`), and a ps_3_0 shader can do the same
with its inputs. fxc doesn't (it gives every semantic a register of its own, checked with
d3dcompiler_47), but hand-written assembly and other compilers can.

The shader reflection kept one usage per register, so the last declaration replaced the others,
and an element's `compCount` went up to its mask's last component. The mesh viewer's VS Out
therefore showed only one of the semantics in a register, with all four of its components
(TEXCOORD1 as o1.xyzw, and no TEXCOORD0).

Now:
- **Reflection** (`d3d9_shader_reflect.cpp`): each declaration is a signature element of its own,
  in order of register and then first component. `regChannelMask` is the declaration's mask and
  `compCount` the number of components in it, as DXBC counts them. This applies to vs_3_0
  outputs, ps_3_0 inputs and vertex shader inputs, so the pipeline state and the shader viewer
  list every declaration too.
- **VS Out** (`d3d9_vs_emulate.cpp`): each element's floats are the register components its mask
  names, in order: `o1.zw` gives o1.z and o1.w, `o4.xz` gives o4.x and o4.z.

## What the GPU does with packed outputs

Native first: before deciding what VS Out should show, a probe (`probe/d3d9_packed_probe.cpp`,
output in `probe/probe.log`, not committed as a demo) drew a point with 8 vertex shaders and 14
pixel shaders declared in different ways, on a hardware vertex processing device and on a software
vertex processing one, and read back what the pixel shader received from a float render target.
The vertex shaders write o1 = (1, 2, 3, 4) and o2 = (5, 6, 7, 8); -1 is what the pixel shader
didn't write.

Hardware vertex processing (`hw.txt`):

```
VS tc0 o1.xy, tc1 o1.zw                          PS tc0 v0.xy                0 0 -1 -1
VS tc0 o1.xy, tc1 o1.zw                          PS tc1 v0.xy                1 2 -1 -1
VS tc0 o1.xy, tc1 o1.zw                          PS tc1 v0                   1 2 3 4
VS tc0 o1.xy, tc1 o1.zw                          PS tc0 v0.xy, tc1 v0.zw     1 2 3 4
VS tc1 o1.zw, tc0 o1.xy (declared in that order) PS tc0 v0                   1 2 3 4
VS tc1 o1.zw, tc0 o1.xy (declared in that order) PS tc1 v0                   0 0 0 0
VS tc0 o1, tc1 o2 (unpacked)                     PS tc0 v0.xy, tc1 v0.zw     5 6 7 8
VS color0 o1.xyz, fog o1.w                       PS color0 v0.xyz            1 1 1 -1
VS color0 o1.xyz, fog o1.w                       PS fog v0.x                 1 -1 -1 -1
```

Each output register reaches the pixel shader under the last semantic declared in it, whole and in
place; the others in the register don't arrive (0, or 1 for a colour). Input registers work the
same way: the unpacked vertex shader's TEXCOORD1 register filled all of a `v0` whose last
declaration was TEXCOORD1.

Software vertex processing (`sw.txt`), where the runtime runs the vertex shader:

```
VS tc0 o1.xy, tc1 o1.zw                          PS tc0 v0                   1 2 0 1
VS tc0 o1.xy, tc1 o1.zw                          PS tc1 v0                   1 2 3 4
VS tc0 o1.xy, tc1 o1.zw                          PS tc0 v0.xy, tc1 v0.zw     1 2 3 4
VS tc0 o1, tc1 o2 (unpacked)                     PS tc0 v0.xy, tc1 v0.zw     1 2 7 8
VS tc0 o1, tc1 o2 (unpacked)                     PS tc1 v0.xy, tc0 v0.zw     0 0 0 0
VS color0 o1.xyz, fog o1.w                       PS color0 v0.xyz            1 2 3 -1
```

Each semantic arrives on its own, as the register's components from x up to the last one its mask
has, in place, then (0, 0, 0, 1). A pixel shader that declared TEXCOORD1 in `v0.xy` and TEXCOORD0
in `v0.zw` read 0 for both, whatever the vertex shader did.

The two paths link mismatched shaders differently, but both give a pixel shader declared like the
vertex shader every semantic in the components it was declared in (1 2 3 4 above, on both). That's
how such shaders are meant to be paired, so that's what VS Out shows and what the test compares
with. Nothing in the replay links a vertex shader's outputs to a pixel shader's inputs, so the
mismatched cases aren't emulated; shader debugging (step 5) would need them.

## The tests

`D3D9_Mesh_Output` has a new draw, "vs_3_0 packed outputs": an assembled vs_3_0 shader over the
2x2 grid that puts 11 semantics in 5 registers:

```
dcl_texcoord0 o1.xy     dcl_texcoord1 o1.zw
dcl_color0 o2.xyz       dcl_fog o2.w
dcl_texcoord2 o3.x      dcl_texcoord3 o3.yzw
dcl_texcoord4 o4.xz     dcl_texcoord5 o4.yw
dcl_texcoord6 o5.x      dcl_texcoord7 o5.y      dcl_color1 o5.zw
```

Each register is r0 * a + b, with r0 = (u, v, x, y) of the vertex and literals a and b of the
register's own, so every component differs. It's drawn with a ps_3_0 shader that packs its inputs too (TEXCOORD0 and TEXCOORD1 in
`v0`, TEXCOORD2 and TEXCOORD3 in `v1`).

- **The reference:** C++ works out each vertex's semantics as the components their masks name.
  The demo also reads them back from the GPU (`hw` rows, marked `hwfloat`): each vertex drawn as a
  point into a 1x1 A32B32G32R32F target, once per register, with a pixel shader that declares `v0`
  as the vertex shader declares the register (with `_centroid` where a colour shares it, which
  ps_3_0 requires) and writes `v0`. Natively the C++ reference and the GPU agree within 3.8e-6
  for every row (the GPU's rounding of the multiply-add).
- **`d3d9_mesh_check.py`** compares VS Out with both, as floats for `hwfloat` draws, and requires
  the same number of components.
- **`D3D9_Mesh_Output.py`** also checks the draw's vertex shader output signature (12 elements:
  name, register, mask and components used) and pixel shader input signature (5), and that every
  element's `compCount` is the number of components in its mask.
- **Unit tests:** a reflection test of a ps_3_0 with two semantics in `v0` (declared in reverse
  order) and three in `v1`, and an emulation test of a vs_3_0 with packed outputs, including a
  non-contiguous mask, through `RunPostVS`.

## Results

### Automated tests

All 17 passed, x64 and x86 (`run_summary.txt`, `run_x64.txt`, `run_x86.txt`):

```
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:17.666930 ##
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:17.801047 ##
```

The new draw, in both:

```
.. PASS vs_3_0 packed outputs (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True, 24 rows checked against the GPU)
** The packed draw's vertex shader output signature has the 12 declared elements
** The packed draw's pixel shader input signature has the 5 declared elements
```

The change is replay only; the x86 run captured the 32-bit demos, which include the new draw and
its GPU readback.

### Other checks

- **Unit tests:** `[d3d9]` passed (6104 assertions in 14 test cases, `unit.txt`). The first run
  had 2 failures, both wrong in the test: COLOR1's name has its index, and the disassembly writes
  `dcl_texcoord` for index 0.
- **Builds:** `renderdoc.sln` and `demos.sln` built with 0 warnings and 0 errors, for both x64 and
  x86.
- **Natively:** the packed draw's C++ reference and GPU readback agree within 3.8e-6
  (`native_reference_packed.txt`). The reference written under RenderDoc is byte-identical to the
  native one (the test checks this).

### The old driver

With the driver from 94a3a564b and the new demo and test (`run_old_driver.txt`), the new draw
fails, and so the test does (264 problems, 11 per row):

```
FAIL vs_3_0 packed outputs (...): 264 problems
    instance 0 row 0: no COLOR.0 output (have ['COLOR.1', 'FOG.0', 'POSITION.0', 'TEXCOORD.1', 'TEXCOORD.3', 'TEXCOORD.5'])
    instance 0 row 0: (GPU) COLOR.1 ours has 4 components, the reference 2
    instance 0 row 0: (GPU) FOG.0 ours has 4 components, the reference 1
    instance 0 row 0: no TEXCOORD.0 output (have [...])
    instance 0 row 0: (GPU) TEXCOORD.1 ours has 4 components, the reference 2
```

Only the last semantic of each register was there, with all four of its components. The other 16
draws passed as before.

## UI

I captured the demo and opened it in qrenderdoc at the packed draw (event 2031):
- **`ui_1_vs_out.png`:** the Mesh Viewer's VS Output, scrolled across in four parts. Every
  semantic is a column of its own, with as many components as its mask: TEXCOORD0 (0.10, 0.20)
  and TEXCOORD1 (-2.70, -3.60) for row 0, COLOR0 3 components, FOG 1, TEXCOORD2 1, TEXCOORD3 3,
  TEXCOORD4 and TEXCOORD5 2, TEXCOORD6 and TEXCOORD7 1, COLOR1 2. The values are the reference's.
- **`ui_2_vs_signatures.png`:** the vertex shader in the shader viewer: the disassembly keeps
  every `dcl`, and the output signature has a row per declaration with its register and mask
  (TEXCOORD0 register 1 float2 RG, TEXCOORD1 register 1 float2 BA, COLOR0 register 2 float3 RGB,
  FOG register 2 A, ...).

The debug message count was 0.

`fxc_check.txt` is the d3dcompiler_47 check that fxc doesn't pack vs_3_0 outputs: two float2
outputs and three scalars each got a register of their own.

## Not covered

- **Other GPUs and Windows versions:** how mismatched shaders link was measured on an RTX 4070 Ti
  on Windows 11 only, and it's a driver matter on the hardware path.
- **Mismatched vertex and pixel shaders** aren't emulated (see above). Nothing shows what a pixel
  shader receives.
- **Real games:** no game with packed outputs was tried.
- **Vertex shader inputs** declared twice in one register: not valid D3D9 as far as is known, not
  tried. The reflection would list each declaration.
