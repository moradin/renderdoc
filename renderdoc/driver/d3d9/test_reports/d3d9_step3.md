# D3D9 step 3: small features

Step 3 of the plan in STATUS.md, one feature at a time, each committed with its tests:

1. GPU duration and samples passed counters.
2. The resource usage list.
3. Debug messages for failed calls.
4. Custom display shaders in the texture viewer.

- **Machine:** Windows 11, RTX 4070 Ti, NVIDIA driver 32.0.16.1088.
- **Builds:** `renderdoc.sln` and `util\test\demos.sln`, Development, x64 and x86, 0 warnings and 0
  errors each.

## 1. GPU counters

`d3d9_counters.cpp` implements `EnumerateCounters`, `DescribeCounter` and `FetchCounters` with two
counters, GPU Duration (timestamp queries) and Samples Passed (occlusion queries), measured per
action the way D3D11 does. STATUS.md's "GPU counters" has how it works.

### Which queries the GPU has

A small native program (not committed) asked the HAL device for each query type with
`CreateQuery(type, NULL)`, as 64-bit and as 32-bit, with the same answers:

```
NVIDIA GeForce RTX 4070 Ti, driver 32.0.16.1088
VCACHE             supported
RESOURCEMANAGER    not supported
VERTEXSTATS        not supported
EVENT              supported
OCCLUSION          supported
TIMESTAMP          supported
TIMESTAMPDISJOINT  supported
TIMESTAMPFREQ      supported
PIPELINETIMINGS    not supported
INTERFACETIMINGS   not supported
VERTEXTIMINGS      not supported
PIXELTIMINGS       not supported
BANDWIDTHTIMINGS   not supported
CACHEUTILIZATION   not supported
MEMORYPRESSURE     supported
```

So there's nothing that maps onto D3D11's pipeline statistics (vertices, primitives, invocations),
and only the two counters are offered.

### The test: `D3D9_Counters`

A new demo and test. Each draw is a full screen quad cut down by a scissor rect and the depth test,
so the samples it passes are known:

| Draw | Set up | Expected |
|---|---|---|
| Scissored quad | 64x32 scissor, depth 0.5, writes depth | 2048 |
| Hidden quad | the same rect at depth 0.75, `LESS` | 0 |
| Half hidden quad | 64x32 overlapping the first by 32 columns, depth 0.75 | 1024 |
| Heavy quad | the whole 400x300 back buffer, a pixel shader with about 4000 loop iterations | 120000 |
| Single pixel quad | the same shader, 1x1 scissor | 1 |
| Multisampled quad | 16x16 scissor into a 4x multisampled target | 1024 (4 per pixel) |

Each draw is inside an occlusion query of the demo's own, and the heavy one between timestamp queries
of its own. The demo checks what the GPU counted against the table and writes the counts and the
heavy draw's fastest time to its tmp folder. So the reference is the GPU natively, and at replay the
demo's queries are nested around RenderDoc's.

The test runs the demo natively first, then captures it, and checks:
- the counts the GPU gave natively and under RenderDoc are the same, and match the table;
- both counters are enumerated and described (seconds as a float, samples as a uint, 8 bytes);
- every action that isn't a marker has a duration and a sample count, and no duration failed (-1);
- each draw's samples passed at replay equals the native count;
- the heavy draw takes at least 10 times as long as the single pixel one, and between a quarter and
  4 times the native time (the GPU's clocks vary, so this only catches wrong units or frequencies);
- a second fetch gives the same sample counts;
- after the fetch the back buffer reads as it did at the event the test was at (pixel 20,20 red, at
  the first draw, before the heavy quad covers it), so the replay was put back.

The x64 run (`python util\test\run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`), from RenderDoc's
counters at replay:

```
** The counters enumerated are ['GPU Duration', 'Samples Passed']
** The counters are described with their units and types
** Picked value at 20,20 in Backbuffer 17 is as expected
** After fetching the counters the back buffer is as it was at the first draw
.. 3 IDirect3DDevice9::Clear: 0.00 us, 0 samples
.. 20 IDirect3DDevice9::DrawPrimitive: 1.02 us, 2048 samples
.. 28 IDirect3DDevice9::DrawPrimitive: 1.03 us, 0 samples
.. 36 IDirect3DDevice9::DrawPrimitive: 1.02 us, 1024 samples
.. 49 IDirect3DDevice9::DrawPrimitive: 843.78 us, 120000 samples
.. 60 IDirect3DDevice9::DrawPrimitive: 0.00 us, 1 samples
.. 72 IDirect3DDevice9::DrawPrimitive: 2.05 us, 1024 samples
.. 76 IDirect3DDevice9::StretchRect: 7.16 us, 0 samples
.. 79 IDirect3DDevice9::Present: 0.00 us, 0 samples
** Each of the 9 actions has a duration and a sample count
** 'Scissored quad' passed 2048 samples, as natively
** 'Hidden quad' passed 0 samples, as natively
** 'Half hidden quad' passed 1024 samples, as natively
** 'Heavy quad' passed 120000 samples, as natively
** 'Single pixel quad' passed 1 samples, as natively
** 'Multisampled quad' passed 1024 samples, as natively
** The heavy quad took 843.78 us, the single pixel one 0.00 us
.. Natively the heavy quad took 962.56 us at its fastest, with a timestamp frequency of 100000000 Hz
** The heavy quad's duration at replay is 0.88 times the native one
** A second fetch gives the same sample counts
```

The x86 run (`--x86`): the same counts and checks, the heavy quad 879.62 us at replay and 913.41 us
natively (0.96). Both runs: 12 of 12 tests passed. Full output: `d3d9_step3/counters_run_x64.txt` and
`d3d9_step3/counters_run_x86.txt`.

Everything with "samples" and "Natively" in it is from the GPU, the rest is RenderDoc's.

### Timestamp resolution

The timestamp frequency is 100 MHz, but NVIDIA's D3D9 timestamps step in about 1 µs: the short draws
read 0, 1.02, 1.03 or 2.05 µs. That's the driver; the demo's own timestamps show it too.

### In the UI

On a `D3D9_Counters` capture in qrenderdoc:
- **Event browser:** the clock button fills the Duration column, with each marker's total and the
  frame's (`d3d9_step3/counters_1_event_browser.png`). The column starts too narrow to show the
  values (they're right-aligned and cut off); widening it shows them. Upstream's column does that
  for every API.
- **Performance counter viewer:** "Capture counters" lists GPU Duration and Samples Passed under
  Generic, D3D9 Built-in, with their descriptions (`d3d9_step3/counters_2_selection.png`). Sampling
  both gives a row per action (`d3d9_step3/counters_3_viewer.png`) with the same sample counts as
  natively.

### Something unexpected: durations with an idle GPU

In the UI the heavy quad took 11.3-11.9 ms in the event browser and 4.5 ms in the counter viewer,
where the tests had 0.84-0.88 ms. A replay from Python gave 8.4 ms. It's the GPU's power state:

```
clocks.current.graphics [MHz], clocks.max.graphics [MHz], pstate
210 MHz, 3105 MHz, P8            <- idle, before
2835 MHz, P0                     <- with a demo running in the background
fetch 2: heavy 882.7 us, total 893.8 us   <- the same Python replay, with the demo running
```

A fetch is too little work to raise the clocks, so the durations are whatever the GPU's current
clocks give. The test passes because it fetches right after the demo ran. Natively, a single frame
of the demo from idle measured 0.97 ms, since the demo's own start-up raises the clocks. D3D11's
counters are measured the same way. STATUS.md and the README list it as a limitation.

### Not covered

- **Other GPUs:** AMD and Intel may support other query types, and may count multisampled samples
  differently. The test compares with what the GPU counted, so it would still hold there.
- **Durations against native per draw:** only the heavy draw is compared with a native time. The
  others are too short for the timer's resolution.

## 2. Resource usage

`GetUsage` now lists each resource's uses, recorded while the capture loads. STATUS.md's "Resource
usage" has what's recorded for each call.

### The test: `D3D9_Resource_Usage`

A new demo and test. The demo uses resources in each way D3D9 can within a frame, and binds others
that nothing reads: a texture on a pixel sampler the shader doesn't sample, a texture after a
disabled fixed function stage, a stream the declaration doesn't use, and the index buffer during a
non-indexed draw and UP draws. Each resource has a size no other has, so the test finds it by its
description; the two render targets bound together must be the same size, so the test takes them
from the draw's outputs. The test then requires each resource's usage to be exactly the expected
list, nothing missing and nothing extra, with no duplicates.

**Native check of Clear.** Before the first frame the demo makes both render targets green on their
own, binds both and clears to red, and reads both back. The GPU cleared both, so RenderDoc records a
colour Clear as clearing every bound target. The first version of the check bound targets of
different sizes (64x32 and 64x36): then the Clear failed altogether and neither turned red, which
the check now catches by reading target 0 as well.

```
** Natively a colour Clear clears both bound render targets, as RenderDoc records
.. Events: clear 5, shader draw 31, non-indexed 35, fixed function 52, StretchRect 67, GetRenderTargetData 70, ColorFill 73, UpdateTexture 77, Unlocks 8 and 11
** sampled pixel texture (8x8): 31 PS_Resource, 35 PS_Resource
** unsampled pixel texture (12x12): unused
** texture after a disabled stage (28x28): unused
** locked texture (44x44): 11 CPUWrite
** stream 0 (1024 bytes): 31 VertexBuffer, 35 VertexBuffer
** stream 1 (1152 bytes): 8 CPUWrite, 31 VertexBuffer, 35 VertexBuffer
** unused stream 2 (1280 bytes): unused
** index buffer (640 bytes): 31 IndexBuffer
** render target 0 (64x32): 5 Clear, 31 ColorTarget, 35 ColorTarget, 52 ColorTarget, 67 CopySrc
** render target 1 (64x32): 5 Clear, 31 ColorTarget, 35 ColorTarget
** depth stencil (72x40): 5 Clear, 31 DepthStencilTarget
** StretchRect destination (64x44): 67 CopyDst, 70 CopySrc, 73 Clear
** VS float constants: 31 VS_Constants, 35 VS_Constants
** PS float constants: 31 PS_Constants, 35 PS_Constants
** VS integer constants: unused
** VS boolean constants: unused
** PS integer constants: unused
** PS boolean constants: unused
** GetRenderTargetData destination (64x44): 70 CopyDst
** D24S8 back buffer or depth (400x300): 85 Clear
** B8G8R8A8_UNORM back buffer or depth (400x300): 85 Clear
** vertex texture (4x4): 31 VS_Resource, 35 VS_Resource
** texture of the fixed function draws (20x20): 52 PS_Resource, 57 PS_Resource
** multisampled target (40x40): 57 ColorTarget, 64 ResolveSrc
** resolve destination (40x40): 64 ResolveDst
** autogen mip texture (128x128): 80 Clear, 81 GenMips
```

The x86 run gives the same lines. Both runs: 13 of 13 tests passed (`d3d9_step3/usage_run_x64.txt`,
`d3d9_step3/usage_run_x86.txt`).

### In the UI

On a `D3D9_Resource_Usage` capture, at the shader draw, viewing render target 0:
- **Timeline:** "Usage for Render Target Texture 29" shows the clear at event 5, writes at 31, 35
  and 52, and the read at 67 (`d3d9_step3/usage_1_timeline.png`).
- **Resource Inspector:** "Usage in Frame" lists EID 5 Clear, EID 31-52 Rendertarget and EID 67
  Copy - Source (`d3d9_step3/usage_2_inspector_rt0.png`). The multisampled target lists EID 57
  Rendertarget and EID 64 Resolve - Source (`d3d9_step3/usage_3_inspector_msaa.png`).

### Not covered

`ProcessVertices`, `DrawRectPatch`/`DrawTriPatch`, `UpdateSurface`, GDI drawing and
`GetFrontBufferData` aren't in the demo. They go through the same functions as the calls that are.

## 3. Debug messages for failed calls

D3D9's debug runtime doesn't run on Windows 8 and later, so the messages are the calls the runtime
rejected. A call that fails while capturing is recorded as a `FailedCall` chunk with its name,
result and parameters. On replay it's an event of its own, named after the call with "(failed)",
and a debug message at that event. STATUS.md's "Debug messages" has how it works and what's
covered.

Tested on top of b184fa7dc, with the changes in the commit that adds this section.

### What the runtime rejects, natively

The first version of the demo tried calls that look invalid. Natively, on this machine:

| Call | Result |
|---|---|
| `SetRenderTarget(4, ...)` | `D3DERR_INVALIDCALL` |
| `SetRenderTarget` with a texture's surface that isn't a render target | `D3DERR_INVALIDCALL` |
| `SetDepthStencilSurface` with a colour render target | `D3DERR_INVALIDCALL` |
| `SetVertexShaderConstantF(300, ...)`, `SetPixelShaderConstantF(224, ...)` | `D3DERR_INVALIDCALL` |
| `EndScene` without `BeginScene`, a second `BeginScene` | `D3DERR_INVALIDCALL` |
| `StretchRect` with a source rect bigger than the surface | `D3DERR_INVALIDCALL` |
| `ColorFill` on a managed texture's surface | `D3DERR_INVALIDCALL` |
| `UpdateTexture` between two default pool textures | `D3DERR_INVALIDCALL` |
| `GetRenderTargetData` into a smaller surface | `D3DERR_INVALIDCALL` |
| `LockRect` on a level the texture doesn't have | `D3DERR_INVALIDCALL` |
| `CreateTexture(0, 0, ...)` | `D3DERR_INVALIDCALL` |
| `SetTexture(300, ...)`, `SetSamplerState(300, ...)`, `SetRenderState(1000, ...)`, `SetStreamSource(16, ...)` | S_OK |
| `SetStreamSource` with an offset of 2 | S_OK |
| `Issue(D3DISSUE_BEGIN)` on an event query | S_OK |
| `UnlockRect` on a texture that isn't locked | S_OK |
| `DrawIndexedPrimitive` without an index buffer | `D3DERR_INVALIDCALL` on frame 0, S_OK from frame 1 |
| `Clear` with two render targets of different sizes (64x32 with 32x32, 32x32 with 64x32, 64x32 with 64x36) | S_OK |
| `Clear` with a depth stencil smaller than the render target | S_OK |

The run that made the out of range `SetTexture`, `SetSamplerState`, `SetRenderState` and
`SetStreamSource` calls ended with heap corruption (exit code 0xC0000374), so the demo doesn't make
them now. Which of them caused it wasn't narrowed down.

The draw without an index buffer is why the demo now warns if a call's result changes between
frames: the capture is of frame 5, and the reference file has frame 0's results. That draw was
taken out.

**Something unexpected, about step 2.** While writing `D3D9_Resource_Usage`, a Clear with targets
of 64x32 and 64x36 seemed to fail (section 2). Here the same sizes clear without an error, so that
failure had another cause, which wasn't tracked down. STATUS.md's "Resource usage" is corrected.

### The test: `D3D9_Failed_Calls`

The demo makes 18 calls, each in a marker of its own: the 13 that fail natively and 5 that
succeed (the four Clears and the `UnlockRect`). It writes each one's result on frame 0, natively and
under RenderDoc. The test checks:

- the results are the same under RenderDoc;
- each call that failed has exactly one message, at an event inside its marker, which is named
  after the call with "(failed)" and has the result and the HRESULT as the message's ID;
- some parameters: the render target index, the constants' start register and count, the
  StretchRect source rect, the texture's width and levels;
- a failed lock records its texture, and the texture is in the capture;
- a failed call's constants aren't read;
- the calls that succeeded have no message and no failed event, and there are no other messages;
- `GetDebugMessages` returns the messages only once;
- the triangle drawn after the failed calls replays.

The x64 run (`d3d9_step3/failed_run_x64.txt`):

```
** All 18 calls returned the same natively and under RenderDoc
.. Natively 13 calls failed: SetRenderTarget index 4 (8876086C), SetRenderTarget without render target usage (8876086C), SetDepthStencilSurface with a colour target (8876086C), SetVertexShaderConstantF past the end (8876086C), SetPixelShaderConstantF past the end (8876086C), EndScene without BeginScene (8876086C), BeginScene twice (8876086C), StretchRect outside the source (8876086C), ColorFill on a managed texture (8876086C), UpdateTexture between default pool textures (8876086C), GetRenderTargetData into a different size (8876086C), LockRect on a missing level (8876086C), CreateTexture with zero size (8876086C)
.. and 5 succeeded: Clear with a smaller second target, Clear with a bigger second target, Clear with a taller second target, Clear with a smaller depth stencil, UnlockRect without a lock
.. 13 debug messages:
** SetRenderTarget index 4: event 31 IDirect3DDevice9::SetRenderTarget (failed) with Function, Result, Object, Parameters, RenderTargetIndex, pRenderTarget: IDirect3DDevice9::SetRenderTarget returned D3DERR_INVALIDCALL (0x8876086C) when it was captured, so it isn't replayed.
** SetRenderTarget without render target usage: event 34 IDirect3DDevice9::SetRenderTarget (failed) with ...
** SetDepthStencilSurface with a colour target: event 37 IDirect3DDevice9::SetDepthStencilSurface (failed) with ...
** SetVertexShaderConstantF past the end: event 42 IDirect3DDevice9::SetVertexShaderConstantF (failed) with Function, Result, Object, Parameters, StartRegister, pConstantData, Vector4fCount: ...
** SetPixelShaderConstantF past the end: event 45 IDirect3DDevice9::SetPixelShaderConstantF (failed) with ...
** EndScene without BeginScene: event 48 IDirect3DDevice9::EndScene (failed) with Function, Result, Object, Parameters: ...
** BeginScene twice: event 52 IDirect3DDevice9::BeginScene (failed) with ...
** StretchRect outside the source: event 65 IDirect3DDevice9::StretchRect (failed) with Function, Result, Object, Parameters, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter: ...
** ColorFill on a managed texture: event 68 IDirect3DDevice9::ColorFill (failed) with ...
** UpdateTexture between default pool textures: event 71 IDirect3DDevice9::UpdateTexture (failed) with ...
** GetRenderTargetData into a different size: event 74 IDirect3DDevice9::GetRenderTargetData (failed) with ...
** LockRect on a missing level: event 77 IDirect3DTexture9::LockRect (failed) with Function, Result, Object, Parameters: IDirect3DTexture9::LockRect returned D3DERR_INVALIDCALL (0x8876086C) when it was captured, so it isn't replayed.
** CreateTexture with zero size: event 82 IDirect3DDevice9::CreateTexture (failed) with Function, Result, Object, Parameters, Width, Height, Levels, Usage, Format, Pool, pTexture: ...
** SetRenderTarget index 4: RenderTargetIndex is 4
** SetVertexShaderConstantF past the end: StartRegister is 300
** SetVertexShaderConstantF past the end: Vector4fCount is 1
** SetPixelShaderConstantF past the end: StartRegister is 224
** StretchRect outside the source: pSourceRect.right is 128
** CreateTexture with zero size: Width is 0
** CreateTexture with zero size: Levels is 1
** LockRect on a missing level: the failed call records the 8x8 texture Texture 29
** SetVertexShaderConstantF past the end: pConstantData isn't read
** Picked value at 199,149 in Backbuffer 17 is as expected
** The triangle after the failed calls replays
** Recompressed and re-imported capture files are identical
```

(The lines shortened with "..." end with the same message as the first, for their call.) The
`Object` and `Parameters` fields are hidden in the UI where they say nothing (see below). The x86
run gives the same lines apart from the capture's bitness. Both runs: 14 of 14 tests passed
(`d3d9_step3/failed_run_x64.txt`, `d3d9_step3/failed_run_x86.txt`). The unit tests
(`renderdoccmd test unit "[d3d9]"`) pass, 6057 assertions in 13 test cases.

The results the test checks came from the GPU: which calls fail, and their HRESULTs, natively and
under RenderDoc. The messages, the events and the parameters are RenderDoc's.

### In the UI

On a 64-bit `D3D9_Failed_Calls` capture:
- **Errors and Warnings** lists the 13 messages with their events, source API, severity High, the
  category and the HRESULT as the ID, and the status bar says "Capture has 13 issues"
  (`d3d9_step3/failed_1_errors_window.png`).
- **API Inspector**, at the failed StretchRect: "StretchRect (failed)(D3DERR_INVALIDCALL)", with
  the function, the result and the call's parameters. The source and destination show as Render
  Target 22 and 23 (`d3d9_step3/failed_2_stretchrect.png`).
- **API Inspector**, at the failed LockRect: the texture shows as Texture 29
  (`d3d9_step3/failed_3_lockrect.png`).

Two changes came from looking at the UI:
- The summary in the API Inspector listed the function as well as the result, which repeats the
  event's name, so only the result is in it now.
- A device call showed "Object: No Resource", so the object is hidden when there isn't one. A
  failed lock's texture first showed as "Unknown Resource 28", since nothing else in the frame used
  it, so a failed call on an object now adds the object to the capture.

### Not covered

- No call in the tests fails only on replay, so the RuntimeWarning messages for those are untested.
- Failed creations other than `CreateTexture`, failed calls on objects other than `LockRect`, and
  failed draws aren't in the test. No draw failed reliably natively.
- A resource that only a failed device call's parameters use isn't in the capture, and shows as
  "Unknown Resource" (not the case in the test, where the StretchRect's surfaces are used
  elsewhere).

## 4. Custom display shaders

The texture viewer's custom shaders: a ps_3_0 pixel shader the user writes is drawn over the
viewed subresource, and the texture viewer shows the result. STATUS.md's "Custom display shaders"
has how it works, the bindings and the decisions.

Tested on top of 119840b39, with the changes in the commit that adds this section.

### What the compiler and the GPU do

Before writing the prefix, fxc (d3dcompiler_47) was tried on what D3D11's custom shaders use, for
ps_3_0:

- The texture viewer's template, `float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) :
  SV_Target0`, compiles: `SV_Position` becomes `vPos` and `SV_Target0` `oC0`.
- `Texture2D` with `SamplerState` compiles, but fxc ignores their registers and puts the pair in a
  sampler of its own (`s1` for `register(t2)` and `register(s0)`). That's why the texture is bound
  to every declared sampler of its type, rather than at fixed registers.
- `Texture2DArray` doesn't compile ("Unsupported texture type for ps_3_0").
- A `cbuffer` of `uint4`s compiles to float constants, and globals the compiler places go around
  ones with explicit registers.
- **Unexpected:** `(uint)` of a float constant doesn't compile ("in ps_3_0 uints can only be used
  with known-positive values"). The demo's first build hit this; the prefix's functions use
  `max(x, 0)` first, which compiles.

Natively, the demo's position shader showed that `vPos` is at integer pixel coordinates (0 for the
first pixel, D3D11's `SV_Position` gives 0.5), and that `TEXCOORD0`'s z and w are 0 when the vertex
shader writes them so, as the texture viewer's now does.

### The test: `D3D9_Custom_Shaders`

The demo makes a texture of each kind the texture viewer samples differently, each with a size of
its own: a default pool A8R8G8B8 2D texture with 3 mips (sampled directly), a managed one with 2
mips (decoded on the CPU), an L8 texture (sampled as L, L, L, 1), a cube and a volume texture with 2
mips, a render target surface (which the texture viewer copies into a texture) and a D24S8 depth
stencil (decoded). Before the first frame it draws each subresource natively with a ps_3_0 shader
that samples the selected mip and face or slice, with the `RD_` functions defined from its own
constants, and writes what the GPU returned at 6 texels to a reference file, with the shader. The
depth stencil can't be sampled, so its reference is the depth and stencil it's cleared to.

The test builds that shader with RenderDoc's prefix as a custom shader, applies it to each
subresource and picks the output at the same texels. It also checks:
- the position shader against the native draw over a 16x8 target;
- the prefix's values for each texture, at its last mip and a face or slice other than 0:
  `RD_TexDim`, the mip, slice, sample and texture type, the selected range, the YUV values (0), and
  that a global the prefix doesn't fill is 0;
- that `Texture2D` and `SamplerState` sample the same as `sampler2D`;
- that the UI's template builds, and that a shader with an error, or with `/T ps_2_0`, doesn't and
  says why. The error is on line 1 of the shader: the prefix ends with `#line 1`, so the line
  numbers the editor shows are the user's own. Without it the first run's error was on line 88.

The x64 run (`d3d9_step3/custom_run_x64.txt`):

```
** The capture was made by a 64-bit process (Windows x86 64-bit)
** HLSL and D3D9 bytecode are accepted, with an HLSL prefix
** The UI template shader builds
** A shader with an error doesn't build: E:\Projects\renderdoc\util\test\main.hlsl(1,37-45): error X3004: undeclared identifier 'undefined'
** A shader for another profile doesn't build: D3D9 custom display shaders are compiled as ps_3_0, the profile ps_2_0 can't be used
** The sample shader builds
** The position shader builds
** The constants shader builds
** The Texture2D and SamplerState shader builds
** Default 2D: 18 texels match the GPU's native sampling
** Managed 2D: 12 texels match the GPU's native sampling
** L8: 6 texels match the GPU's native sampling
** Cube: 72 texels match the GPU's native sampling
** Volume: 36 texels match the GPU's native sampling
** Render Target: 6 texels match the GPU's native sampling
** Depth: 4 texels match the GPU's native sampling
** Position: 6 texels match the GPU's native sampling
** Default 2D mip 2 slice 0: TexDim [64.0, 32.0, 1.0, 3.0], mip/slice/sample/type [2.0, 0.0, 0.0, 2.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Managed 2D mip 1 slice 0: TexDim [48.0, 24.0, 1.0, 2.0], mip/slice/sample/type [1.0, 0.0, 0.0, 2.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** L8 mip 0 slice 0: TexDim [16.0, 8.0, 1.0, 1.0], mip/slice/sample/type [0.0, 0.0, 0.0, 2.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Cube mip 1 slice 3: TexDim [16.0, 16.0, 6.0, 2.0], mip/slice/sample/type [1.0, 3.0, 0.0, 8.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Volume mip 1 slice 1: TexDim [16.0, 8.0, 4.0, 2.0], mip/slice/sample/type [1.0, 1.0, 0.0, 3.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Render Target mip 0 slice 0: TexDim [40.0, 20.0, 1.0, 1.0], mip/slice/sample/type [0.0, 0.0, 0.0, 2.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Depth mip 0 slice 0: TexDim [40.0, 20.0, 1.0, 1.0], mip/slice/sample/type [0.0, 0.0, 0.0, 5.0], range [0.125, 0.75], a global the prefix doesn't fill is 0
** Texture2D and SamplerState sample the same as sampler2D
```

The x86 run, which captures the 32-bit demo, gives the same lines apart from the capture's bitness
and the error's line (it ran before `#line 1` was added; the x64 run above is after, and adding it
changed only the prefix). Both runs: 15 of 15 tests passed (`d3d9_step3/custom_run_x64.txt`, `d3d9_step3/custom_run_x86.txt`).
The unit tests (`renderdoccmd test unit "[d3d9]"`) pass, 6057 assertions in 13 test cases.

The reference values came from the GPU, apart from the depth stencil's, which are its clear values.
The custom shader's output is RenderDoc's draw on the GPU, read back with pick pixel.

### In the UI

On a 64-bit `D3D9_Custom_Shaders` capture, with a custom shader in qrenderdoc's settings folder
that shows a colour texture's alpha, or a depth stencil's stencil, in grey (the file was deleted
afterwards):
- The default pool texture with Channels set to Custom and the shader selected
  (`d3d9_step3/custom_1_alpha.png`): its alpha, which falls by 8 a texel to the right and 4 a texel
  down and wraps, as the demo fills it.
- The depth stencil (`d3d9_step3/custom_2_stencil.png`): stencil 7 on the left half (nearly
  black) and 200 on the right, as cleared.
- The shader editor, after inserting the constants and resources snippets
  (`d3d9_step3/custom_3_snippets.png`): D3D9's texture types and the `sampler2D`, `samplerCUBE` and
  `sampler3D` declarations, rather than D3D11's texture arrays. The prefix with those snippets
  compiles with fxc.

### Not covered

- Multisampled textures, so `RD_SelectedSample()`'s values for them are untested.
- YUV formats and textures decoded because of their format (P8, ATI1 and so on). They take the
  decoded path that the managed texture and the depth stencil test.
- Picking and saving the custom shader's output in the UI, beyond the test's pick pixel.
