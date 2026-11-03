# D3D9 support status

D3D9 support is being built in phases on this fork. Upstream RenderDoc removed its D3D9 stub in 2023,
so none of this is upstream.

| Phase | What | State |
|---|---|---|
| 0 | Driver project restored on current upstream, overlay working | Done |
| 1 | Every object wrapped, bindings tracked, state blocks, Reset | Done, Reset needs more testing |
| 2a | Captures written: every call serialised, resource creation records, structured export | Done, tested with `D3D9_Simple_Triangle` |
| 2b | Full device state at frame start (render/sampler/texture stage states, transforms, lights, constants...) | Done, tested with `D3D9_Frame_Start_State` |
| 2c | Resource contents: Lock tracking, shadow copies of D3DPOOL_DEFAULT data, initial contents | Not started |
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
- The overlay no longer uses a `D3DSBT_ALL` state block to restore the application's state. Applying
  one copied shader constants c0-c127 over c128-c255 (natively too, on an RTX 4070 Ti), so every
  application using upper constants was corrupted each frame. It now saves and restores only what it
  changes. `D3D9_Frame_Start_State` checks c200/c180 survive, and the `BeginCaptureFrame` chunk
  shouldn't have copies of c10/c20 at c138/c148. Also worth checking in a game with a visible overlay:
  nothing in the scene should change when the overlay is toggled.
- D3DPERF markers don't appear in D3D9 captures yet (phase 2d).
- Applications' own `D3DSBT_ALL` state blocks have the same constant-copying behaviour natively. We
  pass them through as they are; replay (phase 3) needs to reproduce it rather than "fix" it.
- Sampler 256 (`D3DDMAPSAMPLER`) reads back `ADDRESSU` 0xBAADCAFE natively. It's recorded as read.

## Inspecting a capture

Captures can't be opened in the UI until replay exists, but their contents can be dumped:

```
renderdoccmd convert -f capture.rdc -o capture.zip.xml -c zip.xml
```

The XML has every chunk with its parameters. Until 2c is done, a capture contains the calls made
during the frame, the device state at the start of the frame (in `BeginCaptureFrame`), and how each
referenced object was created, but no resource contents.

## Code layout

- `d3d9_hooks.cpp` - hooks Direct3DCreate9/Ex. The D3DPERF hooks live in `driver/d3d11/d3d9_hooks.cpp`.
- `d3d9_direct3d.cpp` - IDirect3D9(Ex) wrapper, device creation.
- `d3d9_device.cpp` - device lifetime, device-owned objects, Reset, overlay, capture start/end.
- `d3d9_device_wrap.cpp` - every IDirect3DDevice9(Ex) method.
- `d3d9_device_serialise.cpp` - serialisation of every recorded call, reading captures. Most of the
  `Serialise_` functions and the recording versions of simple device methods were generated from a
  table of the API, so they follow one pattern; edit them by hand now.
- `d3d9_resources.*` - the wrapper base class and the resource wrappers. The comment at the top of
  `d3d9_resources.h` describes the lifetime model (external vs internal references, who holds real
  references, child objects).
- `d3d9_state.*` - the object bindings the device tracks itself, which state blocks snapshot.
- `d3d9_swapchain.*`, `d3d9_manager.*`, `d3d9_serialise.cpp` (struct serialisation),
  `d3d9_stringise.cpp` (chunk and enum names).

Tests: `util/test/demos/d3d9/`.
