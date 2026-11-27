# D3D9: shader debugging documented, and checked from Python and the UI (step 5.11)

Step 5.11 of the shader debugging plan (STATUS.md, "Step 5 plan"): a D3D9 section in
`docs/how/how_debug_shader.rst`, the README and STATUS.md brought up to date, the python bindings
checked, a whole vertex and pixel session in the UI, and a full test run. Nothing in the driver
changed: what's tested is f2b80374c (5.10) with the docs added. Step 5.9 (ps_1_x) is still put off.

Tested on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124), on the zoo capture
(`D3D9_Shader_Debug_Zoo`, frame 5) the 5.10 test run made. Files are in
`d3d9_step5_11_finishing/`.

## The docs

`docs/how/how_debug_shader.rst`:

- The warning at the top lists D3D9 among the APIs that can debug shaders.
- "Including debug info in shaders" says D3D9 shaders are debugged in their assembly: what `/Zi` or
  `D3DXSHADER_DEBUG` adds to SM2/SM3 bytecode isn't read (5.12 and 5.13 would), but the constant
  table is, so it should be kept.
- A new "Debugging D3D9 shaders" section: what can be debugged (vs_1_1 to vs_3_0 from the mesh
  viewer, ps_2_0 to ps_3_0 from the texture viewer; not ps_1_x yet, nor fixed function, which isn't
  a shader; the disabled options say why), the names (the constant table, Literals, Registers,
  semantics), the call stack, Run to Sample and Run to NaN/Inf, how a pixel is read (the event
  drawn again, the shader on the CPU, lookups and transcendentals on the GPU, and the fused
  `mul`/`add` caveat), and which fragment is debugged (the one the GPU wrote; D3D9 has no pixel
  history; every fragment rejected; multisampled targets; `DebugPixelInputs` ignored).

Sphinx isn't installed on this machine, so the page wasn't built. It uses only roles and
substitutions the page and the other pages already use (`:ref:` to a label it adds, `:class:`,
and `|runsample|` and `|runnaninf|`, defined further down the same page).

## The python bindings (RenderDoc)

`debug_api.py` runs a whole session on the capture through the bindings alone, with no test
framework, and checks each part the shader viewer uses. It ran in python 3.14.3 with the module
`build_pymodule_py3.ps1` builds (`debug_api_py314.txt`), and in qrenderdoc's embedded python 3.6.4
through a `--python` wrapper that sets `sys.argv` (`debug_api_py36.txt`). The two outputs are the
same line for line, apart from the first (the python version). Both end:

```
  ok   DebugThread gives a trace without a debugger
  ok   DebugMeshThread gives a trace without a debugger
0 checks failed
```

What it checks:

- `GetAPIProperties()`: `shaderDebugging` True, `pixelHistory` False.
- **A vertex** (the "vs_2_x calls" draw, vertex 0): its reflection is debuggable (encoding
  `D3D9Bytecode`). `DebugVertex` gives a `ShaderDebugTrace` with a `ShaderDebugger`, stage Vertex,
  inputs `v0`, `v1`, constant blocks `c`, `i` and `b`, 17 source variables and 23 `instInfo`
  entries. `ContinueDebug` gives 31 `ShaderDebugState`s with `stepIndex` 0 to 30 in order, each
  `nextInstruction` with an `instInfo` entry, 11 `ShaderVariableChange`s after the first state
  (before and after the same register), and call stacks that start with "main" and reach
  `['main', 'l1', 'l3']`. It then gives nothing, and nothing again. The final `oPos`,
  (-0.45, -0.8, 0, 1), is VS Out's first position, read with `GetPostVSData` and `GetBufferData`.
- **Pixels**, against the GPU's values in the native run's `D3D9_Shader_Debug_Zoo_pixels.txt`:
  - "ps_3_0" at (240, 60): inputs `v0`-`v3`, `vPos` (2 components), `vFace` (1); 40 states; oC0
    (0.14928, 0.01263, -0.55829, 1.68225), the GPU's.
  - "ps_3_0 anisotropic" at (40, 60): `s0` in `readOnlyResources` and `samplers`; the one step
    flagged `SampleLoadGather` is the lookup's; oC0 the GPU's.
  - "ps_3_0 multisampled" at (40, 60): with the defaults, and with `sample` 0 to 3, `primitive` 0
    and `view` 0, oC0 is (-0.25, 0.27458, 0.37966, 0.50610) every time, the GPU's: D3D9 ignores
    them and debugs the fragment covering the pixel centre (5.10).
- **Not debuggable:** the ps_1_1 draw's reflection says "Debugging ps_1_x pixel shaders isn't
  supported yet", and `DebugPixel` there gives a trace without a debugger. `ContinueDebug(None)`
  gives nothing. `DebugThread` and `DebugMeshThread` (D3D9 has neither) give a trace without a
  debugger, which `FreeTrace` frees.

Found along the way, neither a problem:

- The docs for `DebugPixel` refer to `NoPreference`. In python it's
  `renderdoc.ReplayController.NoPreference`; there's no `renderdoc.NoPreference`. Upstream's
  D3D11 tests use the former.
- `debugInfo.encoding` is `Unknown` for D3D9 shaders. That's the encoding of the embedded source
  files, and D3D9 shaders have none (`files` is empty); the reflection's own `encoding` is
  `D3D9Bytecode`.
- Python 3.8 and later don't load DLLs from `PATH`, so the script adds the build's folder with
  `os.add_dll_directory` (`RENDERDOC_DLL_DIR`). qrenderdoc leaves after a `--python` script only if
  it raises `SystemExit`, so the wrapper ends with one.

## The UI (RenderDoc)

qrenderdoc on the capture, driven from inside by `ui_driver.py` (a `--ui-python` script that runs
each command on the UI thread through `InvokeOntoUIThread`; no mouse or keyboard input).
qrenderdoc has no PySide2, so the script can't click the Debug buttons or the "Debug this Vertex"
menu item. It calls `DebugVertex` or `DebugPixel` and opens the result with `DebugShader`, which is
what the buttons' handlers do after working out the vertex or pixel. The handlers' own code isn't
covered by this.

Screenshots are of qrenderdoc's window alone (PrintWindow). The texture viewer and mesh preview are
native D3D surfaces, which PrintWindow doesn't capture: they show blank or whatever was drawn there
before (in `01`, the python scripting panel's text; in `09`, the earlier debugger).

The vertex session, "vs_2_x calls", vertex 0:

- `01_mesh_viewer.png`: the mesh viewer at the draw. VS Out's vertex 0 is at (-0.45, -0.80, 0.00,
  1.00) with COLOR 0.375.
- `02_vertex_start.png`: the debugger at the first instruction (`m4x4 oPos, v0, c0`). The
  constants list the inputs by semantic (POSITION, COLOR), `c0`-`c3`, Literals, `b0` (true) and
  Registers; the call stack is `main`.
- `03_vertex_breakpoint.png`: a breakpoint on `add r0.x, r0.x, v1.x` in `l3`, and Run: it stops
  there at step 6, with the call stack `l3`, `l1`, `main`, and watches r0.x 0.25 and v1.x 0.
- `04_vertex_end.png`: stepped to the end. The watches show oPos (-0.45, -0.80, 0.00, 1.00) and
  oD0 (0.375, 0.375, 0.375, 0.375), VS Out's, and r0.x 0.375.

The pixel session:

- `05_texture_viewer.png`: the "ps_3_0 anisotropic" draw's target, pixel (40, 60) picked: (0.47852,
  0.76242, 0.44602, 1.00) in the status bar. The Debug button is enabled.
- `06_pixel_lookup.png`: that pixel debugged, one step on, past `texld r0, v0, s0`. The watch shows
  r0 (0.47852, 0.76242, 0.44602, 1.00), the picked value, made on the GPU; TEXCOORD is (0.38368,
  0.46608), and `tex` is listed as a resource on `s0`.
- `07_pixel_loop.png`: the "ps_3_0" draw's pixel (240, 60), a breakpoint on the loop's `mul r3,
  r2, c0`, and Run twice: the second pass, step 7. `k0` is named from the constant table, and the
  watches show r2, vFace 1.00 and vPos (240, 60).
- `08_pixel_end.png`: stepped to the end. oC0 (0.14928, 0.01263, -0.55829, 1.68225) is the GPU's;
  oC1 (0.01255, 0.01511, 240.00, 60.00) is the derivatives and vPos; oDepth 0.43983.
- `09_ps11_not_debuggable.png` and `09b_ps11_debug_button.png`: the ps_1_1 draw's pixel (40, 210)
  in the texture viewer. The Debug button is disabled (its reflection says "Debugging ps_1_x pixel
  shaders isn't supported yet", which the button's tooltip shows).

## Builds and tests

`verify_summary.txt`:

```
renderdoc.sln x64 exit 0 warnings 0 errors 0
demos.sln x64 exit 0 warnings 0 errors 0
renderdoc.sln x86 exit 0 warnings 0 errors 0
demos.sln x86 exit 0 warnings 0 errors 0
unit x64 exit 0 All tests passed (20184 assertions in 26 test cases)
unit x86 exit 0 All tests passed (20184 assertions in 26 test cases)
  RunPostVS: 200000 vertices of a 16 instruction shader in 977.1 ms (best of 5)
  RunPostVS: 200000 vertices of a 16 instruction shader in 983.9 ms (best of 5)
  RunPostVS: 200000 vertices of a 16 instruction shader in 988.0 ms (best of 5)
pymodule exit 0
tests x64 exit 0
## Tests complete summary: 19 passed out of 19 run from 210 total in 0:01:36.947965 ##
tests x86 exit 0
## Tests complete summary: 19 passed out of 19 run from 210 total in 0:01:38.143637 ##
```

- Both solutions build with 0 warnings for x64 and x86. The only changes are docs, so nothing
  needed formatting.
- The unit tests pass on x64 and x86, and the automated tests 19 of 19 on x64 and on the 32-bit
  demos (`tests_x64.txt`, `tests_x86.txt`), as they did in 5.10.
- The Development `RunPostVS` bench is 977-988 ms (979-980 in 5.10).

## Not covered

- The Debug button's and "Debug this Vertex"'s own handlers (see "The UI").
- The tooltips, which need the mouse over them; their text is the reflection's debug status, which
  the bindings check reads.
- The docs page wasn't built with Sphinx.
- Everything STATUS.md lists under "Debugging pixel shaders" as not covered still isn't: ps_1_x
  (5.9), `texkill` and the alpha test on multisampled draws, partly covered multisampled pixels, a
  shader writing all four targets, depth stencils StretchRect can't copy, other GPUs.
