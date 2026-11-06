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
