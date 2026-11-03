# D3D9 support status

D3D9 support is being built in phases on this fork. Upstream RenderDoc removed its D3D9 stub in 2023,
so none of this is upstream.

| Phase | What | State |
|---|---|---|
| 0 | Driver project restored on current upstream, overlay working | Done |
| 1 | Every object wrapped, bindings tracked, state blocks, Reset | Done, Reset needs more testing |
| 2a | Captures written: every call serialised, resource creation records, structured export | Done, tested with `D3D9_Simple_Triangle` |
| 2b | Full device state at frame start (render/sampler/texture stage states, transforms, lights, constants...) | Done, tested with `D3D9_Frame_Start_State` |
| 2c | Resource contents: Lock tracking, shadow copies of D3DPOOL_DEFAULT data, initial contents | Written, needs testing with `D3D9_Resource_Contents` |
| 2d | Edge cases: Reset/device loss mid-capture, state block contents, D3DPERF markers in captures | Not started |
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
- D3DPERF markers don't appear in D3D9 captures yet (phase 2d).
- Applications' own `D3DSBT_ALL` state blocks have the same constant-copying behaviour natively. We
  pass them through as they are; replay (phase 3) needs to reproduce it rather than "fix" it.
- Sampler 256 (`D3DDMAPSAMPLER`) reads back `ADDRESSU` 0xBAADCAFE natively. It's recorded as read.
- **Resource contents (2c)**, with `D3D9_Resource_Contents`. Natively and under RenderDoc it should
  log "UpdateTexture copied only the dirty region"; our shadow copies of D3DPOOL_DEFAULT textures
  assume that. In a capture of it, each `InitialContents` chunk names its object and holds its
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

  Also worth checking: capture timing in a real game, since contents are read during the captured
  frame, and memory use in 32-bit games (see the shadow copies below).

## Known gaps in resource contents

- Depth stencils can't be read back unless their format is lockable (D3DFMT_D16_LOCKABLE,
  D3DFMT_D32F_LOCKABLE...). Their initial contents are empty with a note saying why; replay will need
  to clear them.
- D3DPOOL_DEFAULT textures without RENDERTARGET, DEPTHSTENCIL or DYNAMIC usage can't be locked or
  copied from, so we keep a copy of what UpdateTexture and UpdateSurface upload to them (the shadow).
  That costs as much memory as the textures themselves, all the time, which matters for 32-bit
  games. Formats whose layout we don't know get no shadow.
- D3DUSAGE_WRITEONLY buffers are read back by locking them read-only, which works on current
  drivers but is slow, and isn't guaranteed to return the data.
- Not recorded yet (phase 2d): GDI drawing between GetDC and ReleaseDC, GetFrontBufferData, and
  locks that started before the capture and end during it. Initial contents are still read before
  the first two write, so only the data they write is missing.
- UpdateTexture copies the source's dirty region, which we track but don't store in captures yet.
  Replay needs it to reproduce in-frame UpdateTexture calls (phase 3).

## Fixed

- The overlay used to restore the application's state with a `D3DSBT_ALL` state block. Applying one
  copies shader constants c0-c127 over c128-c255 (natively too, on an RTX 4070 Ti), so applications
  using upper constants were corrupted every frame. It also leaked stage 0 `D3DTSS_CONSTANT`
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

- `d3d9_hooks.cpp` - hooks Direct3DCreate9/Ex. The D3DPERF hooks live in `driver/d3d11/d3d9_hooks.cpp`.
- `d3d9_direct3d.cpp` - IDirect3D9(Ex) wrapper, device creation.
- `d3d9_device.cpp` - device lifetime, device-owned objects, Reset, overlay, capture start/end.
- `d3d9_device_wrap.cpp` - every IDirect3DDevice9(Ex) method.
- `d3d9_device_serialise.cpp` - serialisation of every recorded call, reading captures. Most of the
  `Serialise_` functions and the recording versions of simple device methods were generated from a
  table of the API, so they follow one pattern; edit them by hand now.
- `d3d9_contents.*` - resource contents: reading them back, shadow copies, lock tracking. The
  comment at the top of `d3d9_contents.h` describes how each kind of object is read.
- `d3d9_resources.*` - the wrapper base class and the resource wrappers. The comment at the top of
  `d3d9_resources.h` describes the lifetime model (external vs internal references, who holds real
  references, child objects).
- `d3d9_state.*` - the object bindings the device tracks itself, which state blocks snapshot.
- `d3d9_swapchain.*`, `d3d9_manager.*`, `d3d9_serialise.cpp` (struct serialisation),
  `d3d9_stringise.cpp` (chunk and enum names).

Tests: `util/test/demos/d3d9/`.
