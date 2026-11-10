# D3D9 step 8: alpha test, discard and oDepth in the overlays

Tested on the working tree on top of 5951292a2, which was committed as "D3D9: keep alpha test and
discards in the pass and fail overlays". Machine: Windows 11, RTX 4070 Ti. Files are in
`d3d9_step8_overlays/`.

## What changed

The depth, stencil, backface cull and viewport/scissor overlays used to replay the draw with a
constant colour pixel shader and with alpha test off. So pixels that the application's pixel
shader discards (`texkill`, `clip`) or that its alpha test rejects showed as drawn. The depth test
also used the rasterised depth instead of the shader's `oDepth`.

These four overlays now keep the application's pixel shader and alpha test. The shader's colour
can't be replaced, so they get pass and fail through blending instead:
- The overlay is cleared to white.
- The draw is replayed twice with ZERO/ZERO blending and a colour write mask. The "everything"
  pass clears alpha, and the "passing" pass clears red and green.
- Three full-screen quads then blend that into transparent, red and green.

`STATUS.md` ("Overlays") has the details and the reasons for this approach. Highlight drawcall,
wireframe and triangle size are unchanged, as on the other APIs.

## The test

`D3D9_Overlay_Test` has three new draws. Each draws over earlier quads, with ZFUNC LESS:
- **"Alpha Test":** a fixed function quad whose diffuse alpha goes from 0 to 1 across it, with
  ALPHAFUNC GREATEREQUAL 0x80. Its left half is alpha tested away. Its right half fails the depth
  test in the top half and passes in the bottom.
- **"Discard":** a vs_3_0 + ps_3_0 quad whose shader runs `clip(uv.x - 0.5)`.
- **"Shader Depth":** a ps_3_0 quad at depth 0.7 whose shader writes `oDepth` 0.3. It passes
  everywhere, but its own depth would fail in the top half.

`d3d9_overlay_check.py` checks each of the three:
- the Depth overlay at four points, the Backface Cull overlay at two and the Stencil overlay at
  two;
- a whole-image comparison: every pixel the Depth overlay shows in green has to be one that the
  draw changed in the target, and the other way round.

"Changed" comes from the replayed draw, which RenderDoc runs on the GPU: the target is read before
and after the draw's event. So the reference is what the GPU does with the draw, not a decoder or
a formula. There's no native readback of the overlay itself, since overlays only exist at replay.

## Results

### Automated tests, x64

All 16 passed:

```
## Tests complete summary: 16 passed out of 16 run from 207 total in 0:01:13.323133 ##
```

The 16 were D3D9_Wrapping_Check, D3D9_Texture_Formats, D3D9_Simple_Triangle, D3D9_Shader_Editing,
D3D9_Resource_Usage, D3D9_Resource_Contents, D3D9_Reset_Capture, D3D9_Overlay_Test,
D3D9_Mesh_Output, D3D9_Frame_Start_State, D3D9_Failed_Calls, D3D9_Custom_Shaders, D3D9_Counters,
D3D9_Capture_Edge_Cases, D3D9Ex_Wrapping_Check and D3D11_D3DPERF_Markers.

The x86 run wasn't needed: the change is replay-only, and the demo change is a new draw in an
existing demo.

### Other checks

- **Unit tests:** `[d3d9]` passed (6057 assertions in 13 test cases).
- **Builds:** `renderdoc.sln` and `demos.sln` built with 0 warnings and 0 errors, for both x64 and
  x86.

### The new checks, from the test run (the GPU's numbers)

```
Alpha Test:   Depth: green where the draw wrote, and only there: 6000 pixels written, 6000 green, 0 differ
Discard:      Depth: green where the draw wrote, and only there: 5880 pixels written, 5880 green, 0 differ
Shader Depth: Depth: green where the draw wrote, and only there: 28800 pixels written, 28800 green, 0 differ
```

Checking these counts against the demo's layout:
- **Alpha Test:** the quad is 100x240 pixels. The written area is its right half (50 pixels wide)
  in the bottom half (120 rows): 50 × 120 = 6000.
- **Discard:** the written area is 49 × 120 = 5880. The column at u = 0.5 is clipped too.
- **Shader Depth:** all of the 120x240 quad is written: 28800.

All the point checks passed as well:

| Draw | Overlay | Left half | Right half, top | Right half, bottom |
|---|---|---|---|---|
| Alpha Test | Depth | transparent | red | green |
| Discard | Depth | transparent | red | green |
| Shader Depth | Depth | green | green | green |

The Backface Cull and Stencil overlays leave out the same left halves, and show what's drawn in
green. The full check output is in `check_new_driver.txt`.

### The same capture with the old overlay code

To show that the new checks catch the old behaviour, I put back `d3d9_overlay.cpp` from
5951292a2, rebuilt, and ran the check script on the same capture. It failed exactly the 13 new
checks. The existing checks all passed:

```
133 passed, 13 failed
  FAIL Alpha Test Depth: green where the draw wrote, and only there: 6000 pixels written, 12000 green, 6000 differ
  FAIL Discard Depth: green where the draw wrote, and only there: 5880 pixels written, 12000 green, 6120 differ
  FAIL Shader Depth Depth: green where the draw wrote, and only there: 28800 pixels written, 14400 green, 14400 differ
```

The full output is in `check_old_driver.txt`. With the new code rebuilt, the same capture gives
146 passed, 0 failed (`check_new_driver.txt`).

The existing MSAA check also passed with the new code: the Depth overlay is green inside the
multisampled triangle, so the blending works on a multisampled target before the resolve.

## UI

I opened the capture in qrenderdoc and turned on overlays in the texture viewer. The screenshots
show the overlay over the back buffer:
- **`ui_1_alpha_test_depth_full.png`, `ui_2_alpha_test_depth.png`:** Depth Test at "Alpha Test".
  Only the quad's right half shows, red at the top and green at the bottom. The left half, which
  the alpha test rejects, shows the target underneath.
- **`ui_3_discard_depth.png`:** Depth Test at "Discard". Same, for the clipped half.
- **`ui_4_shader_depth_depth.png`:** Depth Test at "Shader Depth". The whole quad is green,
  because the shader's depth passes.
- **`ui_5_alpha_test_drawcall.png`:** Highlight Drawcall at "Alpha Test". The whole quad is shown,
  unchanged and as on the other APIs.

The debug message count was 0.

The `overlay_*_depth.png` files are the overlay textures that the check script saved. Transparent
pixels show as white or checkered, depending on the viewer.

## Not covered

- **Real games:** not tried with a real game's alpha-tested geometry.
- **Other alpha-test cases:** ps_1_x `texkill`, and alpha test together with a pixel shader
  (rather than fixed function), aren't in the demo. They take the same path: the application's
  shader and render states are left as they are.
- **NVIDIA's alpha to coverage switch (`D3DRS_ADAPTIVETESS_Y` = ATOC):** not tested. The overlay
  keeps it, as the draw has it.
- **Other GPUs:** only the RTX 4070 Ti was tested. The blending needs BLENDOP SUBTRACT and the
  destination alpha blend factors, which every shader model 3 GPU should have.
