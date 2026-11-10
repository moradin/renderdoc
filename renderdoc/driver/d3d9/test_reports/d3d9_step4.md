# D3D9 step 4: shader editing and replacement

Step 4 of the plan in STATUS.md: editing a capture's shaders from the pipeline state and replacing
them on replay, with "Edit Generated Stub". STATUS.md's "Shader editing" has how it works.

Tested on top of 1ed60972a, with the changes in the commit that adds this report.

- **Machine:** Windows 11, RTX 4070 Ti, NVIDIA driver 32.0.16.1088.
- **Builds:** `renderdoc.sln` and `util\test\demos.sln`, Development, x64 and x86, 0 warnings and 0
  errors each. Changed C++ files are clang-format 15 clean.

## What d3dcompiler_47 builds

The plan assumed HLSL for shader models 2 and 3 and something else for ps_1_x. Tried from Python
with ctypes on `d3dcompiler_47.dll` before writing anything:

- `D3DCompile` compiles vs_1_1, vs_2_0, vs_2_a, vs_3_0, ps_2_0, ps_2_a, ps_2_b and ps_3_0. ps_1_1
  and ps_1_4 fail with X3539 ("ps_1_x is no longer supported").
- **`D3DAssemble` is exported** (it isn't in `d3dcompiler.h`) and assembles every model, ps_1_1
  and ps_1_4 included. Its signature is Wine's. It validates as it assembles: a ps_1_4 shader
  reading an `r0` component lost at the phase marker fails with X5119, and a vs_1_1 instruction
  reading two constants with X5346.
- fxc's own disassembly (`D3DDisassemble`) of vs_2_0, vs_3_0, ps_2_0, ps_2_b and ps_3_0 shaders,
  constant table comments included, assembles back to the same instruction tokens. ps_2_b
  disassembles as `ps_2_x` and comes back as version 2.1, as compiled.

So editing the disassembly works for every model, and an HLSL stub needs ps_2_0 for a ps_1_x
shader. RenderDoc's disassembly is in fxc's format; the test checks it assembles back the same.

## The test: `D3D9_Shader_Editing`

The demo (`util/test/demos/d3d9/d3d9_shader_editing.cpp`) draws nine quads into a 288x32 render
target, one per 32x32 cell, covering every model and every way replay binds a shader:

| Draw | Shaders | Bound |
|---|---|---|
| Frame Start | vs_3_0 + ps_3_0 (VFACE) | at the end of the previous frame, so in `BeginCaptureFrame` |
| HLSL 3_0 | the same vs_3_0 + a ps_3_0 that "Shared" uses too | `Set*Shader` |
| HLSL 2_0 | vs_2_0 + ps_2_0 | `Set*Shader` |
| SM1 | vs_1_1 + ps_1_1 (co-issue, `_x2`), assembled | `Set*Shader` |
| ps_1_4 | the same vs_1_1 + ps_1_4 (phase, dependent read), assembled | `Set*Shader` |
| State Block | vs_2_0 + another ps_2_0 | a state block recorded before the frame |
| Shared | vs_3_0 + the ps_3_0 of "HLSL 3_0" | `Set*Shader` |
| FFP Pixel | vs_2_0, fixed function pixel processing | `Set*Shader` |
| In Frame | vs_2_0 + a ps_2_0 created before the draw and released at the end of the frame | `Set*Shader` |

The shaders read the application's constants (float, and a bool in vs_3_0) and a texture.
Natively, before the first frame, the demo draws the scene once as it is and once with each of 10
edits in place of the original, and writes what the GPU drew at 4 texels of each cell. The edits
are HLSL (with a profile), assembly or bytecode, of each model and binding. Which cells each edit
changed natively, worked out from the reference:

```
Frame start ps_3_0, HLSL         changes cells [0]
Shared ps_3_0, assembly          changes cells [1, 6]
ps_2_0, bytecode                 changes cells [2]
ps_1_1, assembly                 changes cells [3]
ps_1_4 as ps_2_0, HLSL           changes cells [4]
vs_1_1, assembly                 changes cells [3, 4]
vs_3_0, HLSL                     changes cells [0, 1, 6]
State block ps_2_0, HLSL         changes cells [5]
vs_2_0 with FFP pixel, HLSL      changes cells [2, 5, 7]
```

(That run was before the "In Frame" draw and its edit were added; the test's run below has them.)

The test (`util/test/tests/D3D9/D3D9_Shader_Editing.py`) checks that:
- unedited, the 36 texels match the native drawing;
- every shader's disassembly assembles back to the same instructions (comment blocks aside);
- with each edit built and replacing the original, all 36 texels match what the GPU drew natively
  with the edit, so the draws that share the shader change and the others don't;
- the pipeline state then has the original's ID and the edit's reflection;
- for the vs_1_1 and vs_3_0 edits, which move the quad right by a quarter of its width, VS Out's
  positions move by that;
- removing the replacement brings the native drawing back;
- an HLSL error, an assembly error, HLSL for ps_1_1, a vertex profile for a pixel shader, vertex
  shader bytecode for a pixel shader, bytecode with no end token and a geometry shader don't
  build, with the compiler's or RenderDoc's message;
- HLSL with no profile is ps_3_0;
- a pixel shader doesn't replace a vertex shader;
- freeing a replacement shader removes the replacement.

The texels come from the GPU in both cases: natively from `GetRenderTargetData`, on replay from
RenderDoc's pick pixel of the replayed render target.

The x64 run (`d3d9_step4/editing_run_x64.txt`):

```
** The native run's log has no errors (0 warnings)
** The demo's log has no errors (0 warnings)
** The capture was made by a 64-bit process (Windows x86 64-bit)
** Shaders can be built from ['D3D9Bytecode', 'HLSL', 'D3D9Asm']
** Unedited, the 36 texels match the GPU's native rendering
** Frame Start's vs (vs_3_0): its disassembly assembles back to the same instructions
** Frame Start's ps (ps_3_0): its disassembly assembles back to the same instructions
** HLSL 3_0's ps (ps_3_0): its disassembly assembles back to the same instructions
** HLSL 2_0's vs (vs_2_0): its disassembly assembles back to the same instructions
** HLSL 2_0's ps (ps_2_0): its disassembly assembles back to the same instructions
** SM1's vs (vs_1_1): its disassembly assembles back to the same instructions
** SM1's ps (ps_1_1): its disassembly assembles back to the same instructions
** ps_1_4's ps (ps_1_4): its disassembly assembles back to the same instructions
** State Block's ps (ps_2_0): its disassembly assembles back to the same instructions
** In Frame's ps (ps_2_0): its disassembly assembles back to the same instructions
** Frame start ps_3_0, HLSL: the 36 texels match the GPU's native rendering with the edit
** Shared ps_3_0, assembly: the 36 texels match the GPU's native rendering with the edit
** ps_2_0, bytecode: the 36 texels match the GPU's native rendering with the edit
** ps_1_1, assembly: the 36 texels match the GPU's native rendering with the edit
** ps_1_4 as ps_2_0, HLSL: the 36 texels match the GPU's native rendering with the edit
** vs_1_1, assembly: the 36 texels match the GPU's native rendering with the edit
** vs_1_1, assembly: VS Out positions are the edited shader's
** vs_3_0, HLSL: the 36 texels match the GPU's native rendering with the edit
** vs_3_0, HLSL: VS Out positions are the edited shader's
** State block ps_2_0, HLSL: the 36 texels match the GPU's native rendering with the edit
** vs_2_0 with FFP pixel, HLSL: the 36 texels match the GPU's native rendering with the edit
** In-frame ps_2_0, HLSL: the 36 texels match the GPU's native rendering with the edit
** Removing each replacement restores the original rendering
** an HLSL error doesn't build: error X3004: undeclared identifier 'undefined'
** an assembly error doesn't build: error X2005: invalid register 'r99'
** HLSL for ps_1_1 doesn't build: error X3539: ps_1_x is no longer supported; use /Gec in fxc to automatically upgrade to ps_2_0
** a vertex profile for a pixel shader doesn't build: vs_3_0 isn't a pixel shader profile
** vertex shader bytecode for a pixel shader doesn't build: It's a vs_2_0 shader, not a pixel shader
** bytecode with no end token doesn't build: The bytecode doesn't parse: The shader has no end token
** a geometry shader doesn't build: D3D9 only has vertex and pixel shaders
** HLSL with no profile is compiled as ps_3_0
** A pixel shader doesn't replace a vertex shader
** Freeing a replacement shader removes the replacement
```

All the checks pass. The whole set of D3D9 tests (16, with `D3D11_D3DPERF_Markers`) passes on x64
and on the 32-bit demos (`--x86`), with this test's x86 run in `d3d9_step4/editing_run_x86.txt`.
The unit tests (`renderdoccmd test unit "[d3d9]"`) pass: 6057 assertions in 13 test cases.

## A crash when freeing a replacement

The first full runs crashed (access violation, about two runs in three) at the test's last check,
which replaces a shader, frees the replacement with `FreeTargetResource` and replays. The crash
was in NVIDIA's driver (`nvd3dumx`), in the first draw of the next replay (`WriteDepthStencil`
restoring the depth stencil's initial contents), reading a pointer from freed memory
(`0x100000003`). Repeating the steps from a script (replace the "HLSL 2_0" pixel shader with a
build of the bytecode edit, free it, replay to an event, 5 to 30 times) found:

- No crash without the free (40 replays, or replacing and building again each time).
- No crash when the replacement was removed and replayed first, then freed (30 times at event 148).
- Freeing straight after the replay that drew with it crashed on the first round when the replay
  stopped at event 85, 116, 147 or 148, and didn't at 100, 132, 133, 136, 140, 144 or 146. The
  crashing ones stop with ps_1_4, ps_3_0 or the in-frame ps_2_0 bound; the others with the state
  block's ps_2_0 or no pixel shader. Why that matters inside the driver wasn't found.

So `FreeTargetResource` now removes the replacement at once and releases the shader after the next
replay, which is the order that didn't crash. With that, the cases above (10 rounds at 85, 116, 147
and 148) and five runs of the test in a row passed. qrenderdoc only frees edited shaders when the
capture closes, so the UI didn't hit this.

## The disassembly round trip over the other demos' shaders

The test's shaders don't use loops, relative addressing or `texldl`. `D3D9_Mesh_Output`'s do (none
of the demos uses predicates), so a one-off script (not committed) built
every shader drawn in each of them from its disassembly as D3D9 assembly and compared the
instructions (one line per test; some tests make more than one capture, with the same result,
all in `d3d9_step4/roundtrip_other_demos.txt`):

```
D3D9Ex_Wrapping_Check: 0 shaders, 0 assemble back to the same instructions
D3D9_Capture_Edge_Cases: 0 shaders, 0 assemble back to the same instructions
D3D9_Counters: 3 shaders, 3 assemble back to the same instructions
D3D9_Custom_Shaders: 2 shaders, 2 assemble back to the same instructions
D3D9_Failed_Calls: 2 shaders, 2 assemble back to the same instructions
D3D9_Frame_Start_State: 2 shaders, 2 assemble back to the same instructions
D3D9_Mesh_Output: 5 shaders, 5 assemble back to the same instructions
D3D9_Overlay_Test: 2 shaders, 2 assemble back to the same instructions
D3D9_Reset_Capture: 2 shaders, 2 assemble back to the same instructions
D3D9_Resource_Contents: 2 shaders, 2 assemble back to the same instructions
D3D9_Resource_Usage: 2 shaders, 2 assemble back to the same instructions
D3D9_Shader_Editing: 10 shaders, 10 assemble back to the same instructions
D3D9_Simple_Triangle: 3 shaders, 3 assemble back to the same instructions
D3D9_Texture_Formats: 1 shaders, 1 assemble back to the same instructions
D3D9_Wrapping_Check: 0 shaders, 0 assemble back to the same instructions
total 62/62 (counting a shader again in each capture it's in); by model: ps_1_1 1/1, ps_1_4 1/1, ps_2_0 4/4, ps_3_0 28/28, vs_1_1 2/2, vs_2_0 3/3, vs_3_0 23/23
```

## In the UI

On a 64-bit `D3D9_Shader_Editing` capture in qrenderdoc:

- The pixel shader page of the SM1 draw has the new Edit button between View and Save
  (`d3d9_step4/edit_1_button.png`). Clicking it opens the ps_1_1 shader's disassembly, with Source
  Type "D3D9 Assembly" and the Builtin compiler (`edit_2_disassembly.png`).
- Replacing `mul_x2 r0.xyz, t0, v0` with `mov r0.xyz, c0` and applying it (F5): "Edited Shader
  Active", the API Inspector shows `SetPixelShader(Pixel Shader 26 (Edited))`, and the pipeline
  state still has Pixel Shader 26 with the edit's reflection (`edit_3_applied.png`). The SM1 cell
  of the scene is the flat tint, 0.60, 0.65, 0.54, and the other cells are unchanged
  (`edit_4_scene.png`).
- The Edit menu offers Edit Disassembly and Edit Generated Stub (`edit_5_menu.png`). The vs_3_0
  shader's stub (`edit_6_stub_vs3.png`) has `rect`, `misc` and `flip` at c1, c2 and b0, `c0` (a
  literal fxc defined), the POSITION, TEXCOORD and COLOR inputs and outputs, and `/T vs_3_0` in
  the compile options. The ps_1_4 shader's stub is ps_2_0, with the two samplers it reads, s0 and
  s2 (`edit_7_stub_ps14.png`).
- Each stub compiled unchanged and replaced its shader, checked with the pipeline state's
  reflection ID before and after: vs_3_0, ps_3_0 (with VFACE), vs_2_0, ps_2_0, vs_1_1, ps_1_4,
  and the state block's and the in-frame ps_2_0.

## Not covered

- Shaders from real games, which may have no constant table or come from other compilers.
- Structs and matrices in the HLSL stub: none of the demos' shaders that were stubbed have them.
- Software vertex processing shaders (vs_2_sw, vs_3_sw).
- Two editors open on the same shader at once wasn't tried.
