# D3D9: debugging pixels, with their inputs from the GPU (step 5.7)

Step 5.7 of the shader debugging plan (STATUS.md, "Step 5 plan"): `DebugPixel`, so the texture
viewer's Debug button and Python's `DebugPixel` debug a pixel of a ps_2_0 to ps_3_0 shader that
doesn't sample textures. Shaders that sample (5.8) and ps_1_x (5.9) say why they can't be debugged
yet.

Tested on the commit that adds it, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124). Files
are in `d3d9_step5_7_debug_pixel/`.

## How it works

Like D3D11's, the pixel's inputs come from the GPU and the shader runs on the CPU, in the
interpreter 5.5 and 5.6 wrote, for the pixel's 2x2 quad. D3D9 has no UAVs, so:

- **The event is drawn again** with generated pixel shaders (`BuildInputReadback`,
  `d3d9_ps_debug.cpp`) that write the inputs to A32B32G32R32F render targets of our own, four
  values a draw, read back through a 1x1 target:
  - for ps_3_0, a ps_3_0 shader with the original's input declarations, so the rasterizer
    interpolates them the same way (semantics, masks, `_centroid`). vFace is read with `cmp`,
    since it can only be a condition, and vPos is the pixel's position (5.4).
  - for ps_2_0 and ps_2_x, a ps_2_x shader with their `t#` and `v#` declarations, since ps_2_0
    has no `dsx`. ps_1_x gets the same from the `t#` and `v#` it reads, for 5.9.
  - The replay goes back to before the draw, so depth and stencil are what it tests against, and
    the readback draws don't write them. Blending, alpha test, fog and sRGB writes are off;
    everything else is the draw's. The scissor rect is the pixel alone, and an occlusion query
    says whether the draw writes it at all.
- **The quad's other pixels.** `dsx` is a quad's top row's difference and `dsy` its left
  column's (5.4), so the interpreter needs the top left, top right and bottom left pixels'
  inputs:
  - ps_3_0's readback reads them in the pixel itself. With `cmp` on vPos's parity it keeps the
    input in one pixel of the row or column and 0 in the other, so `dsx` or `dsy` of that is the
    input in that pixel, exactly.
  - ps_2_x has no vPos. The readback also reads each input's `dsx` and `dsy`, and is drawn again
    at the quad's other pixels. A neighbour's values are used when its derivatives there are the
    pixel's bit for bit, which means the same primitive covered it. Otherwise they're worked out
    from the pixel's values and derivatives, which is exact from the top row and left column, and
    nearly so from the bottom right.
- **The trace** (`D3D9Emu::PSDebugger`) steps the pixel through `PSQuad` and describes it as the
  vertex debugger does: the stepping, constants, samplers and disassembly lines are shared
  (`StepDebugger`, split out of `VSDebugger`). Its inputs are `v#`, `t#`, `vPos` and `vFace`,
  by semantic and by register (see "UI"); its registers `r#`, `aL`, `p0`, `oC#` and `oDepth`
  (ps_1_1-1_3's `t#` too).
- Each pixel shader's reflection says whether it can be debugged, or why not, which the UI shows.
- `d3d9_bytecode_test.h`'s helpers for writing bytecode a token at a time moved to
  `d3d9_bytecode_build.h`, since the replay uses them now.

## The first attempt: neighbours from the derivatives

The plan was to work the quad's other pixels out from the debugged pixel's value and its
derivatives. The first zoo run showed what that costs: at the bottom right pixel of a quad
(141, 61) the ps_2_x draw's `oC0` was 8.2e-5 off. That draw's `oC0` is `v + ddx(v) * 16` with `v`
a product of inputs. From the bottom right, the top left is the pixel minus both derivatives,
which with perspective isn't quite where the GPU's top left was, and a derivative magnifies the
difference. So ps_3_0 now reads the neighbours with vPos, and ps_2_x at the neighbours, as above.
The unit tests check both against inputs that aren't linear across the quad.

## What the GPU does

The remaining differences are the driver fusing a `mul` and the `add` after it into one rounding,
which 5.4 had seen in ps_3_0. fxc wrote them apart in the ps_2_0 and ps_2_a shaders here: ps_2_0's
`mul r0, r0, r1.x` then `add r0, r0, r1`, and ps_2_x's `mul r0, t1.yxwz, t1` then
`add r0, r0, v0`.

- ps_2_0 at (40, 60): `oC0.x` is 0x3fc86331 on the GPU. With the `mul` and `add` apart, as the
  bytecode says and the interpreter runs it, it's 0x3fc86332; fused, 0x3fc86331.
- ps_2_x at (140, 60): `oC0.z` is 0.0519179367 on the GPU and 0.051918 in the debugger. With the
  inputs the debugger read at (140, 60) and (141, 60), fusing that pair gives 0.0519179367 exactly
  (`fusecheck.py`, `fusecheck.txt`, run on the test's capture). The 1 ulp in `v` becomes 17 ulps
  after `ddx(v) * 16`.
- ps_3_0: fxc wrote its loop as `mul r3, r2, c0` then `add r2, r3, v2`, three times, so its
  differences (1 to 12 ulps) are the same.

So the inputs read back are right. The interpreter can't know where the driver fuses, so the test
allows a small difference and logs each one.

## Tests

`D3D9_Shader_Debug_Zoo` has three new pixel draws, to two A32B32G32R32F targets, each two
triangles with different w at each corner so the inputs are interpolated with perspective:

- `ps_2_0`: arithmetic on a colour and three texture coordinates (one `.xy`) with constants;
- `ps_2_x` (`ps_2_a`): `ddx` and `ddy` of a product of inputs and of an input, two targets;
- `ps_3_0`: vPos, vFace (one triangle clockwise, the other not), a loop, a branch, derivatives,
  two targets and oDepth.

Natively the demo writes what the GPU wrote at 7 pixels of each (a whole quad, and three others
either side of the diagonal) to `D3D9_Shader_Debug_Zoo_pixels.txt` (`pixels.txt` here). These are
the GPU's results; the test debugs each pixel at replay and checks:

- the replay's pixels are the GPU's bit for bit, at all 21;
- the debugger's `oC#` against the GPU's: bit for bit, or within 1e-6 relative (ps_2_0) or 1e-5
  (with derivatives), relative to the value and at least 1;
- `oDepth` against the D24S8 target, within its 24 bits;
- `process_trace` steps forwards and backwards consistently;
- the ps_3_0 trace: vPos is the pixel, vFace is +1 at (223, 37) and -1 at (270, 100), the inputs
  are listed by register too (`Registers.vFace`), `oC0`, `oC1`, `oDepth` (1 column) and `r0` are
  listed, and every step is on an instruction.

And that a ps_3_0 draw that samples, a ps_1_1 draw, a pixel outside a draw and the fixed function
draw's pixel can't be debugged, the first two saying why.

Results (`tests_x64.txt`):

```
** ps_2_0: 7 pixels debugged, 21 of 28 output components bit for bit the GPU's, the others at most 9.32e-08 off (the driver fuses a mul and an add)
** ps_2_x: 7 pixels debugged, 39 of 56 output components bit for bit the GPU's, the others at most 2.8e-06 off (the driver fuses a mul and an add, then ddx and ddy)
** ps_3_0: 7 pixels debugged, 49 of 56 output components bit for bit the GPU's, the others at most 7.96e-08 off (the driver fuses a mul and an add)
** ps_3_0: vPos and vFace are the pixels', the inputs are listed by register too, the outputs are listed, and every step is on an instruction
** The ps_3_0 texture pixel shader can't be debugged: 'Debugging pixel shaders that sample textures isn't supported yet'
** The ps_1_1 pixel shader can't be debugged: 'Debugging ps_1_x pixel shaders isn't supported yet'
** A pixel outside the draw isn't debugged
** The fixed function draw can't be debugged: 'Fixed function vertex processing isn't a shader'
```

Each of the 31 components that isn't bit for bit is logged with its ulps (in `tests_x64.txt`):
ps_2_0's and ps_3_0's are 1 to 12 ulps, ps_2_x's up to 256 ulps of a small value, where a `ddx` or
`ddy` of a fused result magnifies the difference.

Unit tests (`renderdoccmd test unit "[psdebug]"`, in `d3d9_ps_debug.cpp`), with hand-assembled
shaders, running the generated readback shaders through the interpreter:

- the readback's passes, items, versions and declarations for ps_2_0 (with and without gradient
  instructions), ps_3_0 (copied declarations with `_centroid` and two semantics in one register,
  vFace) and ps_1_1 and ps_1_4;
- the quad built again exactly from every pixel of it: with linear inputs from the derivatives,
  and with inputs that aren't linear (and zeros of both signs) from ps_3_0's neighbours and from
  ps_2_x's neighbours read where the same primitive covered them. A neighbour with other
  derivatives falls back to the extrapolation, which isn't exact from the bottom right;
- a shader with no inputs still gets a pass, writing 0;
- the debugger on a ps_3_0 shader with `dsx`, vPos, vFace and oDepth from each pixel of the quad
  (outputs as `PSQuad` runs them, inputs, registers, source variables, no declaration steps), and
  on a ps_1_1 shader (`texcoord#` inputs, the `t0` register, `r0` to `oC0` as the last step);
- which instructions sample.

## UI

qrenderdoc on the zoo capture, driven from inside by a script (`dbg_driver3.py`, through
`InvokeOntoUIThread`), the screenshots taken with PrintWindow of qrenderdoc's window alone. The
script calls `DebugPixel` and opens the shader viewer with the trace, as the texture viewer's
Debug button does:

- `01_texture_viewer_ps_3_0.png`: the ps_3_0 draw's first target with pixel (270, 100) picked,
  (-1.71539, -0.56119, -2.99522, -1.85049), the GPU's value. The Pixel Context's Debug button is
  enabled.
- `02_ps_3_0_stepping.png`: that pixel stopped on the loop's `endrep`. The inputs are listed by
  semantic with their registers: COLOR (`v0.xyzw`), TEXCOORD0-2, VPOS (270, 100), VFACE (-1, the
  counter-clockwise triangle), the constant `k0` from the constant table; the registers `r0`-`r3`
  (the ones changed in red), `oC0`, `oC1`, `oDepth`.
- `03_ps_3_0_end_watches.png`: at the last step, watches on `vFace`, `vPos`, `v0`, `oC0` (the
  GPU's value) and `oDepth` (0.58224).

**Found here:** before the end, the watches on `vFace`, `vPos` and `v0` said "Couldn't find
variable" (`04_watch_before_aliases.png`). The viewer lists inputs only under their semantics
(VFACE), so the disassembly's register names don't resolve, for vertex shader inputs too since
5.2. The traces now also have a source variable per input register under "Registers"
(`Registers.vFace`), as 5.3 did for the constant table's registers, and the zoo test checks
they're there.

## Other checks

On the final code (`verify_summary.txt`):

- `renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors.
- `renderdoccmd test unit "[d3d9]"`, x64 and x86: all 19,025 assertions in 25 test cases pass.
- The automated tests (`run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`): 19 of 19 pass in x64, and
  19 of 19 with the 32-bit demos (`--x86`, `tests_x86_summary.txt`), since the zoo demo changed.
- The vertex interpreter didn't change: the Development `[d3d9bench]` gives 1038-1047 ms
  (1036-1037 ms before; this is noisy).

## Not covered

- Shaders that sample (5.8) and ps_1_x (5.9).
- Which fragment: with overlapping triangles in one draw, the readback gets the last fragment that
  passes the depth and stencil test as they were before the draw; with alpha test, `texkill` or
  `clip`, maybe one they reject (5.10).
- Multisampled targets: `DebugPixel` refuses them until 5.10. Point sprites and centroid weren't
  drawn.
- A GPU without ps_2_x gradient instructions, where the quad's pixels would all get the debugged
  pixel's inputs. Other GPUs.
- The ps_2_x neighbours drawn outside the draw's own scissor rect: the zoo draws have none.
