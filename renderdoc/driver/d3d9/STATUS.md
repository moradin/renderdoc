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
| 2d | Edge cases: Reset/device loss mid-capture, state block contents, D3DPERF markers in captures, depth stencil read back, GDI drawing, front buffer copies, locks spanning the capture start | Done, needs testing with `D3D9_Capture_Edge_Cases` and `D3D9_Reset_Capture` |
| 3 | Replay | Not started |

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
  - A depth written by Clear may not be one of the format's levels: the RTX 4070 Ti compares
    against the cleared float, so the bisection found the level below it (D16 cleared to 0.5 read
    0x7FFF). Two more tests round those to the nearest level, ties up, below 2^23 (D24 depths under
    0.5); above that fp32 can't test halfway between levels. Values written by draws read exactly.
  - Reading needs multisampled R32F render targets for multisampled depth stencils.
  - Replay writes them back with `oDepth` and ZFUNC ALWAYS, and stencil with 8 draws using REPLACE
    and a write mask per bit (phase 3).
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
  Replay needs it to reproduce in-frame UpdateTexture calls (phase 3).

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
- D16 surface (32x32): every pixel 0x8000. INTZ texture (32x32, if supported): 0x6000000F.
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

## Fixed

- The overlay used to restore the application's state with a `D3DSBT_ALL` state block it created
  each frame. Creating one stores copies of shader constants c0-c127 as c128-c255 (natively too, on
  an RTX 4070 Ti), so applying it corrupted applications using upper constants every frame. It also leaked stage 0 `D3DTSS_CONSTANT`
  (0xFFFFFFFF), which the state block didn't restore. It now saves and restores only what it changes.
  Tested: c200/c180 survive in `D3D9_Frame_Start_State`, c138/c148 stay zero, and `D3DTSS_CONSTANT`
  keeps the application's value. Nothing else in `BeginCaptureFrame` changed.

## Inspecting a capture

Captures can't be opened in the UI until replay exists, but their contents can be dumped:

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
- `d3d9_depth.cpp` - reading back depth stencils that can't be locked.
- `d3d9_resources.*` - the wrapper base class and the resource wrappers. The comment at the top of
  `d3d9_resources.h` describes the lifetime model (external vs internal references, who holds real
  references, child objects).
- `d3d9_state.*` - the object bindings the device tracks itself, which state blocks snapshot.
- `d3d9_swapchain.*`, `d3d9_manager.*`, `d3d9_serialise.cpp` (struct serialisation),
  `d3d9_stringise.cpp` (chunk and enum names).

Tests: `util/test/demos/d3d9/`.
