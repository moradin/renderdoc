# Step 6.2: pixel history's events and their values

Step 6.2 of the plan (STATUS.md, "Step 6 plan"): `D3D9Replay::PixelHistory` lists each event that
wrote a pixel, with the pixel's colour, depth and stencil before and after it, and
`APIProperties.pixelHistory` is on. How it works is in STATUS.md under "Pixel history". Tested on
the commit that adds it (its parent is bc110cd92), on Windows 11 with an RTX 4070 Ti (driver
32.0.16.2124).

Files in `d3d9_step6_2_events_values/`:
- `tests_x64.txt`, `tests_x86.txt`: the automated tests' output, on the 64-bit and the 32-bit demos.
- `hist_dump.py` and its output `hist_dump.txt`: pixel histories of the test's capture, printed
  through the python bindings.
- `ui_driver.py`: drives qrenderdoc from its UI thread for the screenshots.
- `01_history_colour.png`, `02_history_copy.png`, `03_history_depth.png`: the screenshots.

## What changed

- `d3d9_pixelhistory.cpp` (new): `PixelHistory`.
  - Each event is replayed with the counters' partial replays.
  - A draw is listed if an occlusion query finds that it rasterises anything at the pixel. The
    query draw uses a constant colour shader, a one pixel scissor rect, and culling, the depth,
    stencil and alpha tests, clip planes and colour writes off.
  - Clears and copies are listed if the rectangle they write holds the pixel.
  - Values: the colour comes from a 1x1 StretchRect, and depth and stencil from the depth
    readback's bisection, scissored to the pixel.
- `d3d9_depth.cpp`: `ReadDepthStencil` can read one pixel, and `ReadDepthStencilPixel` uses that
  (locking the lockable formats instead).
- `d3d9_device_serialise.cpp`: Clear, ColorFill, StretchRect and UpdateSurface keep where they
  write when the capture loads (`D3D9WriteRegion`).
- `d3d9_pixel.cpp`, `d3d9_overlay.cpp`: the texel decoder and the bound vertex shader's model are
  exposed for pixel history.
- `d3d9_pipestate.cpp`: `pixelHistory` is on.
- New demo `D3D9_Pixel_History` (`util/test/demos/d3d9/d3d9_pixel_history.cpp`) and test
  `util/test/tests/D3D9/D3D9_Pixel_History.py`.

## The demo, and what the GPU did natively

The demo draws `D3D11_Pixel_History`'s batch, with upstream's shared vertices
(`PixelHistory::init`) and the same markers where D3D9 has the feature. D3D9's differences:
- There's no depth bounds test, sample index or pixel shader without output, so "Depth Bounds" and
  "No Output Shader" are left out, and "Sample Colouring" uses the usual shader.
- "Unbound Shader" draws with a vs_2_0 shader and fixed function pixel processing.
- The discarded column uses VPOS, which is the pixel's integer coordinates.
- "300 Instances" is an indexed instanced draw.

D3D9's own writes follow, under "D3D9 Writes":
- a Clear with two rects;
- a Clear of colour, depth and stencil, limited by a smaller viewport and a scissor rect;
- ColorFill, StretchRect and UpdateSurface into the target;
- a fixed function DrawPrimitiveUP of pretransformed vertices;
- an indexed draw with a base vertex.

There are two batches: an A8R8G8B8 render target texture with an INTZ depth stencil, and one with
no depth stencil.

Run natively, the third frame records 24 probe pixels before each batch and after each of its 657
events. These numbers are the GPU's:
- **Colour:** the target read back whole.
- **Depth:** the INTZ texture sampled.
- **Stencil:** 8 stencil test passes, adding up the bits.
- **Coverage:** for each draw, whether it rasterises anything at the probe, from an occlusion
  query with the same state changes pixel history makes.

The probes are the 18 pixels upstream's shared `Pixel_History` test looks at, and 6 for D3D9's
writes, inside and outside their rectangles.

The native values were as designed. At (110, 100) the stencil is 0x33 after "Unbound Shader" and
0x55 after "Stencil Write". At (150, 250) "Background" covers the pixel, but its shader discards
it there, so the pixel stays at the clear colour. (52, 30) is in the second Clear's viewport but
outside its scissor rect, and nothing writes it.

## Automated tests

`D3D9_Pixel_History` checks every probe's history at each batch's last event: the colour target of
both batches, and the depth stencil. 72 histories list 1068 events, and for every one:
- **The events listed** are exactly:
  - the draws the native queries found at the probe (for the depth stencil, only those with the
    depth or stencil test on, which are the ones that use it);
  - the clears and copies whose rectangles hold the probe.
- **The values before and after each event** are the GPU's:
  - the colour bit for bit, as c / 255;
  - the depth to within 1e-7 (in fact exactly: the largest difference was 0);
  - the stencil exactly.
- **What's reported:**
  - Copies are direct writes, and nothing else is.
  - A colour target's clears and copies, and the second batch's draws, have no depth or stencil
    (-1).
  - The depth stencil's modifications have no colour.
  - Draws have no shader output yet (`IsValid()` false), and the primitive is unknown.
- **Only the events up to the current one:** at "Simple Test", three probes list only the events
  up to it.

From `tests_x64.txt`:

```
** Basic: 48 pixel histories list 709 events, each with the colour, depth and stencil the GPU had natively before and after it (depth within 0), in 3.8s
** Basic: at 'Simple Test', only the events up to it are listed
** Colour Only: 24 pixel histories list 359 events, each with the colour, depth and stencil the GPU had natively before and after it (depth within 0), in 1.6s
** Colour Only: at 'Simple Test', only the events up to it are listed
...
## Tests complete summary: 20 passed out of 20 run from 211 total in 0:01:47.174689 ##
```

All 20 D3D9 tests (and `D3D11_D3DPERF_Markers`) pass on x64 and on the 32-bit demos
(`tests_x86.txt`, 20 of 20, the captures recorded as `Windows x86 32-bit`). The `[d3d9]` unit tests
pass: 20184 assertions in 26 test cases.

A history takes about 0.06 s here (`hist_dump.txt`). The pixel the 300 draws cover, (60, 130),
lists 301 events in 0.46 s on the colour target and 0.40 s on the depth stencil. Each event is replayed from the last one, and each written event reads the pixel twice.

## Builds

`renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors. Changed files are
formatted with clang-format 15.

## The UI (RenderDoc)

qrenderdoc on the test's capture, at the "Basic" batch's last event. It's driven by `ui_driver.py`
on the UI thread, with no mouse or keyboard input. The script opens the pixel history window
through `ViewPixelHistory` and `SetHistory`, as the texture viewer's History button does. The
button's own handler isn't covered, but the screenshots show it enabled. The texture viewer's image
is a native surface that PrintWindow doesn't capture, so it's blank.

- `01_history_colour.png`: (275, 258) on the colour target. It lists three events:
  - EID 12, the clear: (1, 1, 1, 1) to 0.2 grey, with no depth (`D: -`);
  - EID 97, "Background": magenta, depth 1.00 to 0.95;
  - EID 1150, "Depth Test": white, depth 0.95 to 0.05.

  The timeline's pixel history row marks the three events. The draws expand to one row each,
  "Unknown primitive", for now.
- `02_history_copy.png`: (330, 30). After the full clear:
  - EID 1267, Clear Rects: red;
  - EID 1277, StretchRect: yellow, shown as "Bound as UAV or copy - potential modification" (the
    UI's wording for direct writes on every API).
- `03_history_depth.png`: (110, 100) on the INTZ depth stencil. It lists:
  - EID 13, the depth clear: D 0.90 S 0x55 to D 1.00 S 0x00;
  - EID 59, "Unbound Shader": D 0.90 S 0x33;
  - EID 78, "Stencil Write": S 0x55.

  There's no colour. The status bar's picked value is Depth 0.90, Stencil 0x55.

## Unexpected

Nothing. The plan said Clear and ColorFill would be `directShaderWrite`. As on D3D11, which
upstream's shared test and the UI expect, they're modifications of their own instead. STATUS.md's
plan now says so.

## Not covered

- Why fragments failed, the fragments within a draw, shader outputs and primitive IDs: chunks 6.3
  to 6.5.
- Formats other than A8R8G8B8 and INTZ, mips and cube faces, and multisampled targets: chunk 6.6.
  The 1x1 copy is expected to fail for A8, L8 and L16 (6.1), which then give no colour.
- UpdateTexture and GenerateMipSubLevels into a render target: they're listed for every pixel of
  what they write, which isn't tested here. GenerateMipSubLevels' levels below the top can't be
  read anyway, since D3D9 doesn't give them out.
- A draw made while a state block is recorded, which is listed without being checked.
- Upstream's shared `Pixel_History` checks, which need 6.3 to 6.5: chunk 6.7.
- Other GPUs.
