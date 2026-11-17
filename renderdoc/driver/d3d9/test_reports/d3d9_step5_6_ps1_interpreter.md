# D3D9: the ps_1_x interpreter (step 5.6)

Step 5.6 of the shader debugging plan (STATUS.md, "Step 5 plan"): ps_1_1 to ps_1_4 in the pixel
shader interpreter that 5.5 started for ps_2_0 to ps_3_0. Nothing visible changes yet: ps_1_x
debugging is turned on in 5.9.

Tested on the commit that adds it, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124).

## What was added

`d3d9_ps_emulate.h` and `.cpp` now run ps_1_1, ps_1_2, ps_1_3 and ps_1_4 (ps_1_0 is refused):

- **Registers:** ps_1_1 to ps_1_3's `t#` are texture registers (`PSRegisters::t`) that the
  texture instructions write and arithmetic reads; the coordinates stay in `PSInputs::t`.
  ps_1_4's `t#` are the coordinates, read by `texcrd`, `texld` and `texkill`.
- **Texture instructions:**
  - ps_1_1 to ps_1_3: `tex`, `texcoord`, `texbem`, `texbeml`, `texreg2ar`, `texreg2gb`,
    `texreg2rgb`, `texm3x2pad`, `texm3x2tex`, `texm3x2depth`, `texm3x3pad`, `texm3x3`,
    `texm3x3tex`, `texm3x3spec`, `texm3x3vspec`, `texdp3`, `texdp3tex` and `texkill`. Each
    samples the stage, and reads the coordinate set, that its destination names.
  - ps_1_4: `texcrd`, `texld` (with `_dz` and `_dw`), `texdepth`, `bem`, `phase` and `texkill`.
    `texld r#` samples the stage its destination names.
- **The rest:** `cnd`, the ps_1_x source and result modifiers, and co-issued pairs, which run as
  one step.
- **Texture stage state** (`PSStageState`, passed to `PSQuad::Start`): the bump matrix, the
  luminance scale and offset, and whether `D3DTTFF_PROJECTED` is set. 5.9 fills it from the
  capture.
- **Lookups** go through `PSCallbacks::Sample` like ps_2_0's, with the coordinate the
  instruction worked out (bumped, reflected, projected) and its derivatives across the quad.
  `PSSample::type` is `Unknown`, since ps_1_x declares no samplers: the texture bound decides.
- **Divides** (`_dz`, `_dw`, projected `tex`, the reflection, the two depth instructions) are a
  multiply by `rcp` through `PSCallbacks::Transcendental`, as the GPU does it (below).
- `PSThread::Step` is split into `Step` and `Execute`, so a co-issued pair can run both halves
  before either writes. `PSQuad` now asks each pixel for its quad operand (`QuadOperand`), which
  for ps_1_x is the lookup's coordinate.

## What the GPU does with ps_1_x

A temporary generator (`d3d9_step5_6_ps1_interpreter/generator/d3d9_ps1_emu_data.cpp`, a demo
test that was in `demos.vcxproj` only to run it) assembles 29 ps_1_1 to ps_1_4 shaders with
`D3DAssemble`. It runs each natively on a 4x4 A32B32G32R32F target, with a different 16x16 float
texture on each of stages 0-5 and a different bump matrix per stage, and reads back r0 and the
inputs each pixel saw. These are the GPU's results, not RenderDoc's. They settled what step 5.4
hadn't measured:

| Question | What the GPU did | Case |
|---|---|---|
| `texcoord`'s w | Clamped to [0, 1] like x, y and z: w = 1.3 gave 1, -0.05 gave 0, 0.85 stayed | `texcoord_1_1` |
| ps_1_1's `texkill t#`: coordinate or register? | The coordinate: the validator refuses `texkill` on a register `tex` wrote | `texkill_1_1` |
| `texkill`'s components | x, y and z only: pixels whose coordinate has only w negative survive, in ps_1_1 and ps_1_4 | `texkill_1_1`, `texkill_w_1_1`, `texkill_1_4` |
| Co-issued pairs | Both read the registers as they were before either writes: `mul r0.rgb, ...` + `mov r0.a, r0.b` gives the old b | `coissue_1_1`, `coissue_1_4` |
| `D3DTTFF_PROJECTED` with a vertex shader | ps_1_1's `tex` divides by w, whether the flags say COUNT3, COUNT4 or no count. ps_1_4 ignores it | `proj_*` |
| `texbem` | u' = u + M00·r + M10·g, v' = v + M01·r + M11·g, with the destination stage's matrix | `texbem_1_1` |
| `texbeml`'s luminance | b · scale + offset clamped to [0, 1] (-0.625 gave 0, 1.375 gave 1), scaling all four channels | `texbeml_1_1` |
| `texm3x3spec`'s eye constant | Not clamped to [-1, 1], unlike every other constant read: c0 = (1.5, -0.75, 2.25) only matched unclamped | `m3x3spec_1_1` |
| `texm3x3`'s w | 1 | `m3x3_1_2` |
| `texcrd` | Not clamped; `_dw` divides x and y by w, as x times `rcp(w)`: a true divide was 1 ulp off in 6 of 64 values, the reciprocal in 2 (the GPU's `rcp` is within 1 ulp of the C library, 5.4) | `texcrd_1_4`, `texcrd_dw_1_4` |

The assembler's validator answered some questions itself, so the interpreter doesn't have to
handle them:
- ps_1_x constants take no source modifiers.
- A register written by `texkill` or `texm*pad` can't be read, so where the pad's dot product is
  kept doesn't show. The interpreter keeps it in that register's x, where a debugger can show it.
- `texcrd` writes `.xyz`, or `.xy` with `_dw`.
- `_dz` is only allowed on `texld` from an r#, and only after `phase`.
- With a `phase` marker, `texkill` must come after it.
- Alpha written before `phase` is lost "on certain hardware", and reading it afterwards is refused.
  The interpreter keeps the value, which no shader can read.

## Tests

`renderdoccmd test unit "[psemu]"`:

- **Against the GPU** (`d3d9_ps1_test_data.h`, from the generator): all 29 cases discard the
  same pixels and match r0 bit for bit, except two values in `texcrd_dw_1_4` that are 1 ulp
  (5.96e-08) apart through the GPU's `rcp`. 5.8's GPU transcendentals will remove that
  difference. `statistics.txt` has the numbers from `[d3d9psstats]`, with 5.5's 26 cases, which
  still match as before.
  - The cases: arithmetic with clamped constants, modifiers, `cnd`, `cmp`, `dp4` and co-issue
    (`arith_1_1`, `arith_1_2`, `arith_1_4`); `texcoord`; `texkill`; `texbem`, `texbeml` and
    `bem`; the `texreg2*` lookups; `texm3x2tex`, `texm3x3tex`, `texm3x3spec` (with and without
    out-of-range constants), `texm3x3vspec`, `texm3x3`, `texdp3` and `texdp3tex`; projection;
    `texcrd` and `texld` with `_dz` and `_dw`; co-issue.
  - Point sampling only lands exactly when a coordinate isn't on a texel edge. So the texel values
    and bump matrices put every bumped or register-read coordinate on a texel centre. The
    dot-product coordinates (`texm3x2tex`, `texm3x3tex`, `texm3x3spec`, `texm3x3vspec`) can't
    be placed: of the 160 lookups in those five cases (the `tex t0` before them included), 9 came
    within 1/64 of a texel of an edge, and all matched anyway.
- **Hand-written** (`D3D9 pixel shader emulation - ps_1_x`), with a stub sampler that returns
  the coordinate and stage:
  - constants clamped before the modifier, registers unlimited, r0 clamped when written out, and
    ps_2_0's constants not clamped;
  - `cnd` at 0.5 and its next float, and with NaN;
  - a co-issued pair reading before it writes, as one step;
  - each texture instruction's formula, stage and derivatives;
  - `texbeml`'s clamp at both ends;
  - the pad registers; `texm3x2depth` and `texdepth`; projection in ps_1_1 and not in ps_1_4;
  - `texkill` on coordinates and registers;
  - a ps_1_4 shader with `texcrd`, `_dw`, `bem`, `phase`, `_dz` and `texdepth`.
- As in 5.5, every run is stepped again a pixel at a time, checking that each step's reported
  changes account for everything that changed, including the `t#` registers, `oDepth` and the
  final copy of r0 to oC0.
  - This caught a co-issued pair writing two parts of one register (`r0.rgb` and `r0.a`): it was
    reported twice, the second time with the half-written value as "before". It's now one change
    with the value from before the pair.

## Found along the way

- The generator first printed the texel function through `printf`, which ate its `% 16`, and
  wrote whole floats as `2f`. Both were fixed in the generator before the data was kept, and the
  data header is generated output.

## Choices without a measurement

- `texm3x2depth` and `texdepth` write oDepth unclamped, with 1 when the divisor is 0. Their
  depth wasn't compared with the GPU (nor was 5.5's oDepth).
- The form of `texbem`'s and `bem`'s arithmetic: the offset is `fma(M10, g, M00 * r)`, added to u
  (the form `dp2add` was measured to have). The test values are exact either way, so the order is
  untested.
- The reflection in `texm3x3spec` and `texm3x3vspec` is 2·N·(N·E)·rcp(N·N) − E. Only its lookups
  were compared, which tolerate the last bits.
- The components a lookup doesn't use are 0: (u, v, 0, 0) for `texbem`, `texreg2ar`/`gb`,
  `texm3x2tex` and `texdp3tex`. They only matter for cube and volume textures.
- `phase` keeps r#'s alpha, which no valid shader can read afterwards.
- The divisor of a projected `tex` is w whatever the count. That was measured with a vertex
  shader; fixed function vertex processing with `D3DTTFF_COUNT2` or `COUNT3` wasn't.

## Not covered

- The GPU's 3D lookups for `texm3x3tex`, `texm3x3spec`, `texm3x3vspec` and `texreg2rgb` sampled
  2D textures here. Cube and volume textures, mips and filtering are 5.8's (sampling on the GPU).
- oDepth from `texdepth` and `texm3x2depth` against the GPU.
- ps_1_x's integer-like formats (`D3DFMT_V8U8` and friends for bump maps): the test textures are
  float. The sampler callback returns whatever the format gives, so this is 5.8's too.
- Fixed function vertex processing feeding ps_1_x (projection counts), and other GPUs.

## Other checks

On the final code (`verify_summary.txt`):
- `renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors (the demos
  without the generator).
- `renderdoccmd test unit "[d3d9]"`, x64 and x86: all 17,950 assertions in 23 test cases pass.
- The automated tests (`run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`, x64): 19 of 19 pass.
  Nothing on the capture side changed, so the x86 run wasn't needed.
- The vertex interpreter didn't change: the Development `[d3d9bench]` gives 1036 ms, as after the
  force-inline change (1035-1037 ms).
