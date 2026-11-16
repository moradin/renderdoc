# D3D9: native probes for pixel shader debugging

Step 5.4 of the shader debugging plan (STATUS.md, "Step 5 plan"): a probe and a report, with no
driver change. It measures what the GPU does in each area where the pixel shader debugger (5.5 to
5.10) would otherwise have to trust the documentation, and records the decisions for those chunks.
Run on top of 94a6ca781. Machine: Windows 11, NVIDIA GeForce RTX 4070 Ti, driver 32.0.16.2124.
Files are in `d3d9_step5_4_native_probes/`.

Every number here comes from the GPU, natively, without RenderDoc. The references called "C
library" are MSVC's `float` functions (`1.0f / x`, `sqrtf`, `exp2f`, `log2f`, `powf`, `sinf`,
`cosf`, `fmaf`), computed in the probe from the exact values the shader received.

## The probe

`probe/d3d9_ps_probe.cpp` is a temporary demo (`D3D9_PS_Probe`), built into `demos_x64.exe` for
the run and then taken out of the project, as the earlier probes were. It draws into small
A32B32G32R32F render targets (A8R8G8B8 and A16B16G16R16F where the target format matters), reads
them back with `GetRenderTargetData`, and writes one file per section to `out/`. Shaders are
assembled with `D3DAssemble`. ps_3_0 shaders are drawn with a pass-through vs_3_0, the others with
a pass-through vs_2_0 (`oD#`, `oT#`).

A cleared float target holds 0x3ce0e0e2 for 7/255 (the CPU's float is 0x3ce0e0e1), so "not
written" is checked against the value read back from a cleared target, not a constant.

The probe was run twice from scratch with the final source. The outputs are identical, apart from
one corrected label in `c_ps1x.txt`. `probe/probe.log` is the final run's log, `out/` its output,
and `probe/analyse_g.py` the script that summarises the arithmetic files into `g_analysis.txt`.
The per-instruction arithmetic files (`g_<op>.txt`, 9 MB, one line per input) aren't kept; running
the probe writes them again.

## Results

### Pixel centre and vPos (`a_centre.txt`)

- **Attributes are sampled at whole screen coordinates.** On a target-sized quad whose texcoord is
  the vertex's screen position, pixel (x, y) reads exactly (x, y), in ps_2_0 and ps_3_0. That is
  D3D9's rule, and the opposite of D3D10's +0.5.
- **Coverage** follows the same centres and the top-left rule:
  - a rectangle from 2.0 to 5.0 covers pixels 2, 3 and 4;
  - from 2.25, 2.49, 2.5, 2.51 or 2.75 (to 3 pixels further), it covers 3, 4 and 5.
- **vPos.xy** is the pixel's whole coordinates (0, 1, 2...), equal to the interpolated position.
- **vPos is relative to the render target, not the viewport.** With a viewport at X 4, Y 2, the
  top left pixel of the viewport reads vPos (4, 2), while its viewport-relative texcoord is (0, 0).

### vFace (`b_vface.txt`)

- **vFace can only be a condition.** The assembler rejects `mov r0, vFace` and `add r0, vFace, ...`
  ("vFace register can only be used as one of the sources to if_(cond), break_(cond), setp_(cond),
  or the condition in a cmp"). So its value was found with `if_gt`, `if_eq` and `if_lt` against
  thresholds from 0 to 1e6.
- **Its value is exactly +1 or -1.**
  - Triangles that are clockwise on screen read +1, counter-clockwise ones -1.
  - That holds for big and small triangles, with cull mode none, CW and CCW (culling only removes
    one kind), and with wireframe and point fill.
  - Lines and points read +1.
  - `cmp` treats it as +1/-1, as do `setp_gt` and `if_ge` against 0.

### ps_1_x (`c_ps1x.txt`)

Caps: PixelShader1xMaxValue 65504, MaxTextureRepeat 8192, PixelShaderVersion 3.0.

- **Constants are clamped to [-1, 1] when read**, in ps_1_1 to ps_1_4: c0 = (2, -2, 0.25, 0.75)
  reads as (1, -1, 0.25, 0.75), and 1.5 as 1. The constants were set with
  `SetPixelShaderConstantF`.
- **The output r0 is clamped to [0, 1]**, even into a float target: `add r0, c0, c0` giving
  (1.5, -1.5, 0.6, 1.2) writes (1, 0, 0.6, 1).
- **Temporaries are fp32 with no range limit.**
  - Values of 921, 171,993 (ps_1_1-1_3, `_x4`) and 1,375,949 (ps_1_4, `_x8`) scaled back down came
    out exactly, so nothing was clamped at ±1, ±8 or the reported 65504.
  - `mul` of 1/3 by 1/3 gives the fp32 product (0.111111119), so there's no fixed point either.
- **Modifiers don't clamp**, except `_sat`:
  - source modifiers apply after the constant clamp: `_bx2` of -0.3 gives -1.6, of 1.5 gives 1
    (clamped to 1 first);
  - `_bias` gives x - 0.5, and `1-x` of -0.3 gives 1.3;
  - `_x2` gives 1.8 and 2, `_d2` and `_d8` divide, and ps_1_4's `_x2` source modifier works the
    same.
  - `_sat` clamps to [0, 1].
- **Texture coordinates and samples:**
  - `texcoord` (ps_1_1 to 1_3) clamps all four components to [0, 1]: (-0.5, 0.3, 1.7, 3) reads as
    (0, 0.3, 1, 1), and values inside the range are full fp32.
  - ps_1_4's `texcrd` doesn't clamp: (2.5, -2, 0.999) reads as is.
  - `tex` and `texld` don't clamp what they sample: a float texel (2, -1, 0.3, 3.5) reads as is in
    every version.
- **Colour inputs:** `v0` arrives clamped to [0, 1] at full fp32 precision (0.123456791 reads as
  is), as in ps_2_0 (see below).
- **`cnd`** takes src1 when the condition is above 0.5, strictly, at fp32: 0.5 takes src2, and the
  next float above 0.5 (0x3f000001) takes src1.
  - It works the same in ps_1_1 to 1_3 (r0.a) and ps_1_4 (per component), for every k with
    0.5 ± 2^-k.
  - It works the same when the condition comes from an interpolated colour or texcoord.
- **`cmp`** (ps_1_2 and later) treats -0 as ≥ 0, and -1e-30 as < 0.

### Colour inputs (`d_colour.txt`)

- **Precision:** colours are full fp32 in every model. 0.123456789, 1/3 and 1e-5 come through
  exactly, so there's no 8-bit quantisation.
- **Clamping:** a vs_2_0's `oD0` arrives clamped to [0, 1] in ps_1_x and ps_2_0. A vs_3_0's COLOR
  output arrives unclamped in ps_3_0 (-0.5 and 1.7 read as is).
- **Interpolation:** colours are interpolated bit-identically to texture coordinates.
  - Across 256 pixels with w 1, and with perspective (w from 1 to 4), every model's colour equals
    its texcoord exactly.
  - ps_2_0's v0 equals ps_3_0's color0 exactly too.
- **Flat shading** (`D3DSHADE_FLAT`) gives colours the first vertex's value, including ps_3_0's
  `dcl_color` inputs, and leaves texcoords interpolated.

### Derivatives (`e_derivatives.txt`)

- **Per 2x2 quad.** In ps_3_0 and in ps_2_x (with a vs_2_0), `dsx` is the quad's top row (top
  right minus top left) and `dsy` its left column (bottom left minus top left), for all four
  pixels.
  - With f = x·y on a full quad, `dsx(f)` is 2 at pixels (4..5, 2..3), the top row's y, also on
    the bottom row where y is 3.
  - `dsy(f)` is 4, the left column's x, also in the right column.
- **Helper pixels run with extrapolated inputs.** With a small triangle covering only the bottom
  right pixel of a quad, that pixel's `dsx` and `dsy` come from the uncovered top row and left
  column, as if the triangle's plane carried on.
- **Scissoring doesn't change derivatives.** A full quad scissored to one pixel gives that pixel
  the same values as the unscissored draw.
- ps_2_x's `dsx`/`dsy` give the same values as ps_3_0's. This GPU's PS20Caps is 0x1f: gradient
  instructions, predication, arbitrary swizzles, no dependent read or texture instruction limits,
  32 temps and 512 instruction slots.

### texldd against texld (`f_texldd.txt`, `f_texldd_diffs.txt`)

Setup:
- **Texture:** 64x64 A32B32G32R32F with 7 levels, texel = (level, checker, u, v). With linear mip
  filtering the red channel reads the level blend.
- **Samplers:** trilinear; point with point mip; bilinear with point mip; anisotropic 4 and 16
  with a linear mip; trilinear with `MIPMAPLODBIAS` 0.5; trilinear with `MAXMIPLEVEL` 2.
- **Mappings:** 1:1, scale 3.7, scale 8 in u only, rotated 30° at 2.5, perspective (w 1 to 5),
  scale 40, mirrored, and a sub-texel offset.

Pass 1 does `texld` and, in the same shader, `texldd` with `dsx`/`dsy` of the coordinate. Pass 2
is a one-pixel draw per pixel doing `texldd` with pass 1's coordinate and derivatives as constants:
the way 5.8 would sample.

- **54 of the 56 combinations are bit-exact** for all 4,096 pixels, in both passes. That includes
  the bias and `MAXMIPLEVEL`, which apply to `texldd` as to `texld`.
- **Anisotropic 4 and 16 with the rotated mapping** differ in 44 of 4,096 pixels.
  - The pixels are 11 whole 2x2 quads, where `texld` used a level 1/128 lower (1.0234375 instead of
    1.03125) and the texel weights moved with it.
  - Both `texldd` passes agree with each other, so `texld`'s internal derivatives must differ from
    what `dsx`/`dsy` return there in the last bits.

### Arithmetic (`g_summary.txt`, `g_analysis.txt`)

Each instruction ran in ps_3_0 on about 3,600 to 4,600 inputs, read from a point-sampled float
texture: special values, random exponents from 2^-60 to 2^60, dense ranges, and denormals. Every
input came through `mov` unchanged, denormals included.

The rest of the arithmetic, exact rules:
- **Denormals act as zero** in arithmetic, keeping their sign, and denormal results are flushed to
  zero.
  - For every instruction tested, each denormal input gives the same result as a zero of the same
    sign.
  - `exp(-127)` gives 0, where the C library gives a denormal, and `mul` and `add` results that
    would be denormal give ±0.
  - `mov` keeps denormals.
- **Zero times anything is +0**, infinity and NaN included, in `mul`, `mad`, `dp3`, `dp4`, `lrp`
  and `nrm`. -0 × 1, 0 × -1, 0 × ∞ and 0 × NaN all give +0. This is D3D9's legacy multiply rule.
- **`mad` is fused.** It equals `fmaf` bit for bit in all 4,115 normal cases. A separate `mul` then
  `add` is fused too: the driver contracts them.
- **Dot products, `lrp` and `nrm` have fixed forms:**
  - `dp3`/`dp4` equal the first product rounded, then an `fmaf` for each further component in
    order, in all 4,100 normal cases;
  - `dp2add` equals `fmaf(a.y, b.y, a.x * b.x) + c`;
  - `lrp` equals `fmaf(s0, s1 - s2, s2)`;
  - `nrm` is within 2 ulps of `x / sqrt(dot)`, and `nrm` of a zero vector is 0.
- **`min`/`max`** return the other operand when one is NaN, and order -0 below +0.
- **`cmp`** is ≥ 0, with -0 (and negative denormals, as -0) counting as ≥ 0. `frc` is x - floor(x).
- **`_pp` changes nothing.** Every `_pp` result (`rcp`, `rsq`, `log`, `exp`, `pow`, `sincos`,
  `mul`) is bit-identical to the full precision instruction.

The transcendentals are close to the C library, but not exact:

| Instruction | Exact | Within 1 ulp | Max error | Range |
|---|---|---|---|---|
| `rcp` | 3,585 of 4,119 | all | 1 ulp | all normal inputs |
| `rsq` (of \|x\|) | 2,716 of 4,119 | 4,101 | 2 ulps | all normal inputs |
| `exp` | 2,251 of 4,579 | 4,549 | 2 ulps | -130 to 130 |
| `log` (of \|x\|) | 1,134 of 2,563 | all but 36 | 2 ulps | \|x\| below 0.5 or above 2 |
| `log` near 1 | 398 of 1,556 | 886 | 1.8e-7 absolute | \|x\| 0.5 to 2 |
| `pow` (of \|x\|) | 501 of 3,713 | 1,064 | 163 ulps, 1.2e-5 relative | \|x\| 2^-10 to 2^11, y -8 to 64 |
| `sincos` | 442 cos, 475 sin of 2,374 | about half | 3.8e-7 absolute | \|x\| ≤ π |
| `sincos` | about 2% | about 5% | 9.5e-6 absolute | π < \|x\| ≤ 100 |
| `sincos` | none of 184 | none | 0.49 absolute | \|x\| > 100 |

Special inputs:
- `rcp(±0)` is ±∞, `rsq(±0)` is +∞, and `rcp`/`rsq(±∞)` is 0.
- `log(±0)` is **-∞**, where the vertex interpreter uses -FLT_MAX as the documentation says, and
  `log(±∞)` is +∞.
- `exp(128)` is ∞ and `exp(-∞)` is 0.
- `pow`:
  - `pow(x, 0)` is 1 for any x, NaN and ∞ included;
  - `pow(±1, y)` is 1 for any y, NaN included;
  - `pow(0, -1)` is ∞.
- `sincos` of ±∞ or NaN is NaN, and sin(-0) is +0.

### Unbound samplers (`c_ps1x.txt`)

Sampling a stage with no texture gives (1, 1, 1, 1) for a 2D sampler in ps_1_1, ps_1_4, ps_2_0
and ps_3_0. It gives (0, 0, 0, 0) for a cube or volume sampler in ps_3_0.

### Alpha test precision (`h_fragments.txt`)

With ALPHAREF 128, the alpha test compares at the precision of the render target's format:
- **A8R8G8B8, GREATEREQUAL:** fails at 0.500122 (127.53/255) and passes at 0.500244 (127.56/255).
  That isn't plain rounding to 8 bits, which would pass from 127.5.
- **A16B16G16R16F:** passes from the fp16 value nearest 128/255 (0.50195).
- **A32B32G32R32F:** passes exactly from 128/255 as an fp32 (0x3f008081). EQUAL passes only that
  value.

So the alpha test can't be emulated exactly on the CPU without knowing the format's conversion.

### Reading inputs back, and which fragment (`h_fragments.txt`)

Setup:
- **Draw:** one draw of three overlapping full-target quads.
- **Each quad's inputs:** t0 = (id / 16, depth, 0, alpha), t1 = a texkill flag, t2 = an
  interpolated value.
- **States:** LESS, LESSEQUAL with equal depths, no depth test, no depth write. Each with the
  nearest or last fragment rejected by texkill or the alpha test.
- **Shader models:** ps_2_0, ps_3_0, and ps_2_x with `dsx` in the appended part.

The truth is the original shader drawn alone into an A8R8G8B8 target with its states. Four
methods were compared at one pixel:

| Method | Right | Wrong | Can't tell |
|---|---|---|---|
| A replacement writing only the inputs, the draw's depth state, alpha test off | 6 | 21 | |
| A replacement after the original, ZFUNC EQUAL, no depth write | 12 | 6 | 9 (no depth test) |
| The original with its inputs written to extra targets, oC0 to the original A8R8G8B8 | 27 | | |
| The same with oC0 to A32B32G32R32F | 27 | | |

- **The plain replacement** reads the last fragment that passed the depth test. That fragment is
  wrong whenever texkill or the alpha test rejected it.
- **Depth EQUAL** fixes the depth-tested cases, but not equal depths, no depth write or no depth
  test.
- **The original with extra outputs** keeps its own texkill and alpha test, so it reads exactly
  the fragment the GPU wrote.
  - The interpolated t2 it reads equals the replacement's for the same fragment (2.375, 0.625).
  - ps_2_x's appended `dsx(t2)` reads the triangle's gradient (0.125).
  - D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS is set, so an 8-bit oC0 can go with float extra targets.
  - With a float oC0 the result was also right here, but the alpha test's precision depends on
    the format (above), so oC0 should keep the original format.

## Decisions for 5.5 to 5.10

Recorded in STATUS.md's "Step 5 plan".

- **5.5, ps_2_0 to ps_3_0 interpreter:**
  - Use fp32 and ignore `_pp`.
  - Treat denormals as signed zero in arithmetic and flush denormal results, but not in `mov`.
  - Make 0 × anything +0 in every multiply.
  - Fuse `mad`, and use the measured forms of `dp3`, `dp4`, `dp2add` and `lrp`.
  - `min`/`max` drop NaN.
  - The transcendentals go through a callback like sampling. The CPU version uses the C library
    with the special cases above, and log(0) is -∞.
  - The driver's contraction of a separate `mul` and `add` can't be predicted, so tests allow a
    small relative difference there.
- **5.6, ps_1_x:**
  - Registers are fp32 with no range clamp (not even at 65504).
  - Constants are clamped to [-1, 1] when read, and the output to [0, 1].
  - `texcoord` clamps to [0, 1]; `texcrd`, `tex` and `texld` don't.
  - Modifiers don't clamp, except `_sat`.
  - `cnd` is > 0.5 and `cmp` is ≥ 0, at fp32.
- **5.7, inputs from the GPU:**
  - The replacement for ps_1_x, ps_2_0 and ps_2_x shaders is ps_2_x, not ps_2_0, since ps_2_0 has
    no `dsx`/`dsy`. For ps_3_0 it is ps_3_0.
  - It writes each input's value, `dsx` and `dsy`. The interpreter builds the quad's other three
    pixels from them, matching the GPU's per-quad derivatives (exact for inputs that are linear on
    screen). It never reads them from neighbouring pixels of the target, which another triangle
    may have written.
  - vPos is computed (the pixel's coordinates).
  - vFace is read with `cmp` (it is ±1). Only ps_3_0 has either, and a ps_3_0 shader is always
    drawn with a vs_3_0.
  - Inputs need no conversion: colour and texcoord interpolation are identical, and the clamps are
    the GPU's.
- **5.8, sampling:** a one-pixel `texldd` draw with the interpreter's derivatives. Unbound
  samplers come out of the draw as the GPU gives them. Transcendentals go through the same GPU
  path, since they differ from the C library by 1 to 2 ulps (`pow` up to 163, `sincos` more for
  large arguments).
- **5.10, which fragment:**
  - For ps_2_0 and later, run the original shader with its inputs (and their `dsx`/`dsy`) written
    to extra render targets.
    - A ps_2_0 shader is moved up to ps_2_x for `dsx`/`dsy`.
    - Its own texkill, alpha test and oC0 format are kept.
    - It takes as many passes as the free targets need.
  - ps_1_x shaders can't have extra outputs: they get the plain replacement, which reads the last
    fragment that passed the depth test. Texkill or the alpha test rejecting that fragment isn't
    covered for them.

## Not covered

- **Other GPUs and drivers.** These are NVIDIA's rules on one driver. Derivative granularity, the
  zero-multiply rule, denormal flushing and the transcendentals' precision may differ elsewhere.
  The decisions that use the GPU (sampling, transcendentals, which fragment) adapt; the CPU rules
  (5.5, 5.6) are this GPU's.
- **Vertex shaders.** Their arithmetic wasn't probed. The vertex interpreter gives log(0) as
  -FLT_MAX, doesn't flush denormals and doesn't fuse `mad`, and VS Out still matched the GPU in
  its tests. Before 5.5 shares an ALU between them, the same rules need checking in a vertex
  shader.
- **ps_2_0's own arithmetic** wasn't probed separately: the arithmetic ran as ps_3_0, and ps_2_0
  only in the input, coverage and fragment sections.
- **The exact 8-bit conversion for the alpha test** isn't pinned down: the threshold lies between
  127.53 and 127.56 of 255.
- **Sampling:** `texldp`, `texldb`, cube and volume `texldd`, sRGB and shadow comparison weren't
  compared with `texld`. Neither were derivatives inside flow control.
- **Mul and add contraction:** the driver fused a `mul` followed by an `add`. Whether it always
  does isn't known.
- **5.10's own subjects** weren't probed: multisampling, centroid and point sprites.
