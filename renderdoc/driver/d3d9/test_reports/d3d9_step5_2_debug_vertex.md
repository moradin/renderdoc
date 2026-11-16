# D3D9: debugging vertex shaders

Step 5.2 of the shader debugging plan (STATUS.md, "Step 5 plan"). Tested on the working tree on
top of 062ab7451, committed as "D3D9: debug vertex shaders". Machine: Windows 11, RTX 4070 Ti.
Files are in `d3d9_step5_2_debug_vertex/`.

## Why

5.1 let the interpreter run a vertex one instruction at a time. This puts it in front of the
user: "Debug this Vertex" in the mesh viewer, `DebugVertex` and `ContinueDebug` in Python, with
registers named by register. Before it, D3D9 said it couldn't debug shaders at all.

## The change

- **`DebugVertex`** (`d3d9_shaderdebug.cpp`) takes the vertex from VS Out's snapshot of the draw
  (`InitPostVSBuffers`): the same streams, constants and vertex textures. Its inputs are fetched
  as `RunPostVS` fetches them, through two functions split out of it (`BindVSInputs`,
  `FetchVSInputs`), and it runs in the same interpreter. So a debugged vertex ends with VS Out's
  values, which `D3D9_Mesh_Output` checks against the GPU.
  - The vertex is the mesh viewer's IDX column, as on D3D11: an indexed draw's index plus the
    base vertex, or a non-indexed draw's vertex from its first.
  - The snapshot moved from `d3d9_postvs.cpp` into `d3d9_postvs.h` so both can use it, and keeps
    the vertex shader's ID. The debugger holds a reference to it.
- **The trace** is made by `D3D9Emu::VSDebugger` (`d3d9_vs_debug.cpp`), which is portable like the
  interpreter, so it's unit tested.
  - Inputs `v#`, constants by register (`c12`, `i0`, `b3`), and the registers the shader can
    change: the temporaries, `a0` (4 integers), `aL` (1 integer), `p0` (4 booleans) and the
    outputs (`oPos`, `oFog` and `oPts` with 1 column, `oD#`, `oT#`, or vs_3_0's `o#`).
  - A source variable per input and output signature element, named by its semantic. That's how
    the viewer labels them, and how the tests' `check_vertex_debug` finds the outputs.
  - One state per instruction, each with the registers it changed, before and after. Declarations
    don't get a state of their own, so stepping never stops on a `dcl` or `def`.
  - Each instruction's disassembly line (5.1's line map), so the viewer highlights the right line.
- **What can be debugged.** `shaderDebugging` is on. A vertex shader's reflection says it can be
  debugged if the interpreter accepts it. Pixel shaders say "Debugging D3D9 pixel shaders isn't
  supported yet", and fixed function vertex processing says it isn't a shader; the UI shows these
  as the reason the Debug buttons are off.
- **A fix found by the new test:** the pipeline state named 2_x shaders `vs_2_1`, while the
  disassembly says `vs_2_x`. It says `vs_2_x` now.
- **A fix found in the UI screenshots:** `m4x4 oPos, v0, c0` reads c0-c3, but for a shader
  without a constant table the reflection listed only `c0`, so the debugger (and the constant
  viewer) didn't show c1-c3. The reflection now counts a register per row of the matrix
  instructions' second source. A unit test covers `m4x4` and `m3x2`, and the zoo test checks the
  traces list c0-c3.
- **Test helpers:** the unit tests' shader `Assembler` moved into `d3d9_bytecode_test.h` so the
  debugger's tests can use it. `util/test/d3d9_shader_unit_tests.sh` builds `d3d9_vs_debug.cpp`
  too.

## The new demo and test

`D3D9_Shader_Debug_Zoo` draws a triangle with a vertex shader of each model, each in its own
marker:

| Draw | Shader | What it exercises |
|---|---|---|
| vs_1_1 | hand assembled (D3DAssemble doesn't take vs_1_1) | `m4x4`, `mov a0.x` and `c[a0.x + 4]`, `mul`, `mad`, `dp3`, `rsq`, `rcp`, `expp`, `logp`, `lit`, `dst`, `frc`, `min`, `max`, `slt`, `sge`, a negated source, `oD0`, `oD1`, `oT0`-`oT3`, `oFog`, `oPts` |
| vs_2_0 | HLSL | a `loop` by `i0` over a relatively addressed array, a branch on `b0`, `sincos`, `pow`, `normalize`, `frac` |
| vs_2_x | assembly | `setp_gt`, predicated writes (`(p0)` and `(!p0.x)`), `loop` with `break_gt`, `if_lt`/`else`, `call`/`label`/`ret` |
| vs_3_0 | HLSL | a dynamic loop over `i0` with a `break`, relative addressing on `aL`, `texldl` from an A32B32G32R32F vertex texture, `cross`, `length` |
| vs_3_0 instanced | HLSL | `DrawIndexedPrimitive` with base vertex 5 and start index 3, 2 instances from a second stream |
| Fixed function | none | can't be debugged |

The test (`tests/D3D9/D3D9_Shader_Debug_Zoo.py`):
- debugs every vertex of every instance of the five shader draws (18 vertices) with upstream's
  `check_vertex_debug`: each output's value, read through the trace's source variables, has to
  equal VS Out's, and `process_trace` checks that every step's changes apply forwards and
  backwards (`validate_trace`);
- checks that each state's instruction is on a line of the disassembly the viewer shows, and not
  a declaration or comment;
- checks the registers' names, types and sizes in the vs_1_1, vs_2_x and vs_3_0 traces, and that
  their constants include c0-c3, which the position's `m4x4` (or `dp4`s) read;
- checks the API says it can debug shaders, the pixel shader and the fixed function draw say why
  they can't, and `DebugVertex` on the fixed function draw gives no debugger;
- checks each draw's shader model in the pipeline state, which found the `vs_2_1` name.

Writing the vs_1_1 shader: the runtime rejected `sub oT3, r0, r3` in it, and accepted
`add oT3, r0, -r3`. The demo uses the second and, if the shader is ever rejected, logs the first
instruction the runtime refuses, as `D3D9_Mesh_Output` does.

## Results

From the final run, with both fixes in (`run/`, `verify_summary.txt`):

| Check | Result |
|---|---|
| `renderdoc.sln` and `demos.sln`, Development x64 and x86 | 0 warnings, 0 errors (`build_*.txt`) |
| `renderdoccmd test unit "[d3d9]"`, x64 and x86 | "All tests passed (13694 assertions in 17 test cases)" (`unit_x64.txt`, `unit_x86.txt`). After 5.1: 13187 in 16 |
| `run_tests_local.py -t "^D3D9\|D3D11_D3DPERF"`, x64 | "19 passed out of 19 run" (`tests_x64.txt`): the 18 from before and `D3D9_Shader_Debug_Zoo` |
| `[d3d9bench]`, Development x64 | `RunPostVS` on 200,000 vertices: 1011.3, 1009.7, 1012.2 ms (`bench_development_x64.txt`). 5.1's final run: 1021.2-1026.9 ms. Splitting out the input fetch didn't slow mesh output |

The new unit test case, "D3D9 vertex shader debugging" (`[vsdebug]`), builds traces of
hand-assembled shaders and checks:
- a vs_3_0 shader with declarations, `mova`, `setp_gt`, `c0[a0.x]`, a `loop` adding `c0[aL]`, a
  predicated write and two outputs packed into `o1`: the states' instructions (7, 8, 9, 10, 11, 12,
  13, 12, 13, 12, 13, 14, 15, 16, 17, 17, so no declaration), the first state's registers in order
  (r0, r1, r2, a0, aL, p0, o0, o1) with nothing before, their types and sizes and final values,
  `aL` going 0 to 1 at the first `endloop`, the inputs and their source variables, the packed
  outputs' components, the constants by register (c0-c10 from the relative reads and the `def`,
  `i0`) and their source variables, and each instruction's disassembly line;
- a vs_1_1 shader writing `oPos`, `oFog`, `oPts`, `oD0` and `oT3`, and `a0` with `mov` (rounding
  down): `oFog` and `oPts` have one column, and the FOG source variable is `oFog.x`;
- a `def` between two instructions isn't a step;
- calls nested too deeply: the trace ends on the failing instruction with the error, after 18
  states;
- for each: going through all the states, each change's value before is what the variable had,
  going back from the end undoes everything, and the outputs equal `Execute`'s bit for bit. The
  states are fetched 1, 3, 4 or 100 at a time.

`D3D9_Shader_Debug_Zoo` (`tests_x64.txt`), per draw:

| Draw | Vertices debugged | Steps per vertex |
|---|---|---|
| vs_1_1 | 3 | 27 |
| vs_2_0 | 3 | 92 |
| vs_2_x | 3 | 22, 20, 20 (vertex 0 takes the `call`) |
| vs_3_0 | 3 | 65, 38, 65 (vertex 1 breaks out of the loop early) |
| vs_3_0 instanced | 6 (vertices 6, 5, 7 of 2 instances) | 11 |

Each "Successfully debugged vertex": every output equal to VS Out's (`check_vertex_debug` allows
5e-6; the values come from the same interpreter so they're identical). The first steps of each are
on real instructions, e.g. vs_1_1's `m4x4 oPos, v0, c0`, `mov r3, c20`, `mov a0.x, r3.y`, and
vs_3_0's `mul r0, c4.xxyy, v2.xyxx`, `texldl r0, r0, s0`. The pixel shader says "Debugging D3D9
pixel shaders isn't supported yet" and the fixed function draw "Fixed function vertex processing
isn't a shader".

Before the two fixes the first run failed on "The vs_2_x draw's vertex shader is vs_2_1, expected
vs_2_x"; the run before the matrix fix passed 19 of 19 too.

## The UI

Screenshots of qrenderdoc on a capture of the zoo (`screenshots/`), at the vs_2_x draw:

1. `01_vs_out.png`: the mesh viewer's VS In and VS Out.
2. `02_debug_this_vertex.png`: right-clicking vertex 1 in VS Out. "Debug this Vertex" is enabled.
3. `03_first_step.png`: the debugger opened by that menu. It starts on line 7, `m4x4 oPos, v0,
   c0`, after the declarations. The inputs are listed by semantic (POSITION, COLOR, TEXCOORD on
   `v0`-`v2`), the constants by register (`c0`-`c3`, `c10`-`c19`), and the registers r0, r1, aL,
   p0, oPos, oD0 and oT0. `03a_first_step_before_matrix_fix.png` is the same before the matrix
   fix: only `c0` of the four registers `m4x4` reads. Screenshots 4 and 5 were taken before the
   fix too, so they list `c0` and then `c10`.
4. `04_in_the_loop.png`: after 8 steps (F10), on `break_gt` in the loop's second pass: aL is 1,
   r0 has just changed (red), p0 is (false, true, false, true) from `setp_gt p0, v2.xyxy, c20.x`
   with v2 = (0.3, 1.0), and oPos is VS Out's (0.66, 0.80, 0, 1).
5. `05_finished.png`: run to the end (F5). The finished marker is on the main program's `ret`,
   and oD0 is (2.25, -1.00, 1.05, 1.00), VS Out's COLOR for vertex 1.

The screenshots were taken with `PrintWindow` on qrenderdoc's window, apart from the menu (a
popup, which `PrintWindow` doesn't capture), which is the screen area of the maximised window.
Each was cropped to the window and checked to show nothing else.

## Not covered

- Pixel shaders, and names from the constant table, call stacks and event flags: steps 5.3 on.
- The debugged values are compared with VS Out, which comes from the same interpreter. VS Out
  itself is compared with the GPU by `D3D9_Mesh_Output`; this test doesn't compare with the GPU
  again.
- An instruction the interpreter fails on ends the trace there, with the reason only in the log.
  The unit tests check that; no demo shader fails.
- The unit tests ran on Windows only, not on Linux with AddressSanitizer.
- The x86 automated tests weren't run, as this is replay-side code (the x86 builds and unit tests
  were).
