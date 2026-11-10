# D3D9 support status

D3D9 support is being built in phases on this fork. Upstream RenderDoc removed its D3D9 stub in 2023,
so none of this is upstream.

The Windows test reports this file refers to (`d3d9_*.md`, with their screenshots and logs) are in
[`test_reports/`](test_reports).

| Phase | What | State |
|---|---|---|
| 0 | Driver project restored on current upstream, overlay working | Done |
| 1 | Every object wrapped, bindings tracked, state blocks, Reset | Done, Reset needs more testing |
| 2a | Captures written: every call serialised, resource creation records, structured export | Done, tested with `D3D9_Simple_Triangle` |
| 2b | Full device state at frame start (render/sampler/texture stage states, transforms, lights, constants...) | Done, tested with `D3D9_Frame_Start_State` |
| 2c | Resource contents: Lock tracking, shadow copies of D3DPOOL_DEFAULT data, initial contents | Done, tested with `D3D9_Resource_Contents` |
| 2d | Edge cases: Reset/device loss mid-capture, state block contents, D3DPERF markers in captures, depth stencil read back, GDI drawing, front buffer copies, locks spanning the capture start | Done, tested with `D3D9_Capture_Edge_Cases` and `D3D9_Reset_Capture`; device loss needs a real game |
| 3a | Replay: loading captures, replaying to any event, the event list, resource descriptions, reading texture and buffer data | Tested from Python and the UI on Windows (`d3d9_phase3_report.md`, `d3d9_phase3_retest*.md`) |
| 3b | Texture viewer and buffers: displaying every D3D9 format, pick pixel, min/max, histogram, mesh viewer inputs | Tested from Python and the UI on Windows (same reports) |
| 3c | D3D9 pipeline state, with its own UI panel and Python bindings | Tested from Python and the UI panel on Windows (same reports) |
| 3d | Shaders: SM1-3 disassembly, reflection from the constant table | Tested on Windows against fxc's disassembly, and in the UI's shader viewer (same reports) |
| 3e | Texture viewer overlays | Tested from Python and the UI on Windows (`d3d9_phase3e_test.md`, `d3d9_phase3e_retest.md`), triangle size included. Quad overdraw isn't supported (no UAVs in D3D9) and shows no overlay |
| 3e | Mesh output (post-transform vertices, VS Out) | Tested from Python and the UI on Windows (`d3d9_phase3e_retest2.md`): all 18 checks pass, including vs_1_1, flow control, loops, FFP lighting, texgen and fog |

## Remaining work

Open items from the last test round:

- Locks of part of an ATI1/ATI2 level aren't tested (see the texture viewer's known gaps).

The plan, in order. The estimates are working days at the pace so far, from when the coding and
the testing were on separate machines.

| # | Work | Estimate | Notes |
|---|---|---|---|
| 1 | ~~**Automated tests:** turn the `util/test/d3d9_*_check.py` scripts into tests in upstream's framework (`util/test/rdtest`, `util/test/tests/`), so that `run_tests.py` runs the demos, captures them and checks them~~ | 1 | Done, see "Automated tests" below. Run them after every change |
| 2 | ~~**32-bit programs:** an x86 build that captures 32-bit programs, replayed in the x64 UI, then fixing what breaks~~ | 1-1.5 | Done, see "32-bit programs" below. Nothing in the driver needed fixing. A real 32-bit game is still to test |
| 2b | **Memory at capture:** cap the copies of textures uploaded to D3DPOOL_DEFAULT, spill them to disk, or keep them only while a capture is pending | 0.5-1 | Matters mostly for 32-bit games |
| 3 | **Small features** | 2-3 | |
| | ~~GPU duration and samples passed counters (timestamp and occlusion queries)~~ | 0.5 | Done, see "GPU counters" below |
| | ~~Resource usage list (what each event binds, recorded during replay)~~ | 0.5 | Done, see "Resource usage" below |
| | ~~Debug messages: the D3D9 debug runtime doesn't run on Windows 8 and later, so report failed calls (`D3DERR_INVALIDCALL` and similar) instead~~ | 0.5 | Done, see "Debug messages" below |
| | ~~Custom display shaders in the texture viewer~~ | 0.5-1 | Done, see "Custom display shaders" below |
| 4 | **Shader editing and replacement**, including Edit Generated Stub | 1.5-2 | HLSL through d3dcompiler_47 for SM2/3. Assembly for SM1, since current compilers can't target ps_1_x. The stub comes from the constant table |
| 5 | **Shader debugging** | 5-8 | The biggest item |
| | Vertex shaders | 1.5-2 | Builds on the interpreter in `d3d9_vs_emulate.cpp`, adding per-step states and variable changes |
| | Pixel shaders | 3-5 | Needs a new interpreter covering ps_1_x's texture addressing instructions, inputs worked out on the CPU from VS Out, sampling on the GPU, and 2x2 quads for derivatives, `vPos` and `vFace` |
| | Source-level debugging from `D3DXSHADER_DEBUG` debug info | +1 | Optional |
| 6 | **Pixel history:** which draws touched the pixel (occlusion queries per event), which primitive wrote it (drawing one primitive at a time, since D3D9 has no primitive ID), values before and after (`GetRenderTargetData`), depth (the existing bisection), and debugging the pixel (step 5) | 3-5 | |
| 7 | **Remote replay:** display textures through the proxy | 1-2 | Mostly mapping D3D9 formats onto the proxy's D3D11 textures |
| 8 | **Limitations worth fixing** | 3-4 | |
| | `texldl` filtering | 0.5 | |
| | vs_3_0 outputs that pack two semantics into one register | 0.5 | |
| | Alpha test and discard in the overlays | 1 | |
| | Saving UpdateTexture dirty regions in captures | 0.5 | Changes the capture format |
| | D24 at a depth of 0.375 | 0.25 | Check with ZFUNC EQUAL and adjust |
| | Saving DXT textures as PNG | 0.5 | |
| | P8 palettes | 0.5 | Low value, since current GPUs don't create P8 textures |

Left out on purpose:

- **Quad overdraw:** it needs UAVs, which D3D9 doesn't have.
- **Each sample of a multisampled colour target:** D3D9 can't read individual samples.
- **D24FS8 and `D3DMULTISAMPLE_NONMASKABLE` depth stencil readback:** both are rare.
- **Full 32-bit D32 depth readback:** it's kept to 24 bits.
- **Fixed function N-patches and displacement mapping:** both are rare.
- **More than 8 fixed function lights:** drivers rarely allow more than 8 anyway.

Outside this plan:

- **Real games:** the user tests these.
- **Other GPUs:** AMD and Intel may round the depth readback differently, and may sample ATI2's
  blue differently.
- **Upstream merges:** about half a day each time.

## Automated tests

Upstream's `util/test/run_tests.py` runs every D3D9 demo, captures it, and checks the capture. With
`renderdoc.sln` and `util/test/demos.sln` built (Development x64):

```
powershell -File util\test\build_pymodule_py3.ps1
python util\test\run_tests_local.py -t "^D3D9|D3D11_D3DPERF"
```

- `build_pymodule_py3.ps1` builds the python module against the installed Python, since
  `run_tests.py` needs 3.7 or later and the build's own module is for qrenderdoc's bundled 3.6. It
  goes to `x64\Development\py3module\pymodules`, leaving the 3.6 one alone. Rebuild it when
  renderdoc's public API changes.
- `run_tests_local.py` runs `run_tests.py` with that module and `x64\Development`, and stops it from
  asking to register the Vulkan layer: on this machine an installed RenderDoc owns the registration,
  and changing it needs admin rights. Vulkan tests won't work through it; D3D9 and D3D11 ones don't
  need the layer. Other `run_tests.py` arguments pass through (`--artifacts`, `--temp`, `-j`...).
- Results go to `util\test\artifacts\output.log.html` unless `--artifacts` says otherwise.

The tests (`util/test/tests/D3D9`, and `tests/D3D11/D3D11_D3DPERF_Markers.py`) share
`rdtest/shared/D3D9_Test.py`, which:
- fails the test if the demo's log has a TEST_ERROR. Several demos check themselves (the wrapping
  checks, state left alone by reads during a capture, constants above c127 surviving Present), and
  only their log shows it.
- captures the demo itself rather than through `TestCase.get_capture()`, so that the demo runs for
  2 more seconds after the capture: some demos check things in the frames after a captured one,
  which the framework's capture would kill before they're logged. It doesn't support remote
  servers (`--remote`), unlike upstream's.
- runs the demo natively first where a test needs a reference from the GPU. The demos write their
  references to their tmp folder (`--tmp`), so they don't land in the source tree.
- runs the checks of the standalone scripts (`util/test/d3d9_*_check.py`), which each have a
  `check_capture(controller, ...)` function for this, with their output in the test log.

| Test | What it checks |
|---|---|
| `D3D9_Simple_Triangle` | The three triangles at replay, including the pixel shader constants one (96, 191, 0), SaveTexture's alpha, the pipeline state at every draw with constants read at length 0 (`d3d9_pipestate_check.py`), the constant blocks and FVF, replaying every action, and the structured export round trip (`check_export`) |
| `D3D9_Frame_Start_State` | The triangles, render states, and VS/PS float constants (c10, c20, c200, c180, and zero c138, c148) set before the first frame, the pipeline state check, the export round trip |
| `D3D9_Texture_Formats` | `d3d9_formats_check.py` against the native run's reference, and that the reference written under RenderDoc is byte-identical to the native one |
| `D3D9_Mesh_Output` | `d3d9_mesh_check.py` against the native run's reference (a draw without a reference fails), and that the reference written under RenderDoc is byte-identical to the native one |
| `D3D9_Overlay_Test` | `d3d9_overlay_check.py`, with the overlays saved in the test's tmp folder |
| `D3D9_Resource_Contents` | Every object's contents at the start of the frame, as listed under "Checking `D3D9_Resource_Contents` captures": each level, face and slice texel by texel, the previous frame's colour in every region the frames alternate, the vertex and write-only index buffers. Then the triangle drawn from them |
| `D3D9_Capture_Edge_Cases` | The markers, and at the start of the frame: the automatic, multisampled (each sample), D16 and INTZ depth stencils texel by texel, the texture locked across Present, and the surface drawn with GDI |
| `D3D9_Reset_Capture` | Both captures: frame 30's has only "After the reset" and a blue clear, the triggered one only "Frame" and a red clear |
| `D3D9_Wrapping_Check`, `D3D9Ex_Wrapping_Check` | The demo's own checks (in its log), and that the capture loads and replays |
| `D3D9_Counters` | The GPU duration and samples passed counters: each draw's samples passed against what the demo's own occlusion queries counted natively (2048, 0, 1024, 120000, 1, and 1024 for a 4x multisampled 16x16 square), a duration for every action, the full screen heavy draw's against the demo's own timestamps (within 4x either way), and that the replay is back at the event it was at afterwards |
| `D3D9_Resource_Usage` | Every texture, surface, buffer and constants pseudo buffer's usage against the exact list it should have: sampled and unsampled textures (a pixel sampler the shader doesn't read, a texture after a disabled fixed function stage), a vertex texture, used and unused streams, the index buffer only at the indexed draw, UP draws reading no buffers, both MRT targets, depth only with the depth test on, clears (both bound targets, as natively), StretchRect, a resolve, GetRenderTargetData, ColorFill, UpdateTexture, GenerateMipSubLevels and Unlock inside the frame |
| `D3D9_Failed_Calls` | Debug messages for failed calls. The 18 calls give the same results natively and under RenderDoc, and each of the 13 that fail natively has exactly one message, at its own event inside its marker, with the HRESULT as its ID. The event is named after the call ("(failed)") and its result is `D3DERR_INVALIDCALL`. Some parameters are checked: render target index 4, start register 300 or 224, a source rect 128 wide, a texture 0 wide. A failed lock records its texture, which is in the capture, and a failed call's constants aren't read. The 5 calls that succeed natively (3 Clears with targets of different sizes, one with a smaller depth stencil, and an UnlockRect without a lock) have no message, there are no other messages, they're returned only once, and the triangle drawn after the failed calls replays |
| `D3D9_Custom_Shaders` | Custom display shaders. The demo samples each of its textures natively with a ps_3_0 shader and writes what the GPU returned; the test builds the same shader as a custom shader and checks its output against that at 6 texels of every subresource: a default pool 2D texture with 3 mips, a managed one (decoded on the CPU), L8 (sampled as L, L, L, 1), a cube and a volume texture with 2 mips, a render target surface, and a D24S8 depth stencil (depth and stencil from its clears). `SV_Position` and `TEXCOORD0` match the native draw. The prefix's values (`RD_TexDim`, the mip, slice, sample and type, the range) are checked for each texture, a global the prefix doesn't fill is 0, `Texture2D` and `SamplerState` sample like `sampler2D`, the UI's template builds, and a shader with an error or for ps_2_0 doesn't |
| `D3D11_D3DPERF_Markers` | `d3d11_d3dperf_check.py`: the markers nest as drawn, with their colours |

What they don't cover:
- The UI. Anything that shows in qrenderdoc still needs checking there by hand, with screenshots.
- Comparing the D3D9 disassembly with fxc's: there's no fxc in the tests. The unit tests
  (`renderdoccmd test unit "[d3d9]"`) cover the parser and disassembler.
- `GetFrontBufferData`'s surface (it holds whatever was on the screen) and the recorded state
  block's contents, which were checked in the XML export.

First run (`d3d9_automated_tests.md`): all 11 passed in under a minute. Upstream's D3D11 tests,
run to check the framework change, passed apart from `D3D11_Shader_ISA`, which needs AMD's driver
(`atidxx64.dll`) for its AMDIL target and can't pass on this NVIDIA machine.

`--x86` runs the same tests on the 32-bit demos, with `renderdoc.sln` and `demos.sln` also built for
x86 (see "32-bit programs"):

```
python util\test\run_tests_local.py --x86 -t "^D3D9|D3D11_D3DPERF"
```

`D3D9_Test` checks that each capture's recorded machine (`RecordedMachineIdent`) has the bitness of
the demos binary it ran, from the binary's PE header.

## 32-bit programs

32-bit D3D9 programs are captured by the x86 build and replayed by the x64 one, as upstream does for
D3D11:
- The x64 build can't inject into a 32-bit process. `win32_process.cpp` hands the launch to the
  32-bit `renderdoccmd.exe`, which injects the 32-bit `renderdoc.dll`.
- A development build finds that in `Win32\<config>` next to `x64\<config>`; an installed one has
  it in `x86\`. So testing 32-bit programs needs `renderdoc.sln` built for both.
- Captures have the same format from both: window handles are serialised as 64-bit values, and
  nothing serialised is pointer-sized.

Tested in `d3d9_x86.md`:
- **Builds:** `renderdoc.sln` builds for x86 with 0 warnings.
- **Unit tests:** the `[d3d9]` tests pass as 32-bit code, with the same counts as x64.
- **Automated tests:** all 11 pass against the 32-bit demos (`--x86`), each capture recorded as
  `Windows x86 32-bit`.
- **UI:** the x64 qrenderdoc launches, captures and opens a 32-bit demo, with the texture viewer,
  pipeline state and mesh viewer correct.

Nothing in the driver needed changing.

The 32-bit demos leave out upstream's Vulkan and D3D12 demos (`demos.vcxproj`), which no longer
compile for 32-bit.

Native references from the two architectures:
- **`D3D9_Texture_Formats`:** identical.
- **`D3D9_Mesh_Output`:** a few vertex shader colour outputs differ by 1/255. Those references come
  from `ProcessVertices`, which runs on the CPU in the 32-bit or 64-bit `d3d9.dll`, and the two
  round differently. Each run is checked against its own reference.

Not tested:
- **A real 32-bit game near its address space limit:** see the memory item under "Needs testing"
  and step 2b.
- **32-bit replay:** the x86 qrenderdoc.
- **Global process hooking.**

## GPU counters

`d3d9_counters.cpp` gives the event browser's Duration column and the performance counter viewer
two counters, `EventGPUDuration` and `SamplesPassed`, under "D3D9 Built-in":
- **How:** as on D3D11. For each action that isn't a marker, the frame is replayed up to it without
  it, then the action alone is replayed between two `D3DQUERYTYPE_TIMESTAMP` queries and inside a
  `D3DQUERYTYPE_OCCLUSION` query. One `TIMESTAMPDISJOINT` and one `TIMESTAMPFREQ` query cover the
  whole fetch. The queries are made on the real device, so they aren't resources of the capture.
- **Which actions:** every one that isn't a marker, so clears, copies and Present get a duration
  too. Each partial replay ends the scene it started, so the second timestamp comes after the
  action's `EndScene`.
- **Afterwards** the frame is replayed to the event the replay controller was last at. D3D9's
  texture viewer and pixel picking read the live device, so they'd otherwise show the end of the
  frame.
- **Which counters:** a counter is only offered if the GPU supports its queries (checked once with
  `CreateQuery(type, NULL)`). D3D9 has no pipeline statistics query: `VERTEXSTATS`, `PIPELINETIMINGS`
  and the other timing queries are optional, and aren't offered. NVIDIA's driver 32.0.16.1088
  supports none of them, 32-bit or 64-bit (checked with `CreateQuery(type, NULL)` natively).
- **Failures** give -1 for a duration and `0xFFFFFFFFFFFFFFFF` for a sample count, as on D3D11. A
  query that doesn't answer within about 10 seconds (a lost device) counts as failed.

Limitations:
- **Durations depend on the GPU's clocks when they're fetched.** A fetch is too little work to raise
  them, so with an idle GPU every duration comes out several times longer. On the RTX 4070 Ti the
  heavy draw of `D3D9_Counters` takes 0.84-0.88 ms at 2835 MHz (P0, the GPU busy with something else) and
  4.5-11.9 ms with the GPU idle at 210 MHz (P8), in qrenderdoc and from Python alike. Natively it
  takes 0.91-0.97 ms. D3D11's counters are measured the same way.
- **Resolution:** NVIDIA's timestamp frequency is 100 MHz, but its timestamps step in about 1 µs,
  so short draws read 0 or about 1 µs.

Tested in `d3d9_step3.md`: `D3D9_Counters` passes on x64 and x86, with the app's own occlusion and
timestamp queries nested around RenderDoc's at replay. In qrenderdoc the event browser's Duration
column and the performance counter viewer show both counters.

## Resource usage

`GetUsage` lists, for each texture, surface and buffer, the events that use it and how. It drives
the Resource Inspector's "Usage in Frame" list, the timeline's usage markers for the texture being
viewed, and later pixel history. It's recorded while the capture loads, as on D3D11, from the
state the device has at each event (`d3d9_pipestate.cpp`, "Resource usage"):

| Event | Usage |
|---|---|
| Draws | `VertexBuffer` for the streams the vertex declaration reads (not other bound streams), `IndexBuffer` for indexed draws, `VS_Resource`/`PS_Resource` for the textures each stage samples, `VS_Constants`/`PS_Constants` on the constants pseudo buffers, `ColorTarget` for each bound render target, `DepthStencilTarget` when the depth or stencil test is on |
| `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP` | as draws, but no vertex or index buffers: they read the application's memory |
| `DrawRectPatch`, `DrawTriPatch` | as draws, without an index buffer |
| `ProcessVertices` | the streams, the vertex shader's textures and constants, and `StreamOut` for the destination buffer |
| `Clear` | `Clear` on every bound render target for a colour clear, and on the depth stencil for a depth or stencil clear |
| `ColorFill` | `Clear` |
| `StretchRect`, `UpdateSurface`, `UpdateTexture`, `GetRenderTargetData` | `CopySrc` and `CopyDst`, or `Copy` within one texture. A `StretchRect` from a multisampled surface is `ResolveSrc` and `ResolveDst` |
| `GenerateMipSubLevels` | `GenMips` |
| `Unlock` inside the frame, GDI drawing (`ReleaseDC`), `GetFrontBufferData` | `CPUWrite` |

Decisions:
- **Which textures a stage samples** comes from `GetDescriptorAccess`, so with reflection only the
  samplers a shader declares count, and without a pixel shader the texture stages up to the first
  disabled one. A texture bound to an unused sampler isn't listed.
- **Surfaces** are listed under the texture that owns them, as the texture viewer shows them.
- **Render targets** count as written whatever `COLORWRITEENABLE` says, as on D3D11. The depth
  stencil only counts when `ZENABLE` or `STENCILENABLE` is on, since D3D9 programs often leave one
  bound with the tests off.
- **A colour Clear clears every bound render target.** `D3D9_Resource_Usage` checks that natively
  first: with two targets bound, both turn the clear colour. (While writing that demo, a Clear with
  targets of different sizes seemed to fail. `D3D9_Failed_Calls` found that it succeeds natively
  with either target the wider one, with a taller second target (64x36 and 64x32, the sizes that
  seemed to fail), and with a depth stencil smaller than the target. So that failure had another
  cause, which wasn't tracked down.)
- **The constants pseudo buffers** are created after loading, so their uses are kept by index and
  added then.

Tested in `d3d9_step3.md`: `D3D9_Resource_Usage` passes on x64 and x86, and in qrenderdoc the
Resource Inspector and the timeline show the uses.

Not covered: `ProcessVertices`, the patch draws, `UpdateSurface`, GDI drawing and
`GetFrontBufferData` aren't in the test; they share the code paths of the calls that are.

## Debug messages

D3D9's debug runtime doesn't run on Windows 8 and later, so there are no API messages to collect.
What the runtime does do is reject some invalid calls with an error, and those are reported
instead.

A call that fails while capturing is recorded into the frame as a `FailedCall` chunk
(`RecordFailedCall` in `d3d9_device.h`). On replay:
- it's an event named after the call, such as `IDirect3DDevice9::StretchRect (failed)`, with the
  call's name, its result (`D3DERR_INVALIDCALL` and so on) and its parameters;
- the Errors and Warnings window has a message at that event: "IDirect3DDevice9::StretchRect
  returned D3DERR_INVALIDCALL (0x8876086C) when it was captured, so it isn't replayed." The source
  is API, the severity High, the ID the HRESULT, and the category comes from the call (state
  setting, execution, resource manipulation or state creation, as D3D11's debug layer has them).

This covers every device call recorded into the frame, creations made mid-frame, locks and
unlocks, `IDirect3DQuery9::Issue` and state block `Capture` and `Apply`.

Decisions:
- **The call isn't replayed.** A D3D9 call that fails doesn't do anything.
- **Parameters** are written by the call's own `Serialise_` function, after the name and result.
  On replay that function reads them back with the state set so that it doesn't replay anything
  (`ReadFailedCallParameters`).
- **Arrays sized by the call's own counts aren't read:** shader constants, UP draws' vertex and
  index data, `Clear`'s rects and the convolution kernel. The runtime may have rejected the call
  because a count was wrong, and reading that much of the application's memory could crash. Their
  counts are still there.
- **Name and result only** for shader creation (the bytecode might not parse), state block
  creation, `EndStateBlock` and `CreateAdditionalSwapChain`.
- **Calls on objects** (locks, unlocks, `Issue`, `Capture`, `Apply`) record the object, and add it
  to the capture so it shows by name. Resources in a failed device call's parameters aren't added:
  one that only failed calls use shows as "Unknown Resource".
- **A call that fails on replay**, having succeeded when captured, gets a message too while the
  capture loads ("... on replay, though it succeeded when captured.", source RuntimeWarning).
- **Messages are returned once**, as on D3D11.
- **`Present`** isn't covered: it ends the frame before the real call is made.

What the runtime doesn't check, natively on the test machine, so these give no message:
- `SetTexture(300, ...)`, `SetSamplerState(300, ...)`, `SetRenderState((D3DRENDERSTATETYPE)1000,
  ...)` and `SetStreamSource(16, ...)` return S_OK. The first native run of the demo, which made
  them, ended with heap corruption (0xC0000374), so the demo doesn't make them now. Which call
  caused it wasn't narrowed down.
- An unaligned stream offset, and `Issue(D3DISSUE_BEGIN)` on an event query, return S_OK.
- `DrawIndexedPrimitive` without an index buffer fails on the first frame and returns S_OK from
  the second on. `D3D9_Failed_Calls` warns if any call's result changes between frames, since the
  capture is of a later frame than the one whose results it writes.
- `UnlockRect` on a texture that isn't locked returns S_OK.

Tested in `d3d9_step3.md`: `D3D9_Failed_Calls` passes on x64 and x86, and qrenderdoc shows the
messages, the failed calls' parameters, and a failed lock's texture by name.

Not covered: no call in the tests fails on replay only, so those messages are untested. Failed
creations other than `CreateTexture`, failed calls on objects other than `LockRect`, and failed
draws aren't in the test either; the draws because no draw failed reliably natively.

## Custom display shaders

The texture viewer's custom shaders (`d3d9_customshader.cpp`), as on D3D11: the user's pixel shader
is drawn over the viewed subresource into a texture of RenderDoc's own, which the texture viewer
shows, picks and saves instead. That texture is A16B16G16R16F with the viewed texture's size and
mips, created through the wrapped device like the overlay texture so that it has a ResourceId.

D3D9 has no texture objects, constant buffers or integer loads, so D3D9's custom shaders are HLSL
compiled as ps_3_0 (or ps_3_0 bytecode), with their own prefix (`GetCustomShaderSourcePrefixes`):
- **The texture is in a sampler.** It's the texture the texture viewer itself samples
  (`GetDisplaySource`): the application's texture where that can be sampled directly, a copy of a
  render target surface, or the subresource decoded on the CPU into a 2D A32B32G32R32F texture.
  RenderDoc binds it to every sampler the shader declares with the matching type (`sampler2D` for
  2D data, `samplerCUBE` for a cube texture, `sampler3D` for a volume), read from the bytecode's
  `dcl` instructions, with point filtering and clamping. The prefix names `s0`, `s1` and `s2`
  (`RD_FLOAT_2D_BINDING` and so on), but HLSL's `Texture2D` and `SamplerState` pairs work too,
  though fxc gives them a sampler register of its own.
- **`RD_TextureType()` says which sampler holds the data**, not the texture's type: 2D, 3D or Cube,
  or Depth and DepthStencil for depth stencils, which are decoded to 2D with depth in red and
  stencil in green. A cube face or volume slice that has to be decoded is 2D.
- **The `RD_` functions read float4 constants** that the compiler places, found by name in the
  shader's constant table. Every other constant is 0, as D3D11 leaves the `$Globals` it doesn't
  recognise. ps_3_0 only converts a float it knows is positive to `uint`, so the prefix's
  functions use `max(x, 0)` first.
- **The vertex shader is the texture viewer's**, which now writes all of `TEXCOORD0` (z and w 0).
  `SV_Position` is `VPOS` on ps_3_0, which has pixel centres at integer coordinates, where D3D11's
  are at .5.
- **`RD_SelectedSample()`** is -samples for a multisampled render target, which is resolved, and
  the selected sample for a multisampled depth stencil (the first for "average").
- **YUY2 and UYVY** are decoded to (V, Y, U) at full rate, so `RD_YUVDownsampleRate()` is (1, 1, 1,
  8) and `RD_YUVAChannels()` (1, 2, 0, 255) for them, and 0 for other formats.
- **The prefix ends with `#line 1`**, so compile errors give the line in the user's file.
- **The older `RENDERDOC_` globals** that D3D11 fills by name aren't supported.

The sampled values are what a D3D9 shader gets, not what the texture viewer shows: L8 samples as
(L, L, L, 1) where the texture viewer shows (L, 0, 0, 1), and sRGB isn't decoded
(`D3DSAMP_SRGBTEXTURE` is off). Decoded textures have the values the texture viewer shows.

qrenderdoc's shader editor inserts D3D9 snippets (constants, samplers, resources) for D3D9
captures, and `docs/how/how_custom_visualisation.rst` has a D3D9 section.

Tested in `d3d9_step3.md`: `D3D9_Custom_Shaders` passes, and in qrenderdoc a custom shader shows a
texture's alpha and a depth stencil's stencil, and the editor inserts the D3D9 snippets.

Not covered: multisampled textures, YUV formats and textures decoded because of their format (P8,
ATI1 and so on) aren't in the test; they go through the same decoded path as the managed texture
and the depth stencil.

## Needs testing

- **Reset / ResetEx with real games.** `D3D9_Wrapping_Check` and `D3D9Ex_Wrapping_Check` exercise
  Reset in a controlled way, but it hasn't been tested with a real game yet. Things to try: alt-tab
  out of and back into fullscreen, changing resolution, games that recreate D3DPOOL_DEFAULT
  resources on device loss. Watch for Reset failing only under RenderDoc (something of ours holding a
  D3DPOOL_DEFAULT reference), and for crashes after Reset (stale back buffer or depth stencil
  wrappers).
- The D3DPERF marker hooks moved to the D3D11 project upstream. They now share `d3d9.dll` with our
  device creation hooks. `D3D11_D3DPERF_Markers` draws inside D3DPERF regions and a marker, with no
  D3D9 device, and `util/test/d3d11_d3dperf_check.py capture.rdc` checks the capture's action tree.
  Tested in `d3d9_formats_d3dperf_test.md`: the markers nest as drawn and keep their colours, and
  upstream's `D3D11_Refcount_Check` passes under RenderDoc. The D3DPERF functions return different
  values under RenderDoc (BeginEvent 0 where native returns -1); that's upstream's D3D11 hook, and
  the refcount test doesn't depend on it.
- Toggling the overlay in a real game shouldn't change anything in the scene. The overlay saves and
  restores only the state it changes; that's tested with `D3D9_Frame_Start_State` and the wrapping
  checks, but not yet in a game.
- Device loss during a capture: the capture is discarded and retried when the device works again.
  Only tested by reading the code, losing a device on purpose needs a real fullscreen game.
- Applications' own `D3DSBT_ALL` state blocks have the same constant-copying behaviour natively:
  `CreateStateBlock(D3DSBT_ALL)` stores copies of c0-c127 as c128-c255 (with hardware and pure
  vertex processing), while one refilled with `Capture()` holds the right values. We pass them
  through as they are; replay (phase 3) creates them the same way, and the creation's values are
  replaced by the initial contents anyway.
- Sampler 256 (`D3DDMAPSAMPLER`) reads back `ADDRESSU` 0xBAADCAFE natively. It's recorded as read.
- Capture time and memory use with resource contents in a real game. Contents are read during the
  captured frame, and the shadow copies (below) cost memory in 32-bit games. Capturing 32-bit
  programs works (`d3d9_x86.md`), but only with the demos, which use little memory.

## Known gaps in resource contents

- Depth stencils that can't be locked are read with the depth and stencil tests
  (`d3d9_depth.cpp`): a bisection per depth bit with `oDepth` and ZFUNC LESSEQUAL, and a test per
  stencil bit with STENCILFUNC EQUAL. Multisampled ones are read a sample at a time with
  D3DRS_MULTISAMPLEMASK and stored with one slice per sample. What's left:
  - D24FS8 (its depth is a 20e4 float) and D3DMULTISAMPLE_NONMASKABLE still can't be read, and
    keep a note saying so. D32 is read to 24 bits.
  - It needs shader model 3 and d3dcompiler_47.dll, which RenderDoc ships.
  - What's stored is the integer l whose depth, computed in the shader as fp32 `l * (1 / max)`,
    the GPU rounds to the stored level. Replay has to write it back with exactly that formula.
    Tested on an RTX 4070 Ti, which rounds fragment depths to the format's levels before comparing,
    and stores Clear's value as one of the levels too: for 848 D16 values (cleared and drawn) the
    value read back is exact and writing it back gives the same level, and for 424 D24 values
    (D24S8, D24X8 and INTZ behave the same) writing it back gives the same level in 423. The
    exception is 0.375: neither 0x5FFFFF nor 0x600000 written back gives its level. If that shows
    up in games, replay could check what it wrote with ZFUNC EQUAL and nudge the depth.
  - The integers aren't always the level's index: fp32 `1 / max` rounds up, so for about 70% of D24
    depths above about 0.4 they're one below it. D16 cleared to 0.5 reads 0x7FFF, which is its
    level (natively, every depth from 0.49999237 to 0.5 compares EQUAL to it). Computing the depth
    more precisely (`l * 2^-24 + l * 2^-48`) made it worse near 1.0, as shader adds don't round to
    nearest. There's no lockable D16 or D24 on that GPU (D16_LOCKABLE fails), so the true bits
    can't be checked directly.
  - Reading needs multisampled R32F render targets for multisampled depth stencils.
  - Replay writes them back with `oDepth` and ZFUNC ALWAYS, and the stencil with a draw per bit
    using REPLACE and a write mask of that bit (`WriteDepthStencil`, phase 3a).
  - Not considered: creating the application's depth stencils as INTZ textures behind its back. D16
    would become 24-bit, it can't be multisampled, and it changes behaviour even without a capture.
- State blocks are read by applying them to the real device, reading the whole device state back
  and setting back whatever changed, so their initial contents are a full device state. Which
  states a state block covers comes from its creation: the type for CreateStateBlock, and the calls
  between BeginStateBlock and EndStateBlock, which are kept with its creation chunk even when they
  happen outside a capture. A state block (or depth stencil) that has to be read while the
  application is recording a state block gets a note instead, since our calls would be recorded.
- D3DPOOL_DEFAULT textures without RENDERTARGET, DEPTHSTENCIL or DYNAMIC usage can't be locked or
  copied from, so we keep a copy of what UpdateTexture and UpdateSurface upload to them (the shadow).
  That costs as much memory as the textures themselves, all the time, which matters for 32-bit
  games. Formats whose layout we don't know get no shadow.
- D3DUSAGE_WRITEONLY buffers are read back by locking them read-only, which works on current
  drivers but is slow, and isn't guaranteed to return the data.
- GDI drawing is recorded as the whole surface after ReleaseDC, and GetFrontBufferData as the
  whole destination surface, since replay can't redo either.
- A lock taken before the capture and released during it is recorded at its Unlock. The object's
  initial contents are read after the Unlock (they can't be read while it's locked), which is fine
  because the Unlock chunk then writes the same data again. An object still locked when the frame
  ends can't be read and gets a note.
- The back buffer's initial contents include the overlay text, since the overlay is drawn into
  the back buffer before the Present the capture starts at.
- UpdateTexture copies the source's dirty region, which we track but don't store in captures yet.
  At replay the source's dirty region is whatever writing its initial contents and the frame's
  locks made dirty, which can be more than the capture's, so an in-frame UpdateTexture can copy more
  than it did.

## Checking `D3D9_Resource_Contents` captures

`D3D9_Resource_Contents` should log "UpdateTexture copied only the dirty region", natively and
under RenderDoc; our shadow copies of D3DPOOL_DEFAULT textures assume that. In a capture of it, each `InitialContents` chunk names its object and holds its
subresources (A8R8G8B8 pixels, so each DWORD is 0xAARRGGBB, stored as bytes BB GG RR AA):
- managed texture (8x8, 4 levels): levels 0xFF111111, 0xFF222222, 0xFF333333, 0xFF444444, except
  level 0's (2,2)-(6,6), which has the *previous* frame's colour: 0xFF00FF00 (green) and
  0xFF0000FF (blue) alternate. The frame's `IDirect3DTexture9::UnlockRect` has the other one.
- D3DPOOL_DEFAULT texture (16x16, 2 levels, from the shadow): 0xFF203040, with 0xFFA0B0C0 in the
  top-left 8x8 of level 0 and 4x4 of level 1, and 0xFF5A5A5A at (12,12)-(16,16) of level 0 with
  the previous frame's colour at (13,13)-(15,15).
- render target texture (32x32): 0xFFFF0000 with the previous frame's colour in the top-left 16x16.
  The frame's `ColorFill` has the other one.
- multisampled render target (64x64, if 4x MSAA is supported): 0xFF00FFFF.
- cube texture (4x4): faces 0-5 are 0xFF000010, 0xFF000020 ... 0xFF000060.
- volume texture (4x4x4): slices 0xFF000100, 0xFF000200, 0xFF000300, 0xFF000400.
- managed vertex buffer: the default triangle. Default pool index buffer: 0, 1, 2. This one is
  D3DUSAGE_WRITEONLY, so it shows whether reading those back works.
- system memory surface (4x4): 0xFF5A5A5A with the previous frame's colour at (1,1)-(3,3).
- recycled texture (4x4): one colour, from the previous frame. It's released mid-frame, and a new
  one is created and filled in the frame (`CreateTexture` and `UnlockRect` chunks).
- automatic depth stencil: no subresources, and a note that D3DFMT_D24S8 can't be read back.
- the frame's `IDirect3DVertexBuffer9::Unlock` chunks: one with Flags 0x2000 (DISCARD) covering
  the first 4 quads, one with 0x1000 (NOOVERWRITE) at the offset of the 5th.

Tested (5fda0357f, Release x64, RTX 4070 Ti): all of the above matched. The recycled texture's old
instance held 0xFFFCFC87 and the new one got 0xFFFDFD88 in its `UnlockRect`. The DISCARD `Unlock`
covers the whole 840 bytes because the demo locks (0, 0); its last 168 bytes are the previous frame's
data. The wrapping checks, `D3D9_Simple_Triangle` and `D3D9_Frame_Start_State` were unchanged apart
from the new `InitialContents` chunks.

## Checking `D3D9_Capture_Edge_Cases` captures

The demo should log no errors, natively or under RenderDoc. The checks it makes after the draws
and state block calls that make a capture read something fail only in the captured frame, if
reading changed the application's state. In a capture:
- automatic depth stencil (D24S8): one subresource with a note that it was read back with the depth
  and stencil tests. DWORDs are depth << 8 | stencil: 0x4000005A at (0,0)-(100,100), 0xBFFFFFA5 at
  (100,0)-(200,100), 0xFFFFFF00 everywhere else.
- multisampled depth stencil (64x64, if 4x MSAA is supported): Depth 4, one slice per sample. Below
  the diagonal 0x80000033 in every sample, including the top row and left column; above it
  0xFFFFFF00, and pixels on the diagonal differ between slices.
- D16 surface (32x32): every pixel 0x7FFF. INTZ texture (32x32, if supported): 0x5FFFFF0F (one
  below 0.375's nominal level, see the known gaps).
- the recorded state block: before the frame, its `BeginStateBlock`, `SetRenderState`
  (D3DRS_TEXTUREFACTOR), `SetTexture` and `SetVertexShaderConstantF` chunks come just before its
  `EndStateBlock`. Its initial contents have one StateBlock element with D3DRS_TEXTUREFACTOR
  0xFF336699, Textures[0] the 4x4 texture and VertexShaderConstants.Float[20] (1, 2, 3, 4).
- the D3DSBT_PIXELSTATE state block: D3DRS_ALPHAREF 0x10 or 0x20, from the previous frame. The
  frame's `SetRenderState` before its `Capture` has the other one.
- the D3DSBT_ALL state block: VertexShaderConstants.Float[200] is (200, 201, 202, 203), since the
  demo refills it with `Capture()` every frame.
- the 4x4 texture locked across Present: one of the first chunks is its `UnlockRect`, with 16
  pixels of 0xFF00FF00 or 0xFF0000FF. Its initial contents (read once it's unlocked) are the same.
- `IDirect3DSurface9::ReleaseDC`: the 32x32 surface, 0x00123456 or 0x00654321 in the top-left
  16x16 (alternating between frames; GDI might leave the top byte 0) and 0x00808080 elsewhere.
- `GetFrontBufferData`: the display-sized surface, holding a copy of the screen.
- markers: `D3DPERF_BeginEvent` "Depth", `D3DPERF_SetMarker` "Multisampled", `D3DPERF_EndEvent`,
  `D3DPERF_BeginEvent` "State blocks", `D3DPERF_EndEvent`.

## Checking `D3D9_Reset_Capture`

Run it under RenderDoc for 70 frames or more. It should make two captures, and RenderDoc's log
should say "Device reset during a capture" twice:
- the in-app API capture of frame 30, which only has what came after the reset: the "After the
  reset" marker, a blue Clear and the triangle.
- a capture of frame 61. The triggered capture of frame 60 was reset half way through, so it's
  discarded and the next frame is captured instead. It has a single "Frame" marker, a red Clear and
  no reset.

Tested (fb546b03e, Release x64, RTX 4070 Ti): both demos matched, with no errors natively, under
RenderDoc or in the captured frame. That version also rounded depths written by Clear to the nearest
level, which only changed INTZ to 0x6000000F; it's since been removed, since the GPU doesn't keep
Clear's value as a float after all. The existing demos' automatic depth stencils read 0xFFFFFF00
where they're cleared to 1.0.

## Fixed

- The overlay used to restore the application's state with a `D3DSBT_ALL` state block it created
  each frame. Creating one stores copies of shader constants c0-c127 as c128-c255 (natively too, on
  an RTX 4070 Ti), so applying it corrupted applications using upper constants every frame. It also leaked stage 0 `D3DTSS_CONSTANT`
  (0xFFFFFFFF), which the state block didn't restore. It now saves and restores only what it changes.
  Tested: c200/c180 survive in `D3D9_Frame_Start_State`, c138/c148 stay zero, and `D3DTSS_CONSTANT`
  keeps the application's value. Nothing else in `BeginCaptureFrame` changed.

## Phase 3 test round

Phases 3a-3d were built (Development x64, no build errors) and tested from Python on Windows with an
RTX 4070 Ti; the report is `d3d9_phase3_report.md` in `test_reports/`. Four bugs blocked
testing; they were tested with workarounds, now replaced by these fixes:
- Shader bytecode came back empty on replay, so no capture loaded: `SERIALISE_ELEMENT_ARRAY` takes
  its count by value, so the length has to be serialised on its own. **This changes the capture
  format: captures made before it don't load.**
- `GetPaletteEntries` on a palette that was never set crashes inside d3d9 (on a D3D9Ex mixed-VP
  device) instead of failing, which crashed loading `D3D9_Frame_Start_State`. The replay now sets
  each captured palette before fetching the device's state, so only palettes that were set are
  ever read.
- Stencil was written back wrong (90 became 122): `StencilWritePS` added its 0.25 after dividing by
  the bit instead of before.
- Clears weren't replayed or listed. They now are, with render target 0 (or the depth stencil, for
  a depth-only clear) as the copy destination.

Since then:
- `d3d9_replay_check.py` saves `_first.png` and reads the depths at event 0, the frame's initial
  state, since the first event is often a Clear now that Clears replay.
- The disassembly matches fxc's three formatting differences (see phase 3d).
- `GetCBufferVariableContents` with a length of 0 read zeros, so scripts had to pass the
  descriptor's size (the UI does). The replay controller deliberately skips fetching any data for
  a length of 0: upstream made it mean an empty range, for D3D11 constant buffers bound with a
  size of 0 (the docstring's "0 to fetch the rest of the buffer" is out of date; `GetBufferData`
  itself still reads to the end for 0). That's left alone. D3D9's constant blocks aren't buffer
  backed and can't be bound partially, so D3D9's `FillCBufferVariables` now reads the stage's
  register file from the device itself when it's given no data, as GL does for bare uniforms.
  `def`/`defi`/`defb` literals still replace the registers they define. Passing the descriptor's
  size works as before. To check: `d3d9_pipestate_check.py` compares the two for every constant
  block a shader uses (see "Checking the pipeline state"). The retests (`d3d9_cbuffer_len0_retest.md`,
  `d3d9_followups_retest.md`) passed every VS float, int and bool block in `D3D9_Mesh_Output`, and
  the UI's constant buffer viewers for D3D9 and D3D11 are unchanged. Pixel shader blocks are
  covered by `D3D9_Simple_Triangle`'s third draw, whose pixel shader reads c#, i# and b# registers
  (see "Checking the pipeline state"). `d3d9_ps_constants_retest.md` passed its float, int and
  bool blocks, with the triangle at 96, 191, 0 natively and at replay, and the variable names in
  the constant buffer viewers.
- The one crash (a call through a null pointer) loading `D3D9Ex_Wrapping_Check`, once in 78 loads,
  wasn't D3D9's (`d3d9_d3d9ex_crash.md`). The test scripts call `rd.InitialiseReplay` inside
  qrenderdoc, which had already called it, and each call started a GPU enumeration thread without
  waiting for the last one. Replay drivers are only created after that thread is joined, but only
  the newest handle was kept, so two enumerations ran at once while the capture loaded. Their
  Vulkan proxy devices share the replay dispatch tables, which each creation zeroes and refills,
  and one thread called `EnumerateDeviceExtensionProperties` while it was null. Calling
  `InitialiseReplay` 6 times in a row crashed 9 runs in 20. `InitialiseReplay` now waits for any
  earlier enumeration, and doesn't enumerate again once it has found GPUs. Vulkan's shared tables
  are unchanged: they're safe while only one replay driver is created at a time. The retest
  (`d3d9_followups_retest.md`, of 3cff00146) had no crashes in 20 runs of the 6-call script and 20
  `D3D9Ex_Wrapping_Check` loads, every log had one set of proxy devices and no `d3d12_replay.cpp`
  assertion, and Settings still lists each GPU's APIs once.

The retest (`d3d9_phase3_retest.md`, of ea95bb75e) passed the four fixes, `_first.png`, the
Clear destinations, 23/23 shaders against fxc, the unit tests, 10 more `D3D9Ex_Wrapping_Check`
loads (the crash is now 1 in 18) and the UI checklist, except for these, now fixed:
- ps_1_4's `t#` source selectors: fxc prints `.xyzz` and `.xyww`, so there's no exception to the
  swizzle rule after all.
- Save Texture as PNG (UI and Python) wrote alpha 0 for X8R8G8B8 back buffers: the readback
  returned the X byte as it was, and RenderDoc reads it as alpha. The readback now sets the unused
  bits of X8R8G8B8, X8B8G8R8, X1R5G5B5 and X4R4G4B4 to 1, as sampling does. Saving formats other
  than 8-bit RGBA (16-bit, float, luminance, depth) used to log "isn't supported yet" and write
  the data unconverted. They're now converted on the CPU, with the black and white points. Block
  compressed formats still aren't.
- The Pipeline State panel showed the Mesh View icon at its full size in the toolbar. It's now
  24x24. The topology diagram sat on the far right, after the declaration; topology and its
  diagram are now on their own left-aligned row above the declaration.
- Event Browser rows that link resources (`ReleaseDC(Offscreen Surface 32)`) were clipped. The
  list has uniform row heights, which Qt takes from the first row, and D3D9's first rows are plain
  text (Clear has no resource parameter), which is shorter than a row with links. The Event
  Browser's delegate made every row at least as tall as a line with links (removed again after
  retest 3, see below).
- Viewing a constants buffer (`VS float constants (c#)`) from the Resource Inspector didn't use
  the `float4` format the Pipeline State's Go button does. The buffers now carry the
  `__rd_format` annotation the Resource Inspector looks for.
- In one of three `D3D9_Reset_Capture` runs, frame 30's back buffer and automatic depth stencil
  kept their IDs from before the reset. Reset kept their wrappers when the new surfaces had the
  old ones' addresses, which happens when the new surface is allocated where the old one was. It
  replayed correctly only because the demo resets to the same size. Reset now always wraps them
  again.

Retest 2 (`d3d9_phase3_retest2.md`) passed everything except two UI items, now fixed:
- The IA topology diagram was still drawn at its SVG size. The D3D9 panel now scales it to two
  lines of text, keeping its aspect ratio.
- Event Browser rows drawn through RichResourceText (resource links, or any row with parameters
  when Show parameter names is on) are drawn low and clipped at this machine's DPI. D3D11
  captures do the same, so it's an existing qrenderdoc issue and not D3D9's. Neither attempted
  fix (a minimum row height, then a line height from the font) changed it in retest 3, so both
  were removed. The phase 3e retest's log found it: the rows are 14 pixels high (the font is 10),
  but the rich text document is 18 (the cells' 14px line height plus padding), or 20 with a
  resource link (its 16 pixel icon). RichResourceTextPaint only centred a document shorter than
  the row, and drew a taller one from the top, so its text sat low and its bottom was cut off. A
  taller document was then centred too, which retest 2 found better but still about 4 screen
  pixels low, with the link icon as tall as the row and no visible hover underline. Centring the
  document can't line its text up: its height includes the cells' padding and the icons, and
  where a fixed line height puts the text inside its line differs between Qt versions (on Qt 5.15
  the same code draws it 2 pixels high instead). A single line document is now placed so that its
  text's baseline is where a plain row in the same font puts it, with fractional offsets. An
  offscreen Qt 5.15 test matched plain rows exactly in 45 of 48 font, row height and scale
  combinations, and was 1 pixel off in the rest. Documents with several lines are still centred.
  Mouse hit testing uses the same offset, and a hovered link is underlined at the font's
  underline position. Retest 3 (`d3d9_phase3e_retest3.md`) confirmed it in D3D9 and D3D11: rich
  rows have the same baseline as plain rows, descenders are whole, and hover and clicks work
  anywhere on the row.
  The link and message icons in rich text are sized to the font and drawn smoothly. Retest 3
  measured the sizes: 1.25 times the ascent (10 pixels at the 8pt UI font) centres the icon on
  the capitals, so it's that, kept within one line and between 8 and 16 pixels. Cells holding only
  a ResourceId or GPU address used the 16 pixel icon, twice the rich text one in retest 3, so they
  now use the same size, centred on the name, and their row height hint follows it. Retest 4
  (`d3d9_phase3e_retest4.md`) measured 9 pixels, because the one line cap rounded the 9.64 line
  spacing down; it rounds up now. It also found those cells' hover area had shrunk to the text,
  which put the underline through the glyphs and left the top and bottom of the row dead. They now
  hover and click over the full cell height and underline at the font's underline position, as
  rich text does. Retest 5 (`d3d9_phase3e_retest5.md`) confirmed both in D3D9 and D3D11: 10
  pixel icons clear of the separators, underlines 1 pixel below the baseline, and hover and clicks
  working across the full row height. The probe
  (`util/test/richtext_probe`) had the same fonts, but rows 18 pixels high. The
  `RENDERDOC_RICHTEXT_LOG` and `RENDERDOC_RICHTEXT_STYLE` debugging variables used to find this
  have been removed.
- The white backgrounds on the `Depth` and `State blocks` markers are expected: the demo passes
  0xffffffff as their colour.
- The "Real D3D9 object ... already has a wrapper, replacing it" warning in one Reset run came from
  the automatic depth stencil. After Reset the new wrapper was made before the old one was
  released, so when the new surface reused the old address the old wrapper was still registered.
  Replay was fine (the newer wrapper wins), but the old wrapper is now released first.

Retest 3 (`d3d9_phase3_retest3.md`): the IA diagram and the Reset warning are fixed.

## Replay (phase 3a)

How it works is described at the top of `d3d9_replay_device.cpp`. In short: a D3D9Ex device on a
hidden window replays through the same wrappers the application used, and wrappers created during
replay keep their captured IDs. The capture's implicit swapchains become additional swapchains,
windowed, and its automatic depth stencil a standalone one. D3D9Ex has no managed pool, so managed
objects go in the default pool, and textures that can't be rendered to are made D3DUSAGE_DYNAMIC
where the driver allows, so they can be locked to write and read them. Buffers lose
D3DUSAGE_WRITEONLY. Depth stencils are written back with `oDepth` and the stencil a bit at a time
(`d3d9_depth.cpp`).

On its own, 3a gives the event browser, the resource list, the structured data and fetching data
from Python. The texture viewer is phase 3b and the D3D9 pipeline state phase 3c, below.

Known gaps:
- Multisampled render targets are written back by copying their resolved contents into a
  non-multisampled render target and StretchRect-ing that into the multisampled one. If the driver
  doesn't allow that, a warning is logged and the contents stay as they are.
- Default pool volume textures that can't be made dynamic can't be written or read.
- Textures replay made dynamic are the destination of any in-frame UpdateTexture and UpdateSurface
  calls. That's allowed as far as I know, but hasn't been tried.
- In-frame UpdateTexture copies the dirty region replay has (see above).
- D24FS8 and D3DMULTISAMPLE_NONMASKABLE depth stencils aren't written back (they weren't read
  either), and the 0.375 D24 value lands one level off.
- (Fixed in 3b: output window screenshots, `GetOutputWindowData`, are implemented.)

### Checking replay

`util/test/d3d9_replay_check.py` opens a capture with the Python module, prints its events and
textures, saves the back buffer at the start of the frame (event 0, before anything in it has
replayed) and at the last event as `<capture>_first.png` and `<capture>_last.png`, saves it again
at the last event with `SaveTexture` as `<capture>_saved.png` and prints its alpha range (255-255
for an X8R8G8B8 back buffer), and prints the depth stencils' values at the start of the frame, which are their
initial contents as written back at replay (in D3D9's layout, depth << 8 | stencil, as listed
above). `--all` also steps through every action. To run it from a build:

```
set PYTHONPATH=<build>\x64\Release\pymodules
set PATH=<build>\x64\Release;%PATH%
python util\test\d3d9_replay_check.py capture.rdc --all
```

What to look for:
- every demo's capture loads, in the script and in the UI, and the event browser shows its calls
  with draws, clears, copies and presents as actions, and D3DPERF markers as regions
  (`D3D9_Capture_Edge_Cases` has "Depth" and "State blocks").
- `<capture>_last.png` looks like the application's window when it was captured, without the
  overlay text. `<capture>_first.png` is the previous frame, with the overlay text.
- `D3D9_Capture_Edge_Cases`: the depth values at the start of the frame match the ones listed for its
  initial contents above (0x4000005A, 0xBFFFFFA5 and 0xFFFFFF00 for the automatic depth stencil,
  0x80000033 and 0xFFFFFF00 in each sample of the multisampled one, 0x7FFF for D16, 0x5FFFFF0F for
  INTZ).
- RenderDoc's log has no errors, and no "Couldn't write the contents" warnings except for the
  gaps above.

## Texture viewer (phase 3b)

This replaces the 3a notes above about `RenderTexture` and
`GetOutputWindowData` not being implemented.

How it works:
- `d3d9_rendertexture.cpp`: `RenderTexture`, `RenderCheckerboard`, `RenderHighlightBox`,
  `GetOutputWindowData`. All drawing happens on the real device, into the bound output window.
  `WrappedD3DDevice9::SaveReplayDrawState`/`RestoreReplayDrawState` read the device state before
  and put it back after, with the render targets and the bindings the wrappers track, the same way
  `d3d9_depth.cpp` does. The shaders are SM3, compiled with D3DCompile on first use.
- A subresource is displayed one of three ways (the comment at the top of the file has details):
  textures in D3DPOOL_DEFAULT whose format the sampler returns as described are sampled directly,
  with a channel matrix that moves the channels to where the format's description has them (L8
  samples as LLL1 and shows as L001); render target surfaces in those formats (back buffers,
  standalone render targets) are copied into a texture with StretchRect, which also resolves
  multisampled ones; everything else (depth stencils, INTZ/DF16/DF24, P8, A4L4, R3G3B2, L6V5U5,
  X8L8V8U8, A2W10V10U10, CxV8U8, YUY2/UYVY, the RGBG formats, ATI1/ATI2, R8G8B8, system memory and
  scratch textures, offscreen plain surfaces) is read with `ReadSubresource`, decoded on the CPU and
  uploaded to an A32B32G32R32F texture.
- The display shader is D3D11's in SM3 form: range, channel masks, mip, slice, cube face, volume
  slice, flip, RGBM, YUV decode, NaN/inf/negative and clipping overlays, gamma, alpha blended over
  the checkerboard. Output windows are written with `D3DRS_SRGBWRITEENABLE` where X8R8G8B8 supports
  it (like D3D11's sRGB output windows); otherwise the shader encodes sRGB itself and blending
  happens in gamma space. `ClearOutputWindowColor` encodes its linear colour to sRGB for the same
  reason, since `ColorFill` writes it as it is.
- `d3d9_pixel.cpp`: `PickPixel`, `GetMinMax` and `GetHistogram` run on the CPU over the decoded
  subresource (decoders for every format listed above, BCn through compressonator). Textures the
  CPU can't read (D3DPOOL_DEFAULT ones replay couldn't make dynamic) are drawn raw through the
  display shader into a float render target and read back instead. Decoded data is cached per
  resource, mip, slice and sample, and dropped on every `ReplayLog`.
- Decoded values use the channels the format is described with: luminance is red, A8L8 and A4L4
  alpha is green, L6V5U5 and X8L8V8U8 are (U, V, L, 1), CxV8U8 has its computed C in blue, YUV is
  (Cr, Y, Cb, 1) which "decode YUV" turns into RGB, depth is red and stencil is green (as
  stencil / 255, like the other APIs). P8 and A8P8 are indices (and alpha) as integers.
- `D3D9MakeResourceFormat` now also describes P8/A8P8 (R8/R8G8 UINT), A4L4 (R4G4), CxV8U8
  (R8G8_SNORM), A2W10V10U10 (R10G10B10A2 SNORM) and D15S1 (depth). The rest that RenderDoc has no
  format for (R3G3B2, A8R3G3B2, L6V5U5, X8L8V8U8, UYVY, the RGBG formats) are Undefined with four
  UNORM channels, and still display.
- `d3d9_rendermesh.cpp`: `RenderMesh` for the mesh viewer's input data. Positions (and the
  secondary element) are decoded on the CPU with the index buffer expanded, into a vertex buffer of
  our own, cached per draw until the next replay; D3D9 can't declare every vertex format the viewer
  is given. Solid, wireframe, lit (face normals from screen space derivatives, D3D9 has no geometry
  shaders), secondary, exploder, other draws in the pass, the highlighted vertex and primitive, the
  bounding box and axis markers are drawn. Buffer contents come from 3a's `GetBufferData`.

Known gaps:
- The mesh viewer's inputs come from the pipeline state (3c): the vertex declaration's elements,
  the streams' offsets and strides, and the index buffer, with the draw's start index and base
  vertex from its action. Post-transform data is 3e, below.
- (Fixed in 3e: `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` data is now kept at load and
  presented as pseudo buffers, see "Mesh output" below.)
- Multisampled colour targets are only shown resolved, whichever sample is picked. Multisampled
  depth stencils show each sample.
- (Since step 3.) Custom display shaders are supported, see "Custom display shaders". The
  overlays are in phase 3e (below).
- P8 and A8P8 show their indices, not the palette colours.
- (Fixed after `d3d9_formats_d3dperf_test.md`.) ATI2 was decoded like BC5, with the first half of
  each block as red. D3D9 samples the second half as red and the first as green, so the decode and
  `GetTextureData` (which describes ATI2 as BC5) now swap the halves. Retested in
  `d3d9_formats_retest.md`: passed. That retest found the decode gave blue 0 where the GPU samples 1;
  the decode now gives 1 (`ff8a6dd08`). Retested in `d3d9_formats_retest.md` ("ff8a6dd08: ATI2
  blue"): passed, with blue compared, and the texture viewer picks 0.658824, 0.486275, 1, 1 at
  texel 0,0. `GetTextureData`'s data, and so a saved DDS, is still BC5 and has blue 0.
- (Fixed after `d3d9_formats_d3dperf_test.md`.) Drivers report ATI1 and ATI2's lock Pitch as one byte
  per pixel column (16 for a 16 wide level), less than a row of their blocks, while the GPU reads the
  blocks tightly packed. Capture copied only Pitch bytes of each row, so most of the texture was
  lost. Reading, recording and writing locks of these now use the size of a row of blocks when the
  reported Pitch is smaller (`D3D9LockedRowPitch`). Locks of part of an ATI1/ATI2 level aren't
  tested: where the runtime puts the region's first block isn't known. Retested in
  `d3d9_formats_retest.md`: whole-level locks passed.
- (Fixed after `d3d9_formats_d3dperf_test.md`.) DXT2-5 colours were decoded on the CPU with DXT1's
  3-colour palette when the first endpoint isn't bigger, which only DXT1 has. This affected pick
  pixel and the texture viewer's values, not the GPU sampling at replay. Retested in
  `d3d9_formats_retest.md`: passed.
- D32 depth is read back to 24 bits and described as D32_FLOAT by 3a, so its raw data in the
  buffer view of the texture looks wrong, though display and pick pixel are right. D24FS8 and
  D3DMULTISAMPLE_NONMASKABLE depth stencils can't be read, so they don't display.
- The texture viewer and mesh viewer need shader model 3 hardware. Linear filtering when zoomed
  out is only used for natively sampled formats that support it; decoded ones are always point
  sampled.
- Textures in the CPU path are read back and decoded whenever the event changes, which can be slow
  for big depth stencils (they're read with the depth tests).
- At an event between `BeginStateBlock` and `EndStateBlock` nothing is drawn (the texture and mesh
  viewers stay empty and their depth isn't cleared, a warning is logged once), since our state
  calls would be recorded into the application's state block instead of reaching the device.
  Pick pixel still works for textures the CPU can read, other than depth stencils.

What to test on Windows (all in the UI unless noted):
- Open the `D3D9_Simple_Triangle` capture, select the Present and open the back buffer: the
  triangles show on the grey background (green in the middle, magenta in the bottom left, and
  96, 191, 0 in the bottom right), the thumbnail strip shows the back buffer, and hovering
  shows the right colours in the pixel picker (the background picks as 0.2, 0.2, 0.2, 1, which is
  0x33 in each byte). Zoom, flip, channel toggles, range sliders, the histogram and auto-fit range
  all work, and Save Texture as PNG works.
- Step backwards through the events: the back buffer changes with each draw, and the replay is
  unaffected afterwards (drawing the triangle again at the last event looks the same as before
  opening the texture viewer).
- `D3D9_Resource_Contents`: its textures display with the contents its log describes, each mip
  and cube face selectable, volume slices selectable, and textures in every pool it uses display.
- `D3D9_Capture_Edge_Cases` at the first event: the automatic depth stencil shows depth in red with
  pick pixel values 0.25 / stencil 90 (0x4000005A), 0.75 / 165 (0xBFFFFFA5) and 1.0 / 0
  (0xFFFFFF00); each sample of the multisampled one 0.5 / 51 (0x80000033) and 1.0 / 0; D16 about
  0.49999 (0x7FFF); INTZ 0.375 / 15 (0x5FFFFF0F). Min/max over the automatic depth stencil gives
  0.25 to 1.0 for depth.
- A real game with DXT, A8L8/L8, float render targets and a depth stencil: compressed textures
  match the game, float targets show values outside 0-1 with the range slider, depth shows with
  auto-fit. `GetMinMax`/`GetHistogram` and pick pixel agree with what's displayed.
- From Python (see `d3d9_replay_check.py` above): `controller.PickPixel(tex, x, y, sub, CompType.Typeless)`
  returns the same values as the UI, and `GetMinMax`/`GetHistogram` return results instead of
  zeros.
- RenderDoc's log has no "Couldn't decode", "Couldn't copy" or "debug shaders" errors, except for
  the formats listed in the gaps.

### Checking texture formats

`D3D9_Texture_Formats` creates a 16x16 managed texture in each of 48 formats the GPU can sample
(every format `d3d9_pixel.cpp` decodes, apart from depth stencils), fills it with pseudo-random data
(halfs and floats from 1/32 to 2 in magnitude), and draws each one 1:1 with point sampling into an
A32B32G32R32F render target, inside a marker named `<format> at x,y`. Formats the GPU doesn't
support get a `Not supported: <format> (...)` marker instead. On the first frame it writes the
render target to `D3D9_Texture_Formats_reference.txt` in its tmp folder (`util\test\tmp\D3D9_Texture_Formats`,
or `<dir>\D3D9_Texture_Formats` with `--tmp <dir>`; `_reference_rdoc.txt` when it runs under
RenderDoc), which is what the GPU's sampler returned for every texel. The `D3D9_Texture_Formats`
test does all of this.

`util/test/d3d9_formats_check.py capture.rdc [reference.txt]` then compares, for each format:
- RenderDoc's decoding (`PickPixel` on the texture, what the texture viewer shows) with the
  reference. Formats decode to the channels they're described with, which isn't always how D3D9
  samples them (L8 samples as LLL1 and decodes as L001), so the script maps decoded channels to
  sampled ones per format. ATI1 compares red only, and ATI2 red, green and blue: a swapped ATI2 shows as
  `decoded r = GPU g` and `decoded g = GPU r`. P8, A8P8, YUY2 and UYVY aren't compared (palette
  indices, and YUV decoded as Cr, Y, Cb), only printed. The tolerance is 0.01, 0.03 for the block
  formats (NVIDIA's DXT green and BC4/BC5 6-value mode differ from the spec's interpolation by up to
  0.024) and 1e-5 for float formats.
- the render target at replay with the reference, which checks the capture reproduces each
  texture's data and the GPU samples it the same way at replay.

It ends with the passed, failed and uncompared formats, the formats the GPU doesn't support, and
the render target result.

The first run (`d3d9_formats_d3dperf_test.md`, RTX 4070 Ti, 31 formats supported) passed 25 formats
and failed DXT2-5, ATI1 and ATI2, and the render target at replay differed for ATI1 and ATI2. The
three causes above (ATI2's channel order, ATI1/ATI2 lock pitch, DXT2-5's palette) are fixed, and
the demo now writes ATI1/ATI2 rows tightly packed too. The retest (`d3d9_formats_retest.md`)
passed all 31 formats and the render target at replay, and the reference the demo writes under
RenderDoc is byte-identical to the native one. ATI2's blue wasn't compared then; it is now, and
the decode's blue changed from 0 to 1 to match the GPU. The retest of that (same report, section
"ff8a6dd08: ATI2 blue") passed all 31 formats with ATI2's r, g and b compared.

## Pipeline state (phase 3c)

Captures now present themselves as `GraphicsAPI.D3D9` (`IsD3D()` is true for it, so the UI uses
D3D naming), with their own pipeline state, `D3D9Pipe::State`
(`renderdoc/api/replay/d3d9_pipestate.h`, `renderdoc.D3D9State` in Python, from
`ReplayController.GetD3D9PipelineState()` and `CaptureContext.CurD3D9PipelineState()`), and their
own pipeline state panel (`qrenderdoc/Windows/PipelineState/D3D9PipelineStateViewer.*`).

How it works (`d3d9_pipestate.cpp`):
- `SavePipelineState` builds the state from the replay device after the replay controller has
  replayed to the event. Object bindings (textures, streams, index buffer, shaders, vertex
  declaration, render targets, depth stencil) come from the wrapper's tracked bindings, so they
  carry the captured IDs. Everything else is read back from the real device with
  `D3D9FetchDeviceState`, the same function capture uses. A declaration made by `SetFVF` is read
  from the declaration the runtime made for it, and the FVF code is listed too.
- The topology is the `PrimitiveType` of the draw at the event, read from its chunk in the
  structured data, and `Unknown` for events that aren't draws.
- D3D9-specific enums (texture stage operations and arguments, fog modes, declaration types and
  usages...) are strings without their prefix, e.g. `MODULATE`, `TEXTURE | COMPLEMENT`, `FLOAT3`,
  `TEXCOORD`. Generic ones use RenderDoc's enums (CompareFunction, BlendMultiplier...). Front faces
  are clockwise, so `D3DCULL_CCW` is `CullMode.Back` and `D3DCULL_CW` is `CullMode.Front`.
  `renderStates` lists every render state with its raw value.
- Blending: each of the four `blends` has the shared blend state with that target's write mask
  (`D3DRS_COLORWRITEENABLE` to `D3DRS_COLORWRITEENABLE3`). `PipeState.GetColorBlends()` returns
  them as they are, and `PipeState.IsIndependentBlendingEnabled()` is true when the write masks
  differ between targets, so a script that only reads the first entry when it's false doesn't miss
  them.
- The descriptors live in one virtual descriptor store, like D3D11: descriptors 0-20 are the
  samplers (texture and sampler states together) numbered like `D3D9SamplerToSlot` (pixel 0-15,
  DMAP 16, vertex 17-20), 21-26 the shader constants.
- Shader constants are presented as six pseudo buffers, listed as resources: "VS float constants
  (c#)", "VS integer constants (i#)", "VS boolean constants (b#)", and the same for PS. Each holds
  the stage's whole register file from register 0: a float4 per c#, an int4 per i#, a uint32 (0 or
  1) per b#. `GetBufferData` on them reads the replay device's current constants, so
  `GetCBufferVariableContents` works unchanged: the constant block descriptors point at them.
  Given a length of 0 (no data), D3D9's `FillCBufferVariables` reads the same registers from the
  device itself.
  `D3D9Shader.floatConstantsBuffer` etc. give their IDs directly.
- Descriptor accesses (`GetDescriptorAccess`): with reflection (phase 3d), the shader's constant
  blocks map to the pseudo buffer of their register set, which is the block's `fixedBindNumber` (0
  float, 1 integer, 2 boolean), and its samplers and read-only resources to their sampler register
  (`fixedBindNumber`; a SM3 vertex shader's s0-s3 are the vertex texture samplers). Without reflection, every sampler with a texture bound that the stage can use is
  listed with the sampler register as its index. Without a pixel shader, the texture stages up to
  the first disabled one are listed. A texture on the displacement map sampler is listed for the
  vertex stage.

Known gaps:
- Phase 3d provides the reflection. When a shader has none (`GetShader` returns NULL, e.g. its
  bytecode wasn't kept), it can't be viewed or saved from the panel, all bound textures count as used, every vertex element counts as used, and the
  constants rows open the raw registers in the buffer viewer (`float4 c;`, `int4 i;`, `uint b;` per
  row) instead of the shader's variables.
- (Implemented in step 3: `GetUsage`, see "Resource usage".)
- The buffers' sizes are the register counts `D3D9FetchDeviceState` found at load time: the
  device's VS float constants (256 with hardware vertex processing, 8192 for software vertex
  processing devices while in software mode), 224 PS float constants on SM3 hardware, 16 int and
  16 bool (2048 VS ones in software mode). Reading them always returns what the device has at the
  time, so a capture that switches software vertex processing mid-frame can read a different size.
- Shader editing isn't offered (D3D9 can't build replacement shaders yet).
- Remote replay presents `localRenderer` D3D9, which has no proxy texture support, so remote
  replay of D3D9 captures probably won't display textures.
- `SetLOD` on managed textures isn't reflected in the descriptors' first mip.

### Checking the pipeline state

`util/test/d3d9_pipestate_check.py capture.rdc [eventId]` prints the D3D9 pipeline state at every
draw (or only at `eventId`, with every render state too), through both `D3D9State` and the generic
`PipeState`, and reads the constants. Run it like `d3d9_replay_check.py`.

For every constant block a shader uses it also calls `GetCBufferVariableContents` twice, with the
descriptor's `byteSize` and with a length of 0, and prints
`constant block N length 0: same V variables (Z non-zero)` or `MISMATCH` with the differing
variables. The last line should be `GetCBufferVariableContents length 0 matched the descriptor size
everywhere`. A `D3D9_Mesh_Output` capture covers VS float, int (the loop count) and bool blocks.
`D3D9_Simple_Triangle`'s third draw ("Pixel shader constants", in the bottom right) covers PS
ones: its ps_3_0 shader reads c5 = (0.5, 2, 1, 1) and c9 = (0.4, 0.2, 0, 0), loops i0 = (4, 0, 1,
0) times and branches on b3 = TRUE, so the triangle is 96, 191, 0 in the back buffer (191, 96, 0
would mean the bool was read as FALSE, and black that the loop didn't run). fxc won't loop over an int
uniform, so the shader has a literal count of 4 and the demo removes the `defi i0` fxc writes for
it (`CompileLoop` in `d3d9_test.cpp`, which `D3D9_Mesh_Output` uses too); the demo log warns if
there was none. Tested on Windows in `d3d9_ps_constants_retest.md`. A block with 0 non-zero variables
where the application sets values would mean both calls read zeros, so check the counts aren't all
0.

What to look for, in the UI's Pipeline State window and in the script:
- the capture's API shows as D3D9, the Pipeline State window shows the D3D9 panel with IA, VS, RS,
  PS, OM and an isolated "Render States" stage, and nothing errors in RenderDoc's log.
- `D3D9_Simple_Triangle`: at the draw, the topology is TriangleList with the triangle diagram, the
  vertex elements match the demo's declaration (or its FVF, with "From FVF 0x..."), the stream 0
  row shows its vertex buffer with the demo's stride, and double-clicking it opens the buffer viewer
  showing the vertices. The mesh viewer shows the triangle's inputs (PipeState.GetVertexInputs).
- shaders: the VS and PS rows name the shader objects with their shader model (e.g. `vs_3_0`),
  and clicking them opens the disassembly with reflection (phase 3d). The Constants rows list the three register sets (256 float, 16
  integer and 16 boolean registers for a hardware VS, 224/16/16 for a SM3 PS); double-clicking
  the float row opens the buffer viewer, and the values match what the demo set
  (`D3D9_Frame_Start_State` sets c200 and c180, which should show there).
- `D3D9_Frame_Start_State`: the render, sampler and texture stage states, transforms, lights,
  material and clip planes it sets at frame start show up in the matching tables (and in the Render
  States list with their raw values), including D3DTSS_CONSTANT on stage 0.
- textures: the textures bound to sampled slots show in the VS/PS Textures tables with their sizes
  and formats, and in the texture viewer's inputs strip (once 3b displays textures). Double-clicking
  opens them in the texture viewer. Sampler states match what the demo set.
- output merger: render target 0 is the back buffer (or the render target texture, with its mip),
  the depth row the depth stencil, and the blend, alpha test, depth and two-sided stencil states
  match the demo's. The texture viewer's "follow" outputs show the same targets.
- fixed function draws: with no pixel shader, the Texture Stages table shows the enabled stages and
  the texture viewer's inputs strip shows their textures; with no vertex shader the lighting,
  transform and light tables aren't in italics.
- Python: `controller.GetD3D9PipelineState()` is a `renderdoc.D3D9State`, `GetD3D11PipelineState()`
  is None, and `controller.GetPipelineState().GetOutputTargets()`, `GetVertexInputs()`,
  `GetVBuffers()`, `GetIBuffer()` and `GetPrimitiveTopology()` agree with it. In the UI's Python
  shell `pyrenderdoc.CurD3D9PipelineState()` returns the same state.
- the Export button writes an HTML file with a section per stage.

## Shaders (phase 3d)

Each vertex and pixel shader's bytecode is kept as it's created at replay
(`Serialise_CreateVertexShader` and `Serialise_CreatePixelShader` call `D3D9Replay::AddShader`),
and parsed once. Shaders created during the frame are created again on every replay of it; the
first one is kept, since the replay controller holds on to its reflection.

The work is in platform-neutral files with their own token definitions, no Windows headers:
- `d3d9_bytecode.*` - the parser and disassembler. Shader model 2 and 3 instructions store their
  length; 1_x ones don't, so their lengths come from an opcode table (with `tex` and `texcoord`
  taking a source from ps_1_4). Parameter tokens are checked to have their top bit set, which
  catches a wrong length instead of decoding the next instruction as parameters. On bad bytecode
  parsing stops at the first problem and keeps what it got; the disassembly shows those
  instructions and then the error. It handles comments, swizzles, write masks, source modifiers
  (`-`, `1 -`, `!`, `_bias`, `_bx2`, `_x2`, `_dz`, `_dw`, `_abs`), destination shifts and
  modifiers (`_x2`..`_d8`, `_sat`, `_pp`, `_centroid`), predicates, relative addressing (vs_1_1's
  implied `a0.x` and the extra address token in 2_0 and later), `dcl` usages and sampler types,
  `def`/`defi`/`defb`, ps_1_x co-issue (`+`), and the version-specific register names (`a0` or
  `t#` for type 3, `oT#` or `o#` for type 6, `c2048`+ for the extra constant types).
- `d3d9_shader_reflect.*` - the constant table (`CTAB` comment, the D3DXSHADER_CONSTANTTABLE
  layout) and `ShaderInfo`, which builds the ShaderReflection and fills constants. Every offset in
  the table is bounds checked and type nesting is limited, since it's untrusted data.
- `d3d9_shaders.cpp` - the `D3D9Replay` functions: `GetShaderEntryPoints` (always `main`),
  `GetShader`, `GetDisassemblyTargets` (one, "D3D9 Bytecode"), `DisassembleShader` and
  `FillCBufferVariables`.

Shaders have their own encoding, `ShaderEncoding::D3D9Bytecode`.

The disassembly is in the fxc / D3DXDisassembleShader style: the constant table as fxc prints it
("Generated by", "Parameters", "Registers"), then the version and one instruction per line,
indented 4 spaces and 2 more inside `if`, `loop` and `rep`, with a co-issued instruction's `+` two
columns to the left (`  + mov r0.w, ...`). As fxc prints them, source swizzles have one letter when
they replicate a component (`.xxxx` is `.x`) and all four otherwise (`.xyww`), ps_1_4's `t#`
sources included (the documented `.xyz` and `.xyw` are `.xyzz` and `.xyww`). The complement
modifier is `1-v0.w`. `dcl` leaves off usage index 0 (`dcl_texcoord`), relatively
addressed registers are `c4[a0.x]` and `v0[aL]`, and `def` floats have 9 significant digits.

Reflection, which is what phase 3c's pipeline state builds on:
- Constants are up to three constant blocks, only for the register sets the shader uses, in this
  order: "Float constants (c#)", "Integer constants (i#)", "Boolean constants (b#)". Each block's
  `fixedBindNumber` is its set (0 float, 1 integer, 2 boolean, `D3D9BC::ConstantSet`) and
  `bufferBacked` is false. Variables are at register * 16 bytes for float and integer registers and
  register * 4 for booleans, and `FillCBufferVariables` takes that set's whole register file from
  register 0 (float4s, int4s, or a uint32 0/1 per boolean). The shader's `def`, `defi` and `defb`
  literals are applied on top of the data, since the shader reads them rather than the device's
  constants, and padded out if the data is shorter. The blocks aren't buffer backed, so the buffer
  viewer shows these variables as `FillCBufferVariables` returns them, without byte offsets, and
  doesn't offer a custom format for them; D3D11's constant buffer packing it picks for D3D APIs
  isn't used to lay them out.
- With a constant table, variables have their HLSL names and types: structs, arrays, matrices
  (`row_major` ones are row major, otherwise each register is a column), and default values for
  ones of up to 64 bits. A variable's base type is what its register set holds, so an `int` or
  `bool` the compiler put in `c#` registers is a float. Arrays are cut to the registers the
  compiler allocated (`RegisterCount`), as the registers after the last one used can belong to
  something else.
- Literals, and registers the code reads that the table doesn't describe, are variables named by
  register (`c30`, `i0`, `b7`). Without a table that's every constant. A relatively addressed
  constant without a table is an array from its base register up to the next register used for
  something else, or the last register.
- Input and output signatures come from `dcl` (vs inputs, vs_3_0 outputs, ps_3_0 inputs including
  `VPOS` and `VFACE`) or from the registers used: before vs_3_0 `oPos`, `oFog`, `oPts`, `oD#` and
  `oT#` are POSITION, FOG, PSIZE, COLOR# and TEXCOORD#; ps_2_x's `v#` and `t#` are COLOR# and
  TEXCOORD#; ps_1_x's texture coordinates are the `t#` its texture instructions fill (or read, in
  ps_1_4), and its output is r0 as COLOR0. `oC#` and `oDepth` are COLOR# and DEPTH. Semantic names
  are D3D9's (POSITION, TEXCOORD, COLOR...), `regIndex` is the register number.
- Samplers are in `samplers` and in `readOnlyResources` (ImageSampler), with `fixedBindNumber` the
  sampler register and the constant table's name and array size, or `s#`. The texture type comes
  from the constant table or `dcl_2d`/`dcl_cube`/`dcl_volume`. ps_1_x has neither without a table,
  so its samplers are 2D, or cubes for `texm3x3tex`, `texm3x3spec` and `texm3x3vspec`.

Known gaps:
- Unverified against fxc, since there's none here (see below): instruction modifier order
  (`mul_x2_sat`, `mov_sat_pp`), `_abs` and other source modifiers before the swizzle
  (`r0_bx2.x`), `dcl_texcoord1_centroid`, and how relatively addressed registers look in ps_3_0
  and vs_3_0 outputs.
- The constant table's default values are read as the type's components packed together, as Wine
  does; that hasn't been checked against a real table, nor have integer and boolean defaults.
- Without a constant table, how far a relatively addressed constant array goes is a guess.
- No shader debugging, editing (`BuildTargetShader`) or source-level debug info.
  `GetTargetShaderEncodings` is empty and the "Edit Generated Stub" option is left out for
  D3D9 bytecode. AMD's GCN ISA disassembly isn't offered either: AMD's plugins only take
  DXBC/DXIL, SPIR-V and GLSL.
- `channelUsedMask` is approximate for instructions that aren't per component.

Tested: the unit tests in `d3d9_bytecode.cpp` and `d3d9_shader_reflect.cpp` (Catch, `[d3d9]`)
pass on Linux with AddressSanitizer and UBSan, built with `util/test/d3d9_shader_unit_tests.sh`.
They use hand-assembled shaders, since there's no SM1-3 compiler on Linux: every shader model
(vs_1_1, vs_2_0, vs_2_x, vs_3_0, ps_1_1, ps_1_3, ps_1_4, ps_2_0, ps_2_x, ps_3_0) with the
instructions and modifiers above, constant tables with structs, arrays, both matrix orders, every
register set and samplers, filling constants with literals, the signatures for each model, and
malformed input: empty and truncated shaders, bad versions, unknown opcodes, wrong lengths,
missing parameter bits, truncated constant tables, out of range offsets, a type containing itself,
and thousands of random shaders and tables. On Windows they run with
`renderdoccmd test unit "[d3d9]"` from a Development build. The Windows files were syntax checked
with clang and the MinGW headers.

### Checking shaders on Windows

To check, with the demos under `util/test/demos/d3d9/`:
- Capture `D3D9_Simple_Triangle` and open it. In the pipeline state (once 3c is in) or with the
  Python API (`controller.GetShader`, `controller.DisassembleShader`), the vertex and pixel shaders
  should have reflection: `POSITION`, `COLOR` and `TEXCOORD` inputs for the vertex shader, matching
  the vertex declaration, and `COLOR0` output for the pixel shader.
- Compare the disassembly with fxc's. Save the demo's shader source as `vs.hlsl` and run
  `fxc /T vs_3_0 /E main /Fc vs.asm vs.hlsl` (and the same with `ps_3_0`); or call
  `D3DXDisassembleShader` on the bytecode. Instructions, operands and modifiers should match line
  for line (fxc's comments and blank lines aside). The constant table part only appears for
  shaders with uniforms; `D3D9_Capture_Edge_Cases`'s depth shaders and the other demos' shaders are
  worth comparing too, and any game with ps_1_x or ps_2_0 shaders. Note any differences in the
  gaps listed above.
- `renderdoccmd test unit "[d3d9]"` passes.
- A shader with uniforms (`float4x4`, an array, a `row_major` matrix, a `bool`, a sampler) shows
  them by name in the constant blocks, with values matching what the application set, and `def`
  constants with their literal values.
- RenderDoc's log has no "Couldn't fully parse shader" warnings for real shaders.

## Overlays (phase 3e)

`d3d9_overlay.cpp`: `RenderOverlay`, `IsRenderOutput` and `GetPassEvents`. Each overlay means the
same and uses the same colours as D3D11's (`d3d11_overlay.cpp`).

How it works:
- The overlay texture is an A8R8G8B8 render target texture created through the wrapped device, so
  it has a ResourceId and `GetTexture`, `GetDisplaySource`, `PickPixel` and `GetTextureData` treat
  it like any other texture. It has the viewed target's size and mip count, and is a cube texture
  when the target is a cube face, so it lines up with the subresource the texture viewer shows.
  It's recreated when those change, and released at shutdown or when the device stops working
  (`FatalErrorCheck`). Every subresource is cleared to transparent before each overlay, and pick
  pixel's decoded copy of it is dropped.
- `IsRenderOutput` (which was always false, so no overlay was ever asked for) is true for the
  textures bound as render targets or the depth stencil at the event. `GetPassEvents` returned
  only the event; it's now D3D11's: the draws before the event back to the last clear, or to
  where the targets change.
- The replay controller calls `RenderOverlay` with the frame replayed up to the event without its
  draw, and replays the draw after it (as for D3D11). The draw overlays save the state
  (`SaveReplayDrawState`), bind the overlay as render target 0 (1-3 unbound) with the
  application's depth stencil, and replay the draw (`eReplay_OnlyDraw`) once or twice with a
  constant colour pixel shader and some render states changed. Every pass has ZWRITEENABLE FALSE,
  a stencil write mask of 0 and KEEP stencil ops, so the depth stencil isn't touched and every
  test sees what the draw saw. Alpha test, blending, fog and sRGB writes are off and
  COLORWRITEENABLE is 0xF. Then the state is restored, so the controller's replay of the draw
  does what it did. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` replay the same way, from the
  vertices in their chunk.
  - Drawcall: (0.8, 0.1, 0.8) over a (0, 0, 0, 0.5) background, solid, with no culling, depth,
    stencil or scissor.
  - Wireframe: D3DFILL_WIREFRAME, no culling, depth or stencil, the application's scissor, in
    (200, 255, 0) over the same colour transparent.
  - Depth: the draw with depth, stencil and culling off in red, then with the application's
    ZENABLE, ZFUNC, depth bias and cull mode (stencil still off) in green. Without a depth
    stencil, or with ZENABLE off, everything passes.
  - Stencil: the same, with depth off and the application's stencil states (two-sided too) in the
    green pass.
  - BackfaceCull: CULL_NONE in red, then the application's cull mode in green.
  - ViewportScissor: the draw without the scissor test in red, then with it in green, then D3D11's
    rectangles alpha blended over them: the viewport filled with (0.2, 0.2, 0.9, 0.4) inside a
    3 pixel (0.1, 0.1, 0.1) border, and, if the scissor test is on, the scissor rect's black and
    white checkered border. They're drawn with a vs_3_0 quad and a ps_3_0 shader reading VPOS.
- D3D9 only links ps_3_0 with vs_3_0, so the constant colour shader is ps_3_0 when the draw's
  vertex shader is vs_3_0 and ps_2_0 otherwise (fixed function, vs_1_1 to vs_2_x).
- Multisampled targets: the depth stencil must match the render target's multisample type, so
  the passes draw into a multisampled A8R8G8B8 render target of the depth stencil's type (or the
  target's, without one), which is resolved into the overlay texture with StretchRect.
- ClearBeforeDraw clears every bound render target to the background colour (all of it: the
  viewport and scissor test are set aside for the Clear) just before the draw. ClearBeforePass
  replays up to the pass's first draw, clears there, and replays the pass's draws and what's
  between them, up to the event without its draw.
  The depth (and stencil) is cleared too when the viewed texture is the depth stencil: to 1.0 for
  LESS and LESSEQUAL, 0.0 for the others, and not at all for EQUAL, NOTEQUAL and ALWAYS, as D3D11
  does. These change the application's targets, as on the other APIs; the replay controller
  replays again when they're turned off. The overlay texture stays transparent.
- At events that aren't draws only the overlay's background is drawn.
- Triangle size (draw and pass): D3D11 works out each triangle's area in a geometry shader, which
  D3D9 doesn't have, so it's done on the CPU from the post-transform vertices (`GetPostVSBuffers`,
  see the mesh output below). For each instance of the draw, or of each draw in the pass, the
  vertices are expanded to a triangle list (strips and fans too, keeping their winding; points and
  lines have no triangles and are left out, as D3D11's geometry shader leaves them out), and each
  vertex gets its triangle's area: D3D11's, after the perspective divide, in pixels of the event's
  viewport. Pretransformed (XYZRHW) vertices are first turned back into the clip space positions
  the viewport maps to them. They're drawn from a dynamic vertex buffer with a vs_3_0 shader that
  passes the position and area through and a ps_3_0 shader that maps the area through D3D11's
  colour ramp (the texture display's `HEATMAP_TRISIZE`). The draw keeps the application's fill
  mode, cull mode, depth bias, scissor test, multisample antialiasing and depth and stencil tests,
  as D3D11 keeps its rasterizer and depth stencil states, with depth and stencil writes off.
  Everything else is the debug draws' state (`D3D9SetDebugDrawState`); the state is saved and
  restored around it as for the other overlays. The pass replays up to its first draw and then
  each draw and what's between them, as ClearBeforePass does, drawing the triangle sizes at each
  draw before replaying it, so each draw's depth test sees the depth buffer as it was. A draw
  without post-transform data (or no positions) is left out. Multisampled targets draw into the
  multisampled render target and are resolved once at the end.
- Quad overdraw (draw and pass) needs UAVs, which D3D9 doesn't have. It returns the overlay texture
  cleared to transparent, with a warning (once), so the target shows as it is. Returning no
  texture, as it did before, left the texture viewer showing the previous overlay.
- At an event between BeginStateBlock and EndStateBlock nothing is drawn and no overlay is
  returned (`SaveReplayDrawState` warns once), as in the texture viewer.

Known gaps:
- Quad overdraw isn't supported (see above).
- The triangle sizes come from the CPU post-transform data, so a draw whose vertex shader the
  interpreter can't run, or a very big draw, shows nothing or takes a while (the mesh output's
  known gaps apply). Positions that differ from the GPU's in the last bits can change which
  pixels on a triangle's edge are covered.
- The constant colour shader replaces the application's pixel shader, so pixels it would discard
  (texkill, clip) or whose depth it would change (oDepth) show as the rasterised primitive passes
  or fails. D3D11 handles oDepth with an extra stencil pass on a copy of the depth stencil; there's
  no copy here.
- Alpha test is off in the overlay passes, so alpha tested pixels show as drawn.
- D3D9 has no depth clip control, so geometry clipped by the near and far planes isn't shown the
  way D3D11 shows it.
- A depth stencil smaller than the target (which D3D9 doesn't allow anyway) is left out, so the
  tests show as passing, with a warning.
- Old hardware that needs a depth stencil of the same bit depth as the render target
  (`CheckDepthStencilMatch`) may refuse the A8R8G8B8 overlay with a 16-bit depth stencil.
- ClearBeforePass replays the frame from the start up to the pass's first draw, on top of the
  controller's replays.

Decisions:
- The depth stencil isn't copied: the controller has already replayed up to the event without the
  draw, and the overlay passes draw with depth and stencil writes off. That's what the draw tests
  against, and nothing has to copy D3D9 depth stencils, which mostly can't be copied or locked.
  (The task's design said to replay without the draw first; the controller already does that,
  and replays the draw afterwards, so the overlay doesn't replay either itself. Replaying the draw
  in the overlay too would apply it twice, which matters for blending and stencil increments.)
- Unlike D3D11, ClearBeforeDraw and ClearBeforePass don't replay the event's draw themselves,
  for the same reason: D3D11 ends up drawing it twice.
- The overlay texture is created through the wrapper (`WrappedD3DDevice9::CreateTexture`) rather
  than registered by hand, so it gets a ResourceId and every existing path (display, pick pixel,
  texture data, save) works without special cases. It isn't added to the resource list.
- It's A8R8G8B8, which every D3D9 GPU renders to and multisamples, rather than D3D11's
  R16G16B16A16_FLOAT. The overlay colours don't need more precision.
- It has the target's mips and is a cube for cube faces, so the subresource the texture viewer
  shows lines up. Volume textures can't be render targets in D3D9.
- Quad overdraw returns a transparent overlay rather than no texture: with no texture the texture
  viewer kept showing the previous overlay, which looked like a working one. There's no API
  property for which overlays are supported.
- The triangle size overlay holds the ramp's colours, not the areas D3D11's holds (an
  R16G16B16A16_FLOAT texture its texture display maps through the ramp). D3D9's texture display
  has no heatmap mode, and the overlay texture is A8R8G8B8, which the ramp's colours fit exactly.
  So picking a pixel of the overlay gives its colour, not the area.
- Each triangle's area is worked out on the CPU and put in a vertex attribute of a non-indexed
  triangle list, rather than with a geometry shader. The ramp is still done in the pixel shader,
  so its rounding is the GPU's, as on D3D11.
- The triangle size draw keeps the application's rasterizer states (fill and cull mode, depth
  bias, scissor test) as well as the depth and stencil tests, because D3D11 does: it changes only
  the shaders, input layout, blend state and targets, and turns off depth and stencil writes.
  User clip planes are off: D3D11's mesh vertex shader doesn't output clip distances.
- The area uses the viewport at each event, where D3D11 takes the viewport of the pass's first
  event for every draw in it.
- `GetPassEvents` and `IsRenderOutput` moved from `d3d9_replay.cpp` to `d3d9_overlay.cpp`, and
  `GetPassEvents` is D3D11's. The mesh viewer's "whole pass" uses it too.
- `InitPostVSBuffers` for a pass read only the last of the pass's draws, with the device at the
  current event, so it cached the current event's inputs for that draw, and the others had no
  data. It now replays to each draw that isn't cached, as D3D11's does. The triangle size pass and
  the mesh viewer's "whole pass" need it.
- The clear before overlays use `Clear` (all bound render targets) with the viewport set to render
  target 0 and the scissor test off, since D3D9's Clear is limited by both.
- The draw is replayed for each overlay pass into our target only, rather than saving and
  restoring the application's targets: the application's targets and depth stencil are never
  written by the passes.
- The viewport and scissor rectangles use VPOS, which is the pixel's integer position in D3D9
  (D3D11's SV_Position is its centre), so 0.5 is added to match D3D11's borders.
- The shaders are HLSL compiled with D3DCompile on first use, like the texture viewer's
  (`D3D9CompileDebugShader` exposes its compile helper). The overlays need shader model 3
  hardware, like the texture viewer.

### Checking the overlays

Capture `D3D9_Overlay_Test` and run `util/test/d3d9_overlay_check.py capture.rdc` like
`d3d9_replay_check.py`. The demo has a new "Triangle Size" marker, so captures made before it
fail those checks: capture it again. The script renders overlays at each of the demo's markers
through a headless ReplayOutput, as the texture viewer does, saves each overlay as
`<capture>_<marker>_<overlay>.png`, and prints PASS or FAIL per check, then the totals. What it
checks (the demo's layout is at the top of `d3d9_overlay_test.cpp`):
- each overlay texture exists, has the target's size, and has the expected colours at known
  pixels: Drawcall magenta inside and dark outside; Wireframe's edge; Depth red where the test
  fails and green where it passes, for a vs_3_0 draw ("Depth Test", ps_3_0 overlay shader) and a
  fixed function `DrawPrimitiveUP` draw that only fails because of its depth bias ("Depth Bias",
  ps_2_0); Stencil green and red; the kept and culled triangles of "Backface Cull"; the
  viewport and scissor colours and borders; and a 4x MSAA target ("MSAA").
- the quad overdraw overlays return a texture that's transparent everywhere.
- triangle size: the ramp's colour for the triangle's area (computed in the script with D3D11's
  formula) at known pixels. "Depth Test": the big quad's colour only where it passes the depth
  test with TriangleSizeDraw, and the setup draw's in the top half too with TriangleSizePass.
  "Viewport Scissor": inside the scissor rect, nothing where it's scissored out or outside the
  viewport. "Triangle Size": a pretransformed triangle strip whose triangles are 10, 24 and 7
  pixels, the ramp's entries 2, 1 and 3; the covered pixels match the Drawcall overlay's (3
  pixels may differ, on edges) and all have ramp colours; TriangleSizePass also has the
  "Depth Test" quad. "MSAA": inside the triangle.
- the target is the same after rendering overlays as before.
- ClearBeforeDraw and ClearBeforePass change the back buffer (to the blue background colour where
  nothing in the draw or pass drew), and turning them off puts it back.

Also check in the UI, at the same events: each overlay in the texture viewer's list shows over the
target as on D3D11 captures; quad overdraw shows the target with no overlay (not the previous
one); triangle size and triangle size (pass) show the ramp's colours (dark purple for the big
triangles; at "Triangle Size", zoomed in, the strip's three triangles in violet, dark purple
and blue), compared with a D3D11 capture if there's one to hand; switching overlays and events
doesn't change the target, the pipeline state or what later events show; Save Texture's "save
overlay instead" writes the overlay; and RenderDoc's log has no errors (one warning for quad
overdraw is expected). If a check fails, the saved PNGs show what was drawn.

## Mesh output (phase 3e)

The mesh viewer's VS Out stage and Python's `GetPostVSData` work for D3D9. D3D9 has no stream
out, so the post-transform vertices are computed on the CPU.

How it works (`d3d9_postvs.cpp`, the comment at its top has more):
- `InitPostVSBuffers` reads what the draw needs from the replay device at the event: the vertex
  declaration (or the one the runtime made for the FVF), the streams' contents, strides and
  frequencies, the indices, and either the vertex shader with its float, integer and boolean
  constants (the shader's `def`/`defi`/`defb` literals override them, as in 3d's reflection) and
  its vertex textures, or the fixed function state. That snapshot is kept per event.
- `GetPostVSBuffers` computes an instance's vertices from it the first time they're asked for,
  and caches them. Programmable vertex shaders run in an interpreter (`d3d9_vs_emulate.cpp`) on
  the program 3d's parser makes; fixed function vertex processing is emulated
  (`d3d9_ffp_vertex.cpp`). Both are portable files with their own state structs, filled from the
  device in `d3d9_postvs.cpp`.
- The results are pseudo buffers (IDs with nothing behind them, like 3c's constants buffers) that
  `GetBufferData` reads: a vertex buffer per instance and, for indexed draws, a 32-bit index
  buffer into it. An indexed draw's unique vertices (index + BaseVertexIndex) are each computed
  once, in sorted order, and the index buffer maps the draw's indices to them. Rows are laid out
  as the mesh viewer expects VS Out columns: the position first, then the rest of the vertex
  shader reflection's output signature in order, each `compCount` floats, packed.
- The position is clip space (oPos, or the fixed function projection) and the MeshFormat asks to
  unproject it, with near and far planes estimated as D3D11 does. Pretransformed (XYZRHW)
  vertices are output as they were given (`POSITIONT`, still the position system value), with
  `unproject` false, like any other non-projected data.
- Fixed function vertex processing has no reflection, so the pipeline state's vertex shader
  `reflection` at a fixed function draw is one made up from its outputs (its `resourceId` stays
  null, so the D3D9 panel still says fixed function, and View/Save stay disabled): `POSITION`
  (`POSITIONT` for pretransformed vertices), `COLOR0`, `COLOR1`, `TEXCOORDn` for each texture
  stage that has a coordinate (its size is the input's, 3 for generated ones, 2 for sphere maps,
  or the `D3DTTFF_COUNTn` count), `FOG` with vertex fog, `PSIZE` for point lists or vertices with
  a size. The input signature lists the declaration's elements. The replay controller and the mesh
  viewer take the VS Out columns from the reflection, so this is how they see them.
- `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP`: their data is only in the chunk and the runtime
  unbinds stream 0 (and the indices) after them, so the device keeps a copy of each one's vertices
  and indices at load (`WrappedD3DDevice9::GetUserPrimitives`). Post-transform data reads that,
  and the pipeline state presents it as two pseudo buffers listed as resources,
  "DrawPrimitiveUP vertices" and "DrawIndexedPrimitiveUP indices", holding the data of the event
  the pipeline state is at. So VS In works for them too.
- `PickVertex` runs on the CPU with the same tests as D3D11's picking compute shader: a ray cast
  against triangles (unprojected through the guessed projection for VS Out), the vertex of the hit
  triangle closest to the hit, or for points and lines the closest vertex within 35 pixels.
- `RenderMesh` draws VS Out like VS In: the positions are read from the pseudo buffers through
  `GetBufferData`, and its existing unproject path handles the rest.
- Caching: the snapshots and results stay until `ClearReplayCache`, not cleared by every replay
  as the texture caches are: a draw's post-transform data can't change.

The interpreter covers every vs_1_1 to vs_3_0 instruction: arithmetic, `lit`/`dst`/`exp`/`log`/
`expp`/`logp`/`frc`/`sincos`/`nrm`/`pow`/`crs`/`sgn`/`lrp`, the `m4x4` family, `if`/`ifc`/`else`,
`loop` (with `aL`) and `rep`, `break`/`breakc`/`breakp`, `call`/`callnz`/`ret`/`label`, `setp` and
predicated instructions, relative addressing by `a0` and `aL` on constants, inputs and vs_3_0
outputs, source and destination modifiers, and `texldl`.

Known gaps:
- `texldl` point samples the nearest mip level to the LOD (plus `D3DSAMP_MIPMAPLODBIAS`, from
  `D3DSAMP_MAXMIPLEVEL` down), whatever the sampler's filters are, with the wrap, mirror, clamp,
  border and mirror once address modes. Linear filtering isn't emulated. The texture is decoded
  with phase 3b's `GetDecoded`, so it works for the formats that can display.
- Shaders run at full fp32 precision; partial precision hints are ignored, except where the
  documentation defines a reduced result (`expp`, `logp`). Transcendentals use the C library, so
  results can differ from a GPU's in the last bits.
- Stream source frequencies only apply to indexed draws (hardware instancing). The divider mode
  of non-indexed draws with software vertex processing isn't used.
- Fixed function: N-patches, `D3DRS_CLIPPLANEENABLE` (user clip planes don't change vertices),
  `D3DDECLMETHOD` other than default, displacement mapping, and more than 8 enabled lights aren't
  handled. Vertex blending uses the world matrices `D3DTS_WORLDMATRIX(0-255)`.
- A shader the interpreter can't run (malformed, or a pixel shader instruction) gives no VS Out
  data, with the reason in the MeshFormat's `status` and the log.
- Big draws with long shaders are slow, as each vertex is interpreted. Results are cached.
- Outputs follow the reflection's output signature, which has one element per register: a vs_3_0
  shader that packs two semantics into one `o#` register (`dcl_texcoord0 o1.xy`,
  `dcl_texcoord1 o1.zw`) shows only one of them.
- The made up fixed function reflections are kept until the replay closes (the replay controller
  holds pointers to them), one per distinct output layout.

### Decisions

- Post-transform data is computed on the CPU rather than with ProcessVertices or by running the
  draw on the GPU: D3D9 has no stream out, ProcessVertices only works on software vertex
  processing devices, gives screen space and can't do vs_3_0 instancing, and the CPU path can be
  unit tested.
- Scalar instructions (`rcp`, `rsq`, `exp`, `log`, `expp`, `logp`, `pow`, `sincos`) read the w of
  the swizzled source. SM2 and later require a replicate swizzle, so it only matters for vs_1_1,
  where the documentation says w is used when no component is given.
- `log` and `logp` of 0 give -FLT_MAX (the documentation's MINUS_MAX), `rcp`/`rsq` of 0 +inf, of 1
  exactly 1. `nrm` of a zero vector gives 0 instead of NaN (the documentation scales by FLT_MAX).
- vs_2_0's partial precision `expp`/`logp` are cut like vs_1_1's z (low 8 mantissa bits cleared).
- mova rounds to nearest with floor(x + 0.5); vs_1_1's `mov a0` uses floor. a0 is clamped to
  +-65536 before converting, and NaN becomes 0.
- Out of range reads (relatively addressed constants, inputs, unbound vertex samplers) give 0 (or
  (0, 0, 0, 1) for samplers and missing vertex elements, as the input assembler fills them).
  Out of range writes are dropped.
- Unwritten output components are 0.
- An instruction budget of 2^20 per vertex and a call depth of 16 stop runaway shaders, which then
  report an error instead of data.
- The fixed function pipeline's direction to the viewer without D3DRS_LOCALVIEWER is (0, 0, -1)
  in camera space (the camera looks down +z, and Wine does the same). The documentation writes
  (0, 0, 1), but ProcessVertices agrees with (0, 0, -1): the phase 3e test's camera space
  reflection vectors matched it. It's one constant, `NonLocalViewer` in `d3d9_ffp_vertex.cpp`.
- With no pixel shader, only the texture stages before the first one whose D3DTSS_COLOROP is
  D3DTOP_DISABLE output a texture coordinate. ProcessVertices does this (the phase 3e test showed
  no coordinates for disabled stages that had texgen set), and so does Wine. With a pixel shader,
  every stage can.
- Sphere map coordinates are r / (2 |r - (0, 0, 1)|) + 0.5 with r the reflection vector, which
  is n.xy / 2 + 0.5 without the local viewer. ProcessVertices gave exactly that in the phase 3e
  retest. Wine adds (0, 0, 1) instead, as OpenGL does with its camera looking down -z. With the
  local viewer it's the same formula, which isn't tested.
- Specular is only added where N.L > 0, and only with D3DRS_SPECULARENABLE (COLOR1 is (0, 0, 0)
  otherwise). COLOR1's alpha is the specular source's alpha when lit, the vertex's otherwise. Fog
  is its own `FOG` output, not COLOR1's alpha.
- Texture coordinates get a 1 after their last component before the texture transform (so a 2D
  coordinate's translation is the matrix's third row), as Wine does. `D3DTTFF_PROJECTED` doesn't
  change the vertex's output: the divide happens per pixel.
- Without normals, lighting uses a zero normal (only ambient and emissive light the vertex).
  Missing diffuse is white, missing specular black.
- Vertex fog uses camera space |z|, or the distance with D3DRS_RANGEFOGENABLE.
- `PSIZE` is output for point lists and for vertices that have a size.
- The spot light factor isn't clamped to 1 inside the inner cone (cos of the angle to the spot
  direction above cos(Theta / 2)), although the documentation says it's 1 there. The falloff
  formula keeps applying, so it's above 1 towards the axis. ProcessVertices and the GPU (an
  RTX 4070 Ti, in the phase 3e retest) both do this: the vertex inside the inner cone came out
  about 1.13 times brighter in both.
- Blend indices in D3DCOLOR or UBYTE4N elements are scaled back by 255.
- The pretransformed position keeps the system value Position, so the mesh viewer still finds it,
  but its semantic is POSITIONT and the MeshFormat doesn't unproject it.
- Post-transform vertex buffers are one pseudo buffer per instance, computed on demand, rather
  than all instances in one buffer like D3D11, since computing every instance up front on the
  CPU can be slow.
- The DrawPrimitiveUP pseudo buffers hold the data of the event the pipeline state is at (like
  the constants buffers), so there are only two resources rather than two per draw.
- `PickVertex` doesn't divide by w for data that isn't unprojected, as D3D11's and Vulkan's
  picking shaders don't.

### Testing the mesh output

Unit tests: `util/test/d3d9_shader_unit_tests.sh` builds and runs the `[d3d9]` tests, now
including `[vsemu]` (the interpreter per opcode, flow control, relative addressing, texldl,
vertex fetch, whole draws) and `[ffp]` (transforms, blending, each light type, material sources,
texgen, texture transforms, fog, point size, the output signature). They pass on Linux with
AddressSanitizer and UBSan. On Windows: `renderdoccmd test unit "[d3d9]"`.

On Windows, with `D3D9_Mesh_Output` (in the demos project):
The `D3D9_Mesh_Output` test does steps 1 and 2.

1. Run it natively once: before drawing it writes `D3D9_Mesh_Output_reference.txt` in its tmp
   folder (`util\test\tmp\D3D9_Mesh_Output`, or `<dir>\D3D9_Mesh_Output` with `--tmp <dir>`;
   `_reference_rdoc.txt` under RenderDoc), and logs where. Its references come
   from `ProcessVertices` on a second, software vertex processing device for the fixed function,
   vs_1_1 and vs_2_0 draws (screen space), and from C++ for pretransformed vertices, vs_3_0 with
   texldl and instancing (clip space). Check its log for "Couldn't" or "ProcessVertices failed"
   warnings: a draw whose reference failed isn't checked, and counts as a failure.
2. Capture a frame and run
   `python util\test\d3d9_mesh_check.py capture.rdc path\to\D3D9_Mesh_Output_reference.txt`
   (set up as for `d3d9_replay_check.py`). It prints PASS or FAIL per draw (with the first
   mismatches), the VS In and VS Out vertex counts, and checks VS In of the DrawPrimitiveUP draws.
   Every draw should pass.
3. In the UI, at each draw: the mesh viewer's VS Out tab shows columns matching the shader's
   outputs (or the fixed function ones), the preview shows the transformed mesh in the guessed
   frustum, and picking a vertex in the preview selects a row. The DrawPrimitiveUP draws show VS
   In data now. The Pipeline State panel still says "Fixed function vertex processing" for the
   fixed function draws.
4. Things to watch for, which the decisions above flag as unverified: specular and reflection
   vectors without the local viewer ("FFP directional light", "FFP texgen and fog"), sphere maps,
   texture transforms on 2D coordinates, vs_1_1's `expp`/`logp` precision, and whether
   ProcessVertices writes the vertex fog to specular alpha. If a fixed function draw fails only in
   one of those outputs, the decision is probably wrong; say which values came out. The indexed
   vertex blending draw only exists if the GPU supports indexed blending with hardware vertex
   processing (`MaxVertexBlendMatrixIndex` >= 7).

### Phase 3e test results

From `d3d9_phase3e_test.md`. The UI (VS Out columns, the preview, vertex picking) works, and these
draws matched: FFP transform, the pretransformed ones, directional and point lights, vertex
blending, both DrawPrimitiveUP draws' VS In, and vs_3_0 instancing. What failed, and what changed:
- vs_2_0 flow control, vs_2_0 DrawPrimitiveUP and vs_3_0 loops and texldl: their shaders didn't
  compile (X4509 on `int count : register(i0)`), so the draws ran without a shader. The loop
  counter was used in arithmetic, which neither aL nor a rep counter can do, so fxc wanted a float
  copy of `count` too. The shaders now only count with it. The demo also leaves out a draw whose
  shader doesn't compile, with an error, instead of drawing it with fixed function.
- vs_1_1: the hand assembled tokens were rejected by CreateVertexShader. `expp`, `logp`, `lit`
  and `dst` now write temporaries that are moved to the outputs, and an instruction separates
  writing a0 from reading it. If it's still rejected, the demo logs the first instruction the
  runtime rejects, by trying the shader cut short at each instruction.
- FFP spot light: one vertex, inside the inner cone, differed: ProcessVertices doesn't clamp the
  spot factor to 1 there. Which one the GPU does is what matters, so the demo now also draws each
  vertex of the lit draws on its own, as a 1x1 point, and reads back its diffuse colour, diffuse
  alpha and specular colour (the `hw` lines in the reference). The check compares ours with those,
  and only notes where ProcessVertices differs from the GPU.
- FFP texgen and fog: ProcessVertices gave no coordinates for stages 1-4, which were disabled.
  RenderDoc now does the same (see the decisions above), and the demo enables stages 1-4 so their
  coordinates are tested.
- FFP point size: ProcessVertices doesn't write the point size. The demo now measures each point's
  size as the GPU draws it, and its sizes are between the clamps (about 8 to 16, clamped to 9 and
  14) instead of all at the maximum.

The retest (`d3d9_phase3e_retest.md`) passed the overlays, the GPU readback of the spot light and
point sizes, and the texgen stages other than the sphere map. What failed, and what changed:
- vs_1_1 was rejected at `mov oFog, v2.z`, with the .x write mask. It now has the full mask, as
  fxc writes oFog.
- vs_2_0 flow control and vs_3_0 loops still didn't compile: fxc rejects a loop over
  `int count : register(i0)` however it's used. The loops now have a literal count, and the demo
  removes the `defi` fxc writes for it, so the count still comes from SetVertexShaderConstantI
  (the same 3 and 5). It warns if there's no `defi i0` to remove.
- The vertex blending draws' 1x1 readback points were 9x9: the "FFP point size" case's
  D3DRS_POINTSIZE_MIN was never reset. Every case now starts with the default point size states.
- FFP spot light: the GPU doesn't clamp the spot factor either, so RenderDoc no longer does (see
  the decisions above).
- FFP texgen and fog: the sphere map used Wine's |r + (0, 0, 1)|; it's |r - (0, 0, 1)| now.
- The mesh viewer showed a GS/DS Output table and GS/DS Out preview tab. They're hidden for D3D9
  captures (and come back for other APIs).

Retest 2 (`d3d9_phase3e_retest2.md`, of 493802b34) passed everything except the Event Browser
rows: the demo log is clean, `d3d9_mesh_check.py` passes all 18 checks, the GS/DS table and tab
are hidden for D3D9 and back for D3D11, the unit tests pass, and the overlay and replay checks
match the previous round. The rows are covered under "Event Browser rows" above, and retest 3
(`d3d9_phase3e_retest3.md`, of e409786bd) passed all of them. Retest 4 (of 22ba5031c) checked
the icon sizes and failed on the 9 pixel icon and the resource cells' underline and hover area,
both fixed in c1049db4f, which retest 5 passed.

## Inspecting a capture

Captures can also be dumped without replaying them:

```
renderdoccmd convert -f capture.rdc -o capture.zip.xml -c zip.xml
```

The XML has every chunk with its parameters: how each referenced object was created, the contents
of each referenced object when the frame started (`InitialContents`), the device state at the start
of the frame (in `BeginCaptureFrame`), and the calls made during the frame, including the data each
Unlock wrote. With `-c zip.xml` the contents are stored as files in the zip next to the XML.

## Code layout

- `d3d9_hooks.cpp` - hooks Direct3DCreate9/Ex. The D3DPERF hooks live in `driver/d3d11/d3d9_hooks.cpp`
  and pass the markers on to D3D9 devices (`WrappedD3DDevice9::PerfMarker`).
- `d3d9_direct3d.cpp` - IDirect3D9(Ex) wrapper, device creation.
- `d3d9_device.cpp` - device lifetime, device-owned objects, Reset, overlay, capture start/end.
- `d3d9_device_wrap.cpp` - every IDirect3DDevice9(Ex) method.
- `d3d9_device_serialise.cpp` - serialisation of every recorded call, reading captures. Most of the
  `Serialise_` functions and the recording versions of simple device methods were generated from a
  table of the API, so they follow one pattern; edit them by hand now.
- `d3d9_contents.*` - resource contents: reading them back, shadow copies, lock tracking. The
  comment at the top of `d3d9_contents.h` describes how each kind of object is read.
- `d3d9_depth.cpp` - reading back depth stencils that can't be locked, and writing them at replay.
- `d3d9_replay.*` - the replay driver the replay controller talks to (`D3D9Replay`), and creating
  the replay device.
- `d3d9_postvs.cpp` - post-transform vertices for the mesh viewer (3e), on the CPU with
  `d3d9_vs_emulate.*` (vertex shader interpreter, vertex fetch, running a draw) and
  `d3d9_ffp_vertex.*` (fixed function vertex processing). The last two are portable and unit
  tested.
- `d3d9_replay_device.cpp` - the replay side of the device: loading, replaying the frame, the
  event list, writing contents.
- `d3d9_overlay.cpp` - the texture viewer's overlays, `IsRenderOutput` and `GetPassEvents` (phase
  3e).
- `d3d9_counters.cpp` - the GPU duration and samples passed counters.
- `d3d9_pipestate.cpp` also records resource usage while loading (`AddDrawUsage`, `GetUsage`).
- Failed calls: `RecordFailedCall` (`d3d9_device.h`, `d3d9_device.cpp`), the `RECORD_FAILED_CALL`
  macros in `d3d9_device_wrap.cpp`, and `Serialise_FailedCall` in `d3d9_device_serialise.cpp`.
- `d3d9_customshader.cpp` - the texture viewer's custom display shaders: their prefix, building
  them and drawing them.
- `d3d9_resources.*` - the wrapper base class and the resource wrappers. The comment at the top of
  `d3d9_resources.h` describes the lifetime model (external vs internal references, who holds real
  references, child objects).
- `d3d9_state.*` - the object bindings the device tracks itself, which state blocks snapshot.
- `d3d9_swapchain.*`, `d3d9_manager.*`, `d3d9_serialise.cpp` (struct serialisation),
  `d3d9_stringise.cpp` (chunk and enum names).

Tests: `util/test/demos/d3d9/`.
