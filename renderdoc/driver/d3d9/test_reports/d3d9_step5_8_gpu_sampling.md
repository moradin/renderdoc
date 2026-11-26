# D3D9: pixel debugging's lookups and transcendentals on the GPU (step 5.8)

Step 5.8 of the shader debugging plan (STATUS.md, "Step 5 plan"): `DebugPixel` makes a debugged
pixel's texture lookups and transcendentals on the GPU, so ps_2_0 to ps_3_0 shaders that sample
can be debugged, and their lookups are the GPU's own. ps_1_x (5.9) still says why it can't be
debugged yet.

Tested on the commit that adds it, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124). Files
are in `d3d9_step5_8_gpu_sampling/`.

## How it works

The interpreter (5.5) asks for lookups and transcendentals through its callbacks. `DebugPixel`
answers them on the GPU:

- **When.** The UI asks for a trace's steps after `DebugPixel` returns, and by then the replay
  may be at any event, with other textures and sampler states. So `DebugPixel` runs the quad
  through the interpreter once while the replay is still at the event (after reading the inputs
  back, before putting the device's state back), with `GPULookups` (`d3d9_shaderdebug.cpp`)
  behind a `PSLookupCache` (`d3d9_ps_debug.cpp`). The cache keeps every answer by what was
  asked: the bits of the coordinates, the sampler, the kind of lookup and the declared type; for a
  transcendental, the operation and its operands. Then the debugger steps the same quad from the
  cache alone, which gives the same answers.
- **`texld`, `texldp` and `texldb`.** The plan was a one-pixel `texldd` draw per lookup, with the
  derivatives the interpreter works out. 5.4 had found that this isn't exact with anisotropic
  filtering on a rotated mapping (44 of 4,096 pixels were 1/128 of a level apart). Instead the
  interpreter now gives the coordinate in each pixel of the quad, and `BuildSampleShader` makes a
  ps_3_0 shader that picks each pixel's coordinate by vPos from c0-c3 and looks it up with the
  same instruction, from the same sampler, declared as the shader declares it. It's drawn into a
  2x2 A32B32G32R32F target with the event's textures and sampler states. The GPU then works out
  the derivatives, level, anisotropy, projection and bias as it did for the event. Filtering,
  addressing, sRGB, depth comparisons and unbound samplers aren't emulated at all, and one draw
  answers all four pixels of the quad.
- **`texldd` and `texldl`** don't depend on the other pixels: one draw each, with the coordinate
  and derivatives from constants.
- **Transcendentals** (`rcp`, `rsq`, `exp`, `log`, `pow`, `sincos`, and the `rsq` and `rcp` of
  `nrm` and ps_1_x's divides): a draw each of `BuildTranscendentalShader`'s, on the interpreter's
  operands.
- The draws use the texture viewer's vs_3_0 and quad, and the overlays' render state but for the
  samplers (`D3D9SetDebugRenderState`, split out of `D3D9SetDebugDrawState`). A lookup that fails
  on the GPU fails the debugging, with its reason; the log says how many lookups, transcendentals
  and draws each pixel took.

## What the GPU does

- **An instruction can read only one constant register.** The first sample shader picked the
  coordinates with `cmp r1, r0.x, c1, c0`, and `CreatePixelShader` rejected it. They're copied to
  temps first now; `mov` and `cmp` move values as they are. D3DAssemble also says one input
  register an instruction (`pow r1.z, v1.w, v0.z` in the zoo's assembled shader).
- **Unbound samplers** read (0, 0, 0, 1) here, from both the 2D and the cube sampler, where 5.4 had
  measured (1, 1, 1, 1) for 2D and (0, 0, 0, 0) for cube and volume, with other sampler states.
  What an unbound sampler gives isn't fixed, which is a reason to sample it on the GPU.
- **Transcendentals.** The GPU's `rcp`, `rsq`, `exp`, `log`, `pow` and `sincos` of inputs as they
  are, drawn again on the same operands, are what the shader got, bit for bit (the assembled zoo
  draw). In the HLSL draw 4 of 56 components weren't, at 2 of 7 pixels (`transcheck.py`,
  `transcheck.txt`, on the test's capture):
  - fxc reduces `sincos`'s argument to [-π, π] with `mad r0.x, v2.x, c0.z, c0.w`, `frc` and
    `mad r0.x, r0.x, 2π, -π`. At (140, 211) and (141, 211) `sin` is 50 and 51 ulps from the GPU's
    (3.7e-7), which is the argument an ulp or two off.
  - At (140, 211), `rsq` of `mad r0.x, v1.y, v1.y, 0.01` is 1 ulp off. The GPU's value is the
    correctly rounded `rsq` of the fused `mad`, but the GPU's own `rsq` of that operand, drawn
    again, is 1 ulp higher; so the shader's `rsq` got another operand.
  - Picking the operands per pixel by vPos, so the driver can't treat them as uniform, changed
    nothing (that's kept).
  - So the driver works out these arguments its own way, as 5.7 found it fusing a `mul` and an
    `add`. The interpreter can't know where; the test allows 1e-6 for this draw and logs each
    difference.

## Tests

`D3D9_Shader_Debug_Zoo` has 12 more pixel draws, in two more pairs of A32B32G32R32F targets, each
two triangles with different w at each corner. The pixel vertex shader maps uv by constants (the
identity unless a draw sets one), so lookups take their coordinates straight from inputs:

| Draw | What it looks up or runs |
|---|---|
| `ps_3_0 texture` | the 4x4 point sampled float texture (5.7's, which couldn't be debugged then) |
| `ps_3_0 transcendentals` | HLSL: `1 / x`, `rsqrt`, `exp2`, `log2`, `sincos`, `pow` (fxc's `log`, `mul`, `exp`) |
| `ps_3_0 transcendentals asm` | assembled: `rcp`, `rsq`, `exp`, `log`, `sincos`, `pow` of inputs as they are |
| `ps_2_0 lookups` | `texld`, `texldp`, `texldb` of a 64x64 A8R8G8B8 texture with 7 mips, trilinear, uv x4 |
| `ps_2_x gradients` | `texldd` (ps_2_a) with derivatives from `ddx`/`ddy`, scaled and swapped, anisotropic x8 |
| `ps_3_0 anisotropic` | anisotropic x16 with u stretched 8 times and the mapping turned 30 degrees |
| `ps_3_0 cube` | `texld` and `texldb` of a 16x16 cube texture with 5 mips, trilinear |
| `ps_3_0 volume` | `texld` and `texldl` of a 16x16x16 volume texture with 5 mips |
| `ps_3_0 border` | `D3DTADDRESS_BORDER` with a border colour, uv outside [0, 1], and `texldl` |
| `ps_3_0 sRGB` | `D3DSAMP_SRGBTEXTURE` on the mipmapped texture |
| `ps_3_0 SetLOD` | a managed copy of it with `SetLOD(2)`, set before the frame |
| `ps_3_0 shadow` | an 8x8 D24S8 texture drawn in the frame (depth u), compared against v with `texldp` and z with `texldb`, filtered |
| `ps_3_0 unbound` | a 2D and a cube sampler with nothing bound |

Natively the demo writes what the GPU wrote at 7 pixels of each to
`D3D9_Shader_Debug_Zoo_pixels.txt` (`pixels.txt` here); it leaves out a draw the GPU can't make
(D24S8 textures, sRGB reads), and the test skips it. The test debugs each pixel at replay and
checks, as for 5.7's draws:

- the replay's pixels are the GPU's bit for bit, at all of them;
- the debugger's `oC0` and `oC1` against the GPU's: bit for bit, but for the HLSL transcendentals
  (1e-6 relative);
- for the draws with more than one kind of lookup, the steps that ran a lookup, and only those,
  are flagged `SampleLoadGather`, by instruction;
- that trace stepped with the replay at another event (the fixed function draw) still ends with
  the GPU's outputs, bit for bit.

Results (`tests_x64.txt`):

```
** ps_2_0: 7 pixels debugged, 21 of 28 output components bit for bit the GPU's, the others at most 9.32e-08 off (the driver fuses a mul and an add)
** ps_2_x: 7 pixels debugged, 39 of 56 output components bit for bit the GPU's, the others at most 2.8e-06 off (the driver fuses a mul and an add, then ddx and ddy)
** ps_3_0: 7 pixels debugged, 49 of 56 output components bit for bit the GPU's, the others at most 7.96e-08 off (the driver fuses a mul and an add)
** ps_3_0 texture: 7 pixels debugged, 28 of 28 output components bit for bit the GPU's
** ps_3_0 transcendentals: 7 pixels debugged, 52 of 56 output components bit for bit the GPU's, the others at most 3.73e-07 off (the driver works out their arguments its own way)
** ps_2_0 lookups: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_2_x gradients: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 anisotropic: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 cube: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 volume: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 border: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 sRGB: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 SetLOD: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 shadow: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 unbound: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0 transcendentals asm: 7 pixels debugged, 56 of 56 output components bit for bit the GPU's
** ps_3_0: vPos and vFace are the pixels', the inputs are listed by register too, the outputs are listed, and every step is on an instruction
** ps_2_0 lookups: the steps that sampled are its lookups, ['texld', 'texldp', 'texldb'], and stepped at another event its outputs are the GPU's
** ps_2_x gradients: the steps that sampled are its lookups, ['texldd', 'texldd'], and stepped at another event its outputs are the GPU's
** ps_3_0 cube: the steps that sampled are its lookups, ['texld', 'texldb'], and stepped at another event its outputs are the GPU's
** ps_3_0 volume: the steps that sampled are its lookups, ['texld', 'texldl'], and stepped at another event its outputs are the GPU's
** ps_3_0 shadow: the steps that sampled are its lookups, ['texldp', 'texldb'], and stepped at another event its outputs are the GPU's
** The ps_1_1 pixel shader can't be debugged: 'Debugging ps_1_x pixel shaders isn't supported yet'
** A pixel outside the draw isn't debugged
** The fixed function draw can't be debugged: 'Fixed function vertex processing isn't a shader'
```

The four HLSL transcendentals components that aren't bit for bit, from the log:

```
.. ps_3_0 transcendentals: (140, 211) oC0.y 3.40002608 vs 3.40002584 (1 ulps)
.. ps_3_0 transcendentals: (140, 211) oC1.x 0.111591019 vs 0.111591391 (50 ulps)
.. ps_3_0 transcendentals: (140, 211) oC1.y -0.993754268 vs -0.993754208 (1 ulps)
.. ps_3_0 transcendentals: (141, 211) oC1.x 0.0489500612 vs 0.0489498712 (51 ulps)
```

The log has a line per debugged pixel with its lookups, transcendentals and draws; for example
`ps_2_0 lookups` takes 3 lookups in 3 draws a pixel (one per instruction for the whole quad), the
HLSL transcendentals 28 transcendentals in 28 draws.

Unit tests (`renderdoccmd test unit "[psdebug]"`, new test case "lookups on the GPU", in
`d3d9_ps_debug.cpp`):

- the generated sample shaders for `texld`, `texldp` and `texldb` with 2D, cube and volume
  samplers, run through the interpreter in a 2x2 target: each pixel looks up its own coordinate
  from c0-c3, with the quad's four, the bias, and what it got as its output; `texldd` and `texldl`
  take theirs from the constants, and an undeclared sampler type samples as 2D;
- the transcendental shaders: each pixel's operands, and the result laid out as the interpreter
  keeps it;
- the cache with a stand-in for the GPU: a quad's lookups in one call, `texldd`'s per pixel,
  another sampler or kind another call, transcendentals by their bits (-0 isn't +0), the same
  answers without the GPU, failures for what wasn't asked before or what the GPU failed;
- a ps_2_0 quad with a lookup and an `rcp` stepped again from the cache: the same outputs, with one
  lookup for the quad and four transcendentals.

## UI

qrenderdoc on the zoo capture, driven from inside by a script (`dbg_driver4.py`, through
`InvokeOntoUIThread`), the screenshots taken with PrintWindow of qrenderdoc's window alone. The
script calls `DebugPixel` and opens the shader viewer with the trace, as the texture viewer's Debug
button does:

- `01_texture_viewer_cube.png`: the `ps_3_0 cube` draw's first target with pixel (140, 60) picked,
  (0.48914, 0.52207, 0.35491, 1.00), the GPU's value. The draw's shader samples, and the Pixel
  Context's Debug button is enabled.
- `02_cube_after_texld.png`: that pixel stopped after the cube `texld`, on the `texldb`. `r0`, just
  written, is the lookup's (0.48914, 0.52207, 0.35491, 1.00); the resources list `tex` as the cube
  texture in `s0`.
- `03_shadow_end.png`: the `ps_3_0 shadow` draw's pixel (223, 187) at its last step, with watches:
  `r0`, the `texldp`, is 0.26953 in all four components, the shadow map's comparisons filtered (the
  GPU's value); `r1`, the `texldb`'s comparison, is 0; `tex` is the depth texture.

"Run to Sample" can't be driven from a script. It stops on the steps flagged `SampleLoadGather`,
which the zoo test checks by instruction, and 5.3 checked it on a vertex shader's `texldl`.

## Other checks

On the final code (`verify_summary.txt`):

- `renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors.
- `renderdoccmd test unit "[d3d9]"`, x64 and x86: all 19,697 assertions in 26 test cases pass.
- The automated tests (`run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`): 19 of 19 pass in x64, and
  19 of 19 with the 32-bit demos (`--x86`, `tests_x86_summary.txt`), since the zoo demo changed.
- The vertex interpreter didn't change: the Development `[d3d9bench]` gives 1005-1008 ms
  (1038-1047 ms in 5.7; this is noisy).

## Not covered

- ps_1_x (5.9), whose lookups go through the same draws, with the type of the texture bound.
- Which fragment, and multisampled targets (5.10).
- Lookups in divergent flow control, where each pixel's coordinate is what its neighbours had at
  the same instruction.
- `nrm` against the GPU's own `nrm` (its `rsq` is drawn on its own); `texldp` of cube and volume
  textures, and `texldb` of volume ones.
- How long debugging takes with many lookups: each is a draw and a readback.
- A GPU without D24S8 textures or sRGB reads (the draws are left out), other GPUs.
