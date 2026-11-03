# D3D9 support status

D3D9 support is being built in phases on this fork. Upstream RenderDoc removed its D3D9 stub in 2023,
so none of this is upstream.

| Phase | What | State |
|---|---|---|
| 0 | Driver project restored on current upstream, overlay working | Done |
| 1 | Every object wrapped, bindings tracked, state blocks, Reset | Done, Reset needs more testing |
| 2a | Captures written: every call serialised, resource creation records, structured export | Done, tested with `D3D9_Simple_Triangle` |
| 2b | Full device state at frame start (render/sampler/texture stage states, transforms, lights, constants...) | Done, tested with `D3D9_Frame_Start_State` |
| 2c | Resource contents: Lock tracking, shadow copies of D3DPOOL_DEFAULT data, initial contents | Done, tested with `D3D9_Resource_Contents` |
| 2d | Edge cases: Reset/device loss mid-capture, state block contents, D3DPERF markers in captures, depth stencil read back, GDI drawing, front buffer copies, locks spanning the capture start | Done, tested with `D3D9_Capture_Edge_Cases` and `D3D9_Reset_Capture`; device loss needs a real game |
| 3a | Replay: loading captures, replaying to any event, the event list, resource descriptions, reading texture and buffer data | Written, not built or tested yet |
| 3b | Texture viewer and buffers: displaying every D3D9 format, pick pixel, min/max, histogram, mesh viewer inputs | Written, not built or tested yet |
| 3c | D3D9 pipeline state, with its own UI panel and Python bindings | Written, not built or tested yet |
| 3d | Shaders: SM1-3 disassembly, reflection from the constant table | Written, unit tested on Linux, not built on Windows yet |
| 3e | Mesh output and overlays | Not started |

## Needs testing

- **Reset / ResetEx with real games.** `D3D9_Wrapping_Check` and `D3D9Ex_Wrapping_Check` exercise
  Reset in a controlled way, but it hasn't been tested with a real game yet. Things to try: alt-tab
  out of and back into fullscreen, changing resolution, games that recreate D3DPOOL_DEFAULT
  resources on device loss. Watch for Reset failing only under RenderDoc (something of ours holding a
  D3DPOOL_DEFAULT reference), and for crashes after Reset (stale back buffer or depth stencil
  wrappers).
- The D3DPERF marker hooks moved to the D3D11 project upstream. They now share `d3d9.dll` with our
  device creation hooks; check that markers still show up in D3D11 captures of apps that use them.
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
  captured frame, and the shadow copies (below) cost memory in 32-bit games.

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
- Output window screenshots (`GetOutputWindowData`) aren't implemented.

### Checking replay

`util/test/d3d9_replay_check.py` opens a capture with the Python module, prints its events and
textures, saves the back buffer at the first and last events as `<capture>_first.png` and
`<capture>_last.png`, and prints the depth stencils' values at the first event, which are their
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
- `D3D9_Capture_Edge_Cases`: the depth values at the first event match the ones listed for its
  initial contents above (0x4000005A, 0xBFFFFFA5 and 0xFFFFFF00 for the automatic depth stencil,
  0x80000033 and 0xFFFFFF00 in each sample of the multisampled one, 0x7FFF for D16, 0x5FFFFF0F for
  INTZ).
- RenderDoc's log has no errors, and no "Couldn't write the contents" warnings except for the
  gaps above.

## Texture viewer (phase 3b)

Written, not built or tested yet. This replaces the 3a notes above about `RenderTexture` and
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
- The mesh viewer needs the vertex inputs from the pipeline state (3c) before it has anything to
  show. Post-transform data is 3e.
- Multisampled colour targets are only shown resolved, whichever sample is picked. Multisampled
  depth stencils show each sample.
- Custom display shaders aren't supported (a warning is logged), and neither are the overlays
  (3e); `RenderOverlay` still returns nothing.
- P8 and A8P8 show their indices, not the palette colours.
- ATI2 is decoded with its first block as red, like BC5. Some tools swap ATI2's channels; check
  against a known texture.
- D32 depth is read back to 24 bits and described as D32_FLOAT by 3a, so its raw data in the
  buffer view of the texture looks wrong, though display and pick pixel are right. D24FS8 and
  D3DMULTISAMPLE_NONMASKABLE depth stencils can't be read, so they don't display.
- The texture viewer and mesh viewer need shader model 3 hardware. Linear filtering when zoomed
  out is only used for natively sampled formats that support it; decoded ones are always point
  sampled.
- Textures in the CPU path are read back and decoded whenever the event changes, which can be slow
  for big depth stencils (they're read with the depth tests).
- At an event between `BeginStateBlock` and `EndStateBlock` nothing is drawn (the texture and mesh
  viewers stay empty, a warning is logged once), since our state calls would be recorded into the
  application's state block instead of reaching the device. Pick pixel still works for textures
  the CPU can read, other than depth stencils.

What to test on Windows (all in the UI unless noted):
- Open the `D3D9_Simple_Triangle` capture, select the Present and open the back buffer: the
  triangles show on the grey background, the thumbnail strip shows the back buffer, and hovering
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

## Pipeline state (phase 3c)

Captures now present themselves as `GraphicsAPI.D3D9` (`IsD3D()` is true for it, so the UI uses
D3D naming and HLSL packing), with their own pipeline state, `D3D9Pipe::State`
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
- The descriptors live in one virtual descriptor store, like D3D11: descriptors 0-20 are the
  samplers (texture and sampler states together) numbered like `D3D9SamplerToSlot` (pixel 0-15,
  DMAP 16, vertex 17-20), 21-26 the shader constants.
- Shader constants are presented as six pseudo buffers, listed as resources: "VS float constants
  (c#)", "VS integer constants (i#)", "VS boolean constants (b#)", and the same for PS. Each holds
  the stage's whole register file from register 0: a float4 per c#, an int4 per i#, a uint32 (0 or
  1) per b#. `GetBufferData` on them reads the replay device's current constants, so
  `GetCBufferVariableContents` works unchanged: the constant block descriptors point at them.
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
- `GetUsage` isn't implemented, so the resource context menu's usage list is empty.
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

What to look for, in the UI's Pipeline State window and in the script:
- the capture's API shows as D3D9, the Pipeline State window shows the D3D9 panel with IA, VS, RS,
  PS, OM and an isolated "Render States" stage, and nothing errors in RenderDoc's log.
- `D3D9_Simple_Triangle`: at the draw, the topology is TriangleList with the triangle diagram, the
  vertex elements match the demo's declaration (or its FVF, with "From FVF 0x..."), the stream 0
  row shows its vertex buffer with the demo's stride, and double-clicking it opens the buffer viewer
  showing the vertices. The mesh viewer shows the triangle's inputs (PipeState.GetVertexInputs).
- shaders: the VS and PS rows name the shader objects with their shader model (e.g. `vs_3_0`) and
  "no reflection available". The Constants rows list the three register sets (256 float, 16
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
indented 4 spaces and 2 more inside `if`, `loop` and `rep`. Swizzles drop trailing repeats
(`.xyzz` is `.xyz`, `.xxxx` is `.x`), `dcl` leaves off usage index 0 (`dcl_texcoord`), relatively
addressed registers are `c4[a0.x]` and `v0[aL]`, and `def` floats have 9 significant digits.

Reflection, which is what phase 3c's pipeline state builds on:
- Constants are up to three constant blocks, only for the register sets the shader uses, in this
  order: "Float constants (c#)", "Integer constants (i#)", "Boolean constants (b#)". Each block's
  `fixedBindNumber` is its set (0 float, 1 integer, 2 boolean, `D3D9BC::ConstantSet`) and
  `bufferBacked` is false. Variables are at register * 16 bytes for float and integer registers and
  register * 4 for booleans, and `FillCBufferVariables` takes that set's whole register file from
  register 0 (float4s, int4s, or a uint32 0/1 per boolean). The shader's `def`, `defi` and `defb`
  literals are applied on top of the data, since the shader reads them rather than the device's
  constants, and padded out if the data is shorter.
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

Not built with MSVC yet. To check, with the demos under `util/test/demos/d3d9/`:
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
- `d3d9_replay_device.cpp` - the replay side of the device: loading, replaying the frame, the
  event list, writing contents.
- `d3d9_resources.*` - the wrapper base class and the resource wrappers. The comment at the top of
  `d3d9_resources.h` describes the lifetime model (external vs internal references, who holds real
  references, child objects).
- `d3d9_state.*` - the object bindings the device tracks itself, which state blocks snapshot.
- `d3d9_swapchain.*`, `d3d9_manager.*`, `d3d9_serialise.cpp` (struct serialisation),
  `d3d9_stringise.cpp` (chunk and enum names).

Tests: `util/test/demos/d3d9/`.
