# D3D9: the ps_2_0 to ps_3_0 interpreter (step 5.5)

Step 5.5 of the shader debugging plan (STATUS.md, "Step 5 plan"): a portable, unit tested pixel
shader interpreter for ps_2_0, ps_2_x and ps_3_0. It runs a 2x2 quad in lockstep, samples
through a callback, and follows the arithmetic the GPU was measured doing in step 5.4
(`d3d9_step5_4_native_probes.md`). Nothing visible changes yet: `DebugPixel` comes in 5.7.

Tested on the commit that adds it, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124).

## What was added

- `d3d9_ps_emulate.h` and `.cpp` (`D3D9Emu`):
  - `PSEmulator::Init` takes a ps_2_0, ps_2_x or ps_3_0 program, matches up its flow control and
    notes each sampler's declared type. Vertex shaders, ps_1_x (5.6) and unparsed programs are
    refused with the reason.
  - `PSQuad` runs the four pixels of a 2x2 quad (top left, top right, bottom left, bottom
    right). `Step(pixel, &changes)` runs one instruction in every pixel that hasn't finished and
    records what `pixel` changed, with the old values, as the vertex interpreter does. `Run` runs
    them all to the end.
  - `PSThread` is one pixel's run: `Registers()` has `r0`-`r31`, `p0`, the loop and call stacks,
    `oC0`-`oC3` and `oDepth` (with which were written), and whether `texkill` discarded it.
  - `PSInputs` is what the rasterizer gives a pixel: `v0`-`v9`, `t0`-`t7`, `vPos` and `vFace`.
    5.7 fills it from the GPU.
  - `PSCallbacks`: `Sample` gets each lookup as a `PSSample`: the kind (`texld`, `texldp`,
    `texldb`, `texldd`, `texldl`), the sampler and its declared type, the coordinate, its
    derivatives and the bias or level. `Transcendental` gets `rcp`, `rsq`, `exp`, `log`, `pow` and
    `sincos` (and `nrm`'s `rsq`), and by default calls `CPUTranscendental`.
- Shared with the vertex interpreter, moved out of `VSEmulator` without changing what it does:
  `ProgramLayout` (flow control matching, labels and literals), `FlowRegisters` (`p0`, the loop and
  call stacks, `aL`) and the small helpers (`Saturate`, `Compare`, `ApplyMod`,
  `ConstantRegisterOffset`, now `D3D9_FORCE_INLINE` in `d3d9_vs_emulate.h`; see "Speed" below). The
  fixed function code's own copy of `Saturate` was the same, and now uses the shared one.
- `d3d9_bytecode_test.h`'s `Assembler` can assemble pixel shaders.
- `d3d9_ps_test_data.h`: compiled shaders with the GPU's results (below).

## How it follows the GPU

From 5.4's measurements:
- **Derivatives are per quad:** `dsx` is the top row's difference and `dsy` the left column's, in
  all four pixels. `texld`, `texldp` and `texldb` take their derivatives the same way, `texldp`'s
  of the coordinate after the divide by w.
- Before each step the quad reads, for each pixel whose next instruction needs them, that
  instruction's source in all four pixels. So `dsx r0, r0` sees its neighbours' `r0` from before
  the instruction, as on the GPU.
- **Arithmetic:** denormal sources act as zero and denormal results are flushed, keeping the
  sign; `mov` keeps denormals. Zero times anything is +0 in `mul`, `mad`, the dot products, `lrp`
  and `nrm`. `mad` is `fmaf`; `dp3`/`dp4` round the first product and fuse the rest in order;
  `dp2add` and `lrp` have the measured forms. `min`/`max` drop NaN and put -0 below +0. `cmp`
  counts -0 as ≥ 0. `_pp` is ignored.
- **Transcendentals:** `CPUTranscendental` is the C library with the GPU's special cases:
  `rcp(±0)` = ±∞, `rsq` and `log` of |x|, `log(0)` = -∞, `pow(x, 0)` and `pow(±1, y)` = 1,
  `sin(-0)` = +0. They're within a few ulps of the GPU; 5.8 can run them on the GPU through the
  callback.
- **`texkill`** marks the pixel discarded, and it runs on to the end, as the GPU keeps running it
  for its neighbours' derivatives.

Choices 5.4 didn't measure, made here:
- `cmp` moves the value it picks unchanged, denormals included, like `mov`; only the condition is
  flushed. `abs` keeps denormals too.
- `setp`, `if_*` and `break_*` compare flushed values, as `cmp` was measured to.
- `texkill` tests the components in its write mask (all four in compiled shaders, w included from
  ps_2_0 on), with denormals as zero.
- `nrm`'s w is src.w times the same factor, as documented.
- `crs` is two products and a subtraction per component, not fused.
- `vPos.zw` are 0.
- Pixels can take different paths through dynamic flow control; their neighbours' values for
  derivatives are then whatever those hold, which is undefined on the GPU too.

## Tests

### Hand-assembled (`[d3d9][psemu]`)

`D3D9 pixel shader emulation - arithmetic as measured` checks the special cases 5.4 measured
(`g_analysis.txt` there), each bit for bit:
- zero times ∞, NaN, -1 and -0; denormal inputs and results in `mul`, `add`, `frc`, `exp`, `rcp`,
  `log`; `mov` keeping denormals;
- `mad` against `fmaf` (with a case where unfused differs), the probe's `mad` cases with
  denormals, `dp3`, `dp4`, `dp2add` and `lrp` against their forms, `dp3` of ∞ and 0, `lrp` with
  NaN and ∞;
- `min`, `max` and `cmp` with NaN, -0 and negative denormals;
- the transcendentals' special cases, `sincos` with ps_2_0's extra operands and a partial mask,
  `nrm` of zero and infinite vectors, `_sat` of NaN, and `_pp`.

`D3D9 pixel shader emulation - quads`:
- derivatives: f = x·y on pixels (4, 2) to (5, 3) gives `dsx` 2 and `dsy` 4 in all four pixels,
  as the GPU did; `dsx r0, r0` reads the old `r0`; ps_2_x's `dsy` on `t#`.
- lookups: `texld`, `texldp`, `texldb`, `texldd` and `texldl` each reach the callback with the
  right kind, sampler, declared type, coordinate, derivatives and level; the sampler's swizzle
  applies to the result; without callbacks sampling fails with a reason.
- `texkill` on `t0`: a negative y or w discards, -0 and a negative denormal don't, and every
  pixel runs to the end.
- the pixels' own paths: `if_lt vPos.x` with a `loop` reading `v[aL]` in the left column only,
  `if_lt vFace` in one pixel, and `vPos` read back.
- predication with `setp_gt`; `rcp`, `sincos` and `nrm` going through the callback.

`D3D9 pixel shader emulation - what it won't run`: a vertex shader, ps_1_4, and 255³ loop
iterations.

Every hand-assembled run is stepped again for each pixel, checking that the changes each step
reports account for every register that changed, and that it ends the same.

### Compiled shaders against the GPU

A generator (`d3d9_step5_5_ps_interpreter/generator/d3d9_ps_emu_data.cpp`, added to the demos
only to run it) compiled 10 HLSL shaders with D3DCompile for ps_2_0, ps_2_a (ps_2_x) and ps_3_0
where they fit, and ran each natively on a 4x4 A32B32G32R32F target covered by one triangle:
- **inputs:** TEXCOORD0-3 and COLOR0-1, each an affine function of the screen position (through a
  vs_2_0 for ps_2_x, which clamps the colours, and a vs_3_0 for ps_3_0). Each pixel's inputs were
  read back by a shader declaring the same inputs, which 5.4 found reads them bit for bit, and
  vFace with `if_lt vFace`.
- **constants:** c0-c7, i0, b0 and b1.
- **texture:** 16x16 float, one level, point sampled with wrap; TEXCOORD3 puts each pixel at a
  texel's centre, so the CPU's point sampling picks the same texel.
- **outputs:** up to four targets, read back. A discarded pixel keeps the clear colour in all of
  them.

`d3d9_ps_test_data.h` holds the bytecode, the inputs and the GPU's outputs, and the unit test
`D3D9 pixel shader emulation - compiled shaders against the GPU` runs each case's four quads
through the interpreter, sampling the same texture on the CPU (`VSTexture`, point, one level). It
checks:
- every pixel the GPU discarded is discarded, and no other;
- every output the GPU wrote is within 1e-5 absolute or 1e-4 relative.

ps_2_0 had no room for the trigonometry (64 arithmetic slots), so that's a separate shader for
ps_2_a and ps_3_0. fxc's ps_2_a takes no flow control at all (no loops, no bool registers), so
the flow control shaders are ps_3_0 only; ps_2_x's flow control is in the hand-assembled tests,
on the same code.

Results (`statistics.txt`, from the hidden `[d3d9psstats]` test):

All 26 cases pass: no failures, every pixel discarded where the GPU discarded it.

| Case | Discarded | Exact values | Max difference | Max relative |
|---|---|---|---|---|
| `arith_ps_2_0` | 0 of 16 | 242 of 256 | 5.96e-08 | 1.25e-07 |
| `arith_ps_2_a` | 0 of 16 | 242 of 256 | 5.96e-08 | 1.25e-07 |
| `arith_ps_3_0` | 0 of 16 | 242 of 256 | 5.96e-08 | 1.25e-07 |
| `transcendental_ps_2_0` | 0 of 16 | 91 of 192 | 9.54e-07 | 1.57e-06 |
| `transcendental_ps_2_a` | 0 of 16 | 91 of 192 | 9.54e-07 | 1.57e-06 |
| `transcendental_ps_3_0` | 0 of 16 | 87 of 192 | 9.54e-07 | 1.57e-06 |
| `trig_ps_2_a` | 0 of 16 | 63 of 128 | 7.15e-07 | 5.12e-06 |
| `trig_ps_3_0` | 0 of 16 | 63 of 128 | 7.15e-07 | 5.12e-06 |
| `matrix_ps_2_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `matrix_ps_2_a` | 0 of 16 | 128 of 128 | 0 | 0 |
| `matrix_ps_3_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `staticflow_ps_3_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `flow_ps_3_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `deriv_ps_2_a` | 0 of 16 | 128 of 128 | 0 | 0 |
| `deriv_ps_3_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `texture_ps_2_0` | 0 of 16 | 256 of 256 | 0 | 0 |
| `texture_ps_2_a` | 0 of 16 | 256 of 256 | 0 | 0 |
| `texture_ps_3_0` | 0 of 16 | 256 of 256 | 0 | 0 |
| `texgrad_ps_2_a` | 0 of 16 | 64 of 64 | 0 | 0 |
| `texgrad_ps_3_0` | 0 of 16 | 64 of 64 | 0 | 0 |
| `texlod_ps_3_0` | 0 of 16 | 64 of 64 | 0 | 0 |
| `clip_ps_2_0` | 7 of 16 | 36 of 36 | 0 | 0 |
| `clip_ps_2_a` | 7 of 16 | 36 of 36 | 0 | 0 |
| `clip_ps_3_0` | 7 of 16 | 36 of 36 | 0 | 0 |
| `vface_ps_3_0` | 0 of 16 | 128 of 128 | 0 | 0 |
| `vface_ps_3_0_ccw` | 0 of 16 | 128 of 128 | 0 | 0 |

The outputs are the GPU's (read back natively); the comparison and the differences are
RenderDoc's interpreter against them.

- Everything without transcendentals is bit-exact: matrices, both kinds of flow control,
  derivatives, all five kinds of lookup (with a dependent read), `texkill` (7 of 16 pixels
  discarded, the same ones) and vFace/vPos in both windings.
- In `arith`, the 14 values that differ are all `normalize`'s, which uses `rsq`, at most 1.25e-7
  relative.
- The transcendental shaders differ by at most 9.5e-7 (1.6e-6 relative), the trigonometry by
  7.2e-7 (5.1e-6 relative). The C library's results aren't the GPU's in the last bits, as 5.4
  measured.
- The driver fusing a separate `mul` and `add`, which 5.4 saw, didn't show in any of these
  shaders: there's no difference outside the transcendentals.

## Speed of VS Out

The vertex interpreter's code moved, so `RunPostVS` was timed with the hidden `[d3d9bench]` test
(200,000 vertices of a 16-instruction vs_2_0 shader), old and new back to back. Release has no unit
tests, so `globalconfig.h` was switched for that build only and restored.

| Build | Before (HEAD) | First version | `static inline` | `D3D9_FORCE_INLINE` |
|---|---|---|---|---|
| Release | 150.1-151.3 ms | 200.0-202.2 ms | 137.9-139.1 ms | 146.8-148.3 ms |
| Development | 1093.7-1097.8 ms | 1124.8-1126.8 ms | 1016.0-1020.4 ms | 1035.0-1037.3 ms |

- The first version made the helpers (`ApplyMod`, `Saturate`, `Compare`,
  `ConstantRegisterOffset`) ordinary functions called from both interpreters. With two files
  calling them, the whole program optimisation stopped inlining them into the vertex loop: VS Out
  was a third slower in Release. Making them `inline` in the header gave 188.9-190.9 ms, still a
  quarter slower.
- As `static inline` in the header, each file has its own copy, as the vertex interpreter's were
  before. Release is then 8% faster than before; the code is otherwise the same, so that's likely
  layout. This was in d417ffd0f.
- A later commit replaced the `static` copies with a `D3D9_FORCE_INLINE` macro (`__forceinline`,
  or `always_inline` on gcc and clang), so the reason is in the code rather than in a comment
  about `static`. Timed back to back with the `static inline` code (139.1-140.6 ms in that
  session), Release is 147 ms: still 2% faster than before 5.5 and well clear of the regression,
  but 5% slower than letting the compiler choose with `static inline`. Forcing every call inline,
  `ApplyMod`'s switch included, likely makes the loop bigger. Kept for the clearer code.
- In the Development build (`/Od`, nothing inlined) the first version was 2.7% slower; the final
  one is 7% faster than before. Its final numbers come from the verification run, not back to back
  with the old code; the other columns are each from one session.
- The outputs are in `benchmark/`: `release.txt` (new, old, new again), `release_inline.txt`,
  `release_static.txt`, `development.txt`, and for the macro `release_static_again.txt`,
  `release_forceinline.txt` and `verify_summary_forceinline.txt`.

## Other checks

On the final code (`verify_summary.txt`):
- `renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors.
- `renderdoccmd test unit "[d3d9]"`, x64 and x86: all 16,989 assertions in 21 test cases pass.
- The automated tests (`run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`, x64): 19 of 19 pass, so VS
  Out and vertex debugging are unchanged by the moved code. Nothing on the capture side changed,
  so the x86 run wasn't needed.
- The generator was only in `demos.vcxproj` to run it; the demos were rebuilt without it.

## Not covered

- ps_1_x: refused, it's 5.6.
- Cube and volume lookups, mip levels, filtering and bias against the GPU: the test texture has
  one level and is point sampled. The callback gets the right request (checked by the hand tests);
  5.8 samples on the GPU.
- Derivatives inside dynamic flow control, where pixels diverge: not measured on the GPU (5.4
  left it out too).
- `oDepth` against the GPU (only in the hand tests), and `texkill` with a partial write mask.
- The unmeasured choices listed above.
- Other GPUs.
