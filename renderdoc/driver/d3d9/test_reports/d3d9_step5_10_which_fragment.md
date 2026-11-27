# D3D9: pixel debugging debugs the fragment the GPU wrote (step 5.10)

Step 5.10 of the shader debugging plan (STATUS.md, "Step 5 plan"): `DebugPixel` reads a pixel's
inputs from the fragment the GPU wrote, where overlapping triangles in one draw, depth and stencil
writes, `texkill` and the alpha test decide which that is, and on multisampled targets. Step 5.9
(ps_1_x) was put off, so ps_1_x shaders still say why they can't be debugged.

Tested on the commit that adds it, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124). Files
are in `d3d9_step5_10_which_fragment/`.

## How it works

Since 5.7 `DebugPixel` draws the event again with pixel shaders that write the inputs to float
render targets of our own. Until now those were replacements, which can't discard and drew with
depth and stencil writes off, so they read the last fragment drawn that passes the depth and
stencil tests as they were before the draw. 5.4 had measured that the original shader with its
inputs written to extra targets reads the right one, 27 of 27.

- **The original shader with the reading appended** (`BuildInputReadback`, now with a
  `ReadbackShader`, in `d3d9_ps_debug.cpp`). For ps_2_0 and later each pass is the original's
  bytecode with:
  - the reading inserted where main ends: before its first `ret` ahead of the subroutines'
    labels, or before the end token. The original's temps are free by then;
  - its values written to the render targets the original doesn't write: oC1-oC3 for a shader
    writing oC0, so three a pass;
  - the two constants the reading needs defined in registers the original doesn't read or define
    (the highest two). A relatively addressed read could still reach them, which isn't checked;
  - ps_2_0 moved up to ps_2_x for `dsx` and `dsy`, as the replacement was, and vPos declared for
    ps_3_0's reading of the quad if the original doesn't declare it.

  The original's `texkill` and `oDepth` and the draw's alpha test then decide, as they did. oC0
  goes to a target of the draw's own format, since the alpha test compares at its precision (5.4).
- **A copy of the depth stencil.** 5.4's probe drew with the draw's own depth writes, so one
  draw's fragments tested against each other. A redraw with depth writes off reads the last
  fragment drawn that passes the test against the depth as it was before the draw, even when an
  earlier fragment of the same draw was nearer. So when the draw writes depth or stencil, each
  pass now draws to a copy of the draw's depth stencil as it was (StretchRect between depth
  stencils, outside the scene, which the replay is in), with the draw's own depth and stencil
  state. When it can't be copied (an INTZ texture, a lockable format) the passes leave it alone as
  before, and the log says so.
- **When the original can't** (ps_1_x, a shader writing every render target, a multisampled draw,
  a shader the runtime rejects with the reading appended), a replacement reads the inputs, now
  with the depth copy too. When the original's passes write nothing at the pixel, because
  `texkill` or the alpha test rejected every fragment there, the replacement reads the fragment
  that would have been written without them, which is what there is to debug. The log says which
  fragment each debugged pixel is: "the fragment the draw wrote", or why not.
- **Multisampled targets** were refused until now. The probe below found how to read them:
  - Our target has the draw's sample count and quality, and so does the depth stencil copy.
  - The passes draw with `MULTISAMPLEANTIALIAS` FALSE. The GPU then shades the fragment covering
    the pixel centre at the centre, as it shaded it for the draw (a fully covered pixel is shaded
    once, at its centre), and writes it to every sample.
  - A one-pixel resolve reads it back exactly, but for -0 and denormals, which come back +0.
  - D3D9 has no multiple render targets with multisampling, so a multisampled draw is read by a
    replacement, one value a pass. `texkill` and the alpha test don't choose its fragment.
- Also fixed on the way: `RestoreReplayDrawState` only sets the render targets the replay has, so
  readback targets left in slots 1-3 stayed bound, and the 2x2 lookup draws (5.8) could have run
  with them. The readback now unbinds them, and the lookup draws unbind slots 1-3 themselves.

## Native probe: multisampled targets (GPU)

`d3d9_msaa_probe.cpp` (a demo built into `demos_x64.exe` for the run and taken out again) drew
into 16x16 multisampled A32B32G32R32F targets natively. Its output is `msaa.txt`. All numbers here
are the GPU's.

- **Formats.** A32B32G32R32F, A16B16G16R16F, A8R8G8B8 and D24S8 all have 2x, 4x and 8x (one
  quality level each) and NONMASKABLE (4 levels).
- **Resolves** of a fully covered pixel give its value exactly, 8 of 10 values at each sample
  count: -0 and the denormal (1e-40) come back +0. Writing sample 0 alone (`MULTISAMPLEMASK` 1)
  into a target cleared to 0 resolves to the value over the sample count, exactly the same 8.
- **Where the pixel is shaded.** A shader writing 4 dx² and 4 dy² of its position's offset from the
  pixel centre resolves to exactly 0 with every sample written: a fully covered pixel is shaded
  once, at its centre, in 2x, 4x and 8x, on A32B32G32R32F and A8R8G8B8.
- **`MULTISAMPLEMASK` moves the shading.** With one sample masked in, the same shader gives that
  sample's offset: (-0.125, -0.375), (0.375, -0.125), (-0.375, 0.125) and (0.125, 0.375) for 4x's
  samples 0-3, the standard positions. 2x and 8x's first four do the same; 8x's samples 4-7 are
  shaded at the centre. So writing sample 0 alone can't read the inputs the GPU used for the
  pixel.
- **`MULTISAMPLEANTIALIAS` FALSE** shades at the centre (exactly 0 again), covers by the centre
  (an edge swept across the pixel covers it while it's at or left of the centre), writes every
  sample (the same 8 of 10 values resolve exactly), and still tests each sample's depth:
  against a depth stencil whose left half is nearer, a full draw passes 128 pixels times the
  sample count.
- **Depth stencil copies.** StretchRect copies a multisampled D24S8 depth stencil to another
  (S_OK), and a draw tested against the copy passes exactly where the original's depth lets it.
- **Not measured: centroid in a partly covered pixel.** The probe read partly covered pixels with
  one sample masked in, which moves the shading to that sample, plain or centroid. Covered
  samples of a triangle over the pixel's corner got their own positions in 4x (plain and
  centroid alike), and in 8x plain inputs at two of five covered samples were at the centre
  while centroid ones were at the samples. That says the mask changes the shading, not where
  centroid inputs are without it.

Found in the zoo, not the probe: **D3D9 has no multiple render targets with multisampling.** The
first multisampled zoo draw had two 4x targets and wrote nothing natively, with nothing in the
log; with one target it draws.

## The zoo's draws (GPU)

`D3D9_Shader_Debug_Zoo` has 8 more pixel draws, in two more pairs of targets: one whose first
target is A8R8G8B8, one 4x multisampled with its own 4x D24S8 depth stencil (one target). Each
which-fragment draw is three (or two) overlapping layers in one draw, each layer the cell's two
triangles with perspective, with its id in uv1.z, which the shader writes to oC0.x and oC1.z. The
`texkill` boundary (uv1.w < 0) is at u = 0.5, about two thirds across the cell, so 5 of the 7
probes are on its left and 2 on its right.

| Draw | Shader | State | Layers, in order | Written natively |
|---|---|---|---|---|
| ps_3_0 depth | ps_3_0 | depth LESS, writes | 0.3 killed on the left, 0.5, 0.6 | the middle on the left, the nearest on the right |
| ps_2_0 texkill | ps_2_0 | no depth test | three, the last killed on the left | the second on the left, the last on the right |
| ps_2_x alpha test | ps_2_a, `ddx`/`ddy` | GREATEREQUAL 128, float target | alpha 1, alpha 0.25 to 0.75 | the first on the left, the second on the right |
| ps_3_0 stencil | ps_3_0 | EQUAL 0, INCR, depth ALWAYS without writes | the first killed on the left | the second on the left, the first on the right |
| ps_3_0 alpha test 8-bit | ps_3_0 | GREATEREQUAL 128, A8R8G8B8 target | alpha 1, 0.5003, 0.5001 | the second: 127.58 of 255 passes on 8 bits (5.4), not at fp32; 0.5001 fails both |
| ps_2_x point sprites | ps_2_a, `ddx`/`ddy` | 48 pixel sprites | two sprites over the probes | |
| ps_3_0 centroid | ps_3_0, `TEXCOORD0_centroid`, `TEXCOORD1_centroid` | | | |
| ps_3_0 multisampled | ps_3_0, one target | 4x, depth LESS, writes | 0.3, 0.5, 0.6 | the nearest |

The demo writes what the GPU wrote at the 7 probes of each, natively (the A8R8G8B8 target as
c / 255, the multisampled one resolved: the probes are inside the layers, where every sample has
the same value). In the native reference, the layer each probe got is the one in the last column
at all 35 probes of the five which-fragment draws (checked by its id), and the multisampled
draw's is the nearest at all 7.

Point sprites replace every texture coordinate set with the sprite's coordinate, TEXCOORD1 as
well as TEXCOORD0 (oC1.xy is the sprite's coordinate, not 2u - 0.5).

## Results (RenderDoc against the GPU)

`D3D9_Shader_Debug_Zoo` checks each probe: the replay's pixels against the native ones, bit for
bit, and the debugger's outputs against them, bit for bit, for every new draw. The A8R8G8B8 draw's
oC0 is compared as that target stores it (rounded to 8 bits).

```
ps_3_0 depth: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_2_0 texkill: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_2_x alpha test: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_3_0 stencil: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_3_0 alpha test 8-bit: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_2_x point sprites: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_3_0 centroid: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
ps_3_0 multisampled: 7 pixels debugged, 28 of 28 output components bit for bit the GPU's
```

All 420 output components of the 8 draws are the GPU's bit for bit. The 17 earlier draws give
the same numbers as before (`tests_x64.txt`), through the original shader now: the log says "the
fragment the draw wrote" for all 168 of their pixels and the new ones, and gives the replacement's
reason for the multisampled draw's 7.

### Without the new parts (RenderDoc, a scratch build)

To check the draws tell the ways apart, a scratch build (not committed) could switch off the
original shader, the depth copy, or oC0's format. `fragments.py` debugged each probe of a capture
of the zoo with each and compared the debugger's oC0 and oC1 with the GPU's (`fragments.txt`):

| Draw | 5.10 | 5.8's way: a replacement, no depth or stencil writes | A replacement with the depth copy | The original without the depth copy | The original, oC0 to A32B32G32R32F |
|---|---|---|---|---|---|
| ps_3_0 depth | 7 of 7 | 0 | 2 | 0 | 7 |
| ps_2_0 texkill | 7 of 7 | 2 | 2 | 7 | 7 |
| ps_2_x alpha test | 7 of 7 | 2 | 2 | 7 | 7 |
| ps_3_0 stencil | 7 of 7 | 0 | 2 | 0 | 7 |
| ps_3_0 alpha test 8-bit | 7 of 7 | 0 | 0 | 7 | 0 |
| ps_2_x point sprites | 7 of 7 | 7 | 7 | 7 | 7 |
| ps_3_0 centroid | 7 of 7 | 7 | 7 | 7 | 7 |

Each part is needed by at least one draw. 5.8's way gets 4 of these 35 pixels. With oC0 to a float
target, the 8-bit draw's alpha test compares at fp32 and rejects the layer the GPU wrote, as 5.4
predicted.

## Unit tests

`renderdoccmd test unit "[psdebug]"` runs the original-with-reading shaders through the
interpreter: a ps_2_0 shader that kills, defines c31 and writes oC0 gets three passes, in ps_2_x,
writing oC1-oC3, with c30 and c29 defined and its own instructions in order; its oC0 and discards
are as they were and the values read are the replacement's, in every pixel of the quad. A ps_3_0
shader whose main calls a subroutine gets the reading before main's `ret`, vPos declared once,
vFace read, and its own oC0 and oC1 unchanged. A shader that declares vPos isn't given a second
declaration; one with no inputs is the original as it is; ps_1_x and a shader writing every target
are refused.

## Builds and tests

`verify_summary.txt`:

```
renderdoc.sln x64 exit 0 warnings 0 errors 0
demos.sln x64 exit 0 warnings 0 errors 0
renderdoc.sln x86 exit 0 warnings 0 errors 0
demos.sln x86 exit 0 warnings 0 errors 0
unit x64 exit 0 All tests passed (20184 assertions in 26 test cases)
unit x86 exit 0 All tests passed (20184 assertions in 26 test cases)
  RunPostVS: 200000 vertices of a 16 instruction shader in 978.9 ms (best of 5)
tests x64 exit 0
## Tests complete summary: 19 passed out of 19 run from 210 total in 0:01:37.098081 ##
tests x86 exit 0
## Tests complete summary: 19 passed out of 19 run from 210 total in 0:01:37.767340 ##
```

- Both solutions build with 0 warnings for x64 and x86, formatted with clang-format 15.
- The x86 run captures the zoo with the 32-bit demo, and gives the same lines for every pixel
  draw as x64 (`zoo_lines.txt`). The demo changed, so both ran.
- The Development `RunPostVS` bench is 979-980 ms (1005-1008 in 5.8): pixel debugging doesn't
  touch it.

## UI

qrenderdoc, driven from inside by a script on its UI thread (no mouse or keyboard input), on a
capture of the zoo. Screenshots are of qrenderdoc's window alone (PrintWindow).

- `01_texture_viewer_depth.png`: the "ps_3_0 depth" draw in the texture viewer, pixel (140, 60)
  picked: (0.00, 0.27458, 0.37966, 1.00). x is 0, layer 2's id: the nearest layer is killed
  there. The Debug button is enabled.
- `02_debug_depth_texkill.png`: that pixel debugged, at the end. TEXCOORD1 is (0.04915, 0.25932,
  0.00, 1.50): z 0 is layer 2, and w 1.5 passes `texkill`. oC0 is (0.00, 0.27458, 0.37966,
  1.00), the pixel the GPU wrote. 5.8 debugged layer 3 here (0.25), the last drawn.
- `03_debug_multisampled.png`: the multisampled draw's pixel (40, 60) debugged, at the end.
  TEXCOORD1 is -0.25, the nearest layer, and oC0 (-0.25, 0.27458, 0.37966, 0.5061) is what the
  GPU wrote.

## Not covered

- ps_1_x (5.9, put off): it would be read by a replacement, so `texkill` doesn't choose its
  fragment.
- `texkill` and the alpha test on multisampled draws: a replacement reads them (D3D9 has no
  multiple render targets with multisampling).
- A fragment covering some of a multisampled pixel's samples but not its centre isn't found, and
  a pixel at a primitive's edge has several fragments, of which the one covering the centre is
  debugged. The zoo's multisampled probes are all inside the layers.
- Centroid inputs in a partly covered multisampled pixel: the readback reads them at the centre,
  which isn't where the GPU reads them. Not measured either (see the probe).
- A shader writing all four targets (a replacement reads it), a depth stencil that can't be copied
  (INTZ, lockable formats), and relatively addressed constants reaching the two the reading
  defines: not tested.
- Other GPUs. A GPU without `D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS` gets the replacement unless the
  draw's target is A32B32G32R32F (not tested).
