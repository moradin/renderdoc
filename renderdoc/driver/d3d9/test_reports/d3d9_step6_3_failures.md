# Step 6.3: why a draw's fragments failed

Step 6.3 of the plan (STATUS.md, "Step 6 plan"): pixel history says why a listed draw's fragments at
the pixel failed: culled, clipped by a user clip plane, scissored, sample masked, discarded by
texkill or the alpha test, or failing the depth or stencil test. How it works is in STATUS.md under
"Pixel history". Tested on the commit that adds it (its parent is bc03802bd), on Windows 11 with an
RTX 4070 Ti (driver 32.0.16.2124).

Files in `d3d9_step6_3_failures/`:
- `tests_x64.txt`, `tests_x86.txt`: the automated tests' output, on the 64-bit and the 32-bit demos.
- `stages.py` and its output `native_stages.txt`: the native run's counts for the draws made to
  fail, read from the demo's reference file.
- `hist_dump.py` and its output `hist_dump.txt`: pixel histories of the test's capture with each
  draw's failure and how long each took, printed through the python bindings.
- `ui_driver.py`: drives qrenderdoc from its UI thread for the screenshots (the same as 6.2's).
- `01_depth_stencil_failures.png`, `02_clip_plane.png`, `03_sample_mask.png`,
  `04_alpha_test_ff.png`: the screenshots.

## What changed

- `d3d9_pixelhistory.cpp`: for each draw the occlusion query lists, the query is repeated with the
  pipeline's stages turned back on one after another, in the order the GPU runs them, each with
  the ones before it:
  1. culling: `backfaceCulled`;
  2. user clip planes: `viewClipped`;
  3. the scissor test, decided on the CPU: `scissorClipped`;
  4. the sample mask: `sampleMasked`;
  5. the pixel shader with the alpha test and alpha to coverage: `shaderDiscarded`;
  6. the depth test: `depthTestFailed`;
  7. the stencil test, two-sided too: `stencilTestFailed`.

  The first stage that leaves nothing at the pixel is the reason. A stage that's off in the draw's
  state isn't queried, and nothing is written.
- `qrenderdoc/Windows/PixelHistoryView.cpp`: the pixel history window shows `viewClipped`, as "View
  clipped". No API set it before, so the window didn't show it. This is outside the D3D9 driver,
  and listed in the README.
- The demo `D3D9_Pixel_History` has a "D3D9 Tests" section: draws made to fail one test at one probe
  and pass at another. There's also a third, 4x multisampled batch with only the clear and those
  draws. Its native run records what each draw leaves at each probe as each stage is turned on.
- The test `D3D9_Pixel_History.py` checks every listed draw's failure flags.

## The demo, and what the GPU did natively

Each failure test draws two 12x12 squares in a column, from x = 20 every 20 pixels. The top one
(y 262 to 274) is made to fail; the bottom one (y 276 to 288) is made to pass:
- **"Alpha Test FF":** fixed function, pretransformed, alpha 0x40 and 0xC0, alpha test GREATER than
  0x80.
- **"Alpha Test":** the same with vs_3_0 and ps_3_0, alpha 0.25 and 0.75.
- **"Texkill ps_1_1":** `texkill t0`, where t0 is the vertex alpha minus 0.5 (D3DAssemble, since
  the compiler no longer makes ps_1_x).
- **"Texkill ps_2_0":** `clip()` of the same.
- **"Two Sided Stencil":** two-sided stencil, ALWAYS for clockwise and NEVER for counter-clockwise
  faces, culling off. The top square is wound counter-clockwise.
- **"Clip Plane":** a user clip plane keeping clip space y below the pixel row 275.
- **"Sample Mask 0"** and **"Sample Mask 5":** `MULTISAMPLEMASK` 0 and 0x5, both squares.

The probes are the centres of the squares, 16 more than 6.2's 24. The demo's existing draws
already give culling ("Cull Front"), the scissor test ("Scissor Fail"), a ps_3_0 discard
("Background" at (150, 250)), depth failures and stencil failures.

Natively the third frame records each probe after every event. For each draw it records the
samples the draw leaves at the probe, from an occlusion query with a one pixel scissor rect and
writes off, as each stage is turned on in turn: nothing, then culling, clip planes, the scissor
rect (the draw's own, intersected with the pixel), the sample mask, the shader with the alpha test,
the depth test, and the stencil test. These are the GPU's numbers, on the demo's own state, not
replayed. From `native_stages.txt` (the samples after each stage, then the first stage that left
none):

```
Basic        Cull Front         (170, 160) [1, 0, 0, 0, 0, 0, 0, 0]         culled
Basic        Simple Test        (170, 140) [1, 1, 1, 1, 1, 1, 1, 0]         stencil
Basic        Scissor Fail       (106, 248) [1, 1, 1, 0, 0, 0, 0, 0]         scissor
Basic        Alpha Test FF      (26, 268)  [1, 1, 1, 1, 1, 0, 0, 0]         shader
Basic        Texkill ps_1_1     (66, 268)  [1, 1, 1, 1, 1, 0, 0, 0]         shader
Basic        Two Sided Stencil  (106, 268) [1, 1, 1, 1, 1, 1, 1, 0]         stencil
Basic        Clip Plane         (126, 268) [1, 1, 0, 0, 0, 0, 0, 0]         clip plane
Basic        Sample Mask 0      (146, 268) [1, 1, 1, 1, 1, 1, 1, 1]         passed
Multisampled Sample Mask 0      (146, 268) [4, 4, 4, 4, 0, 0, 0, 0]         sample mask
Multisampled Sample Mask 5      (166, 268) [4, 4, 4, 4, 2, 2, 2, 2]         passed
```

Every draw failed where it was made to fail and passed where it was made to pass. Two things were
new:
- **The sample mask does nothing on single sampled targets.** A mask of 0 still draws there: the
  probe is the draw's orange (e57f19) afterwards. On the 4x target it leaves the clear colour, and
  0x5 leaves 2 of the 4 samples.
- **Without a depth stencil** (the second and third batches) the stencil tests can't fail, so the
  two-sided stencil draw passes there.

## Automated tests

`D3D9_Pixel_History` now checks, on top of 6.2's checks, every listed event's failure flags:
- **Draws:** the flags are exactly the first stage that left nothing natively, or none.
  `depthClipped`, `depthBoundsFailed` and `predicationSkipped` are never set.
- **Clears and copies:** no flags.
- **The demo's draws:** a table in the test says how each one is made to fail at its probes, per
  batch. The native counts have to agree with it, so a draw that stops testing what it's meant to
  fails the test rather than passing quietly.

From `tests_x64.txt`:

```
** Basic: 80 pixel histories list 773 events, each with the colour, depth and stencil the GPU had natively before and after it (depth within 0), in 6.4s
** Basic: the draws listed failed as they did natively: backfaceCulled 2, depthTestFailed 10, passed 662, scissorClipped 1, shaderDiscarded 6, stencilTestFailed 4, viewClipped 1
** Basic: at 'Simple Test', only the events up to it are listed
** Colour Only: 40 pixel histories list 398 events, each with the colour, depth and stencil the GPU had natively before and after it (depth within 0), in 3.1s
** Colour Only: the draws listed failed as they did natively: backfaceCulled 1, passed 344, scissorClipped 1, shaderDiscarded 5, viewClipped 1
** Colour Only: at 'Simple Test', only the events up to it are listed
** Multisampled: 40 pixel histories list 56 events, each with the colour, depth and stencil the GPU had natively before and after it (depth within 0), in 0.7s
** Multisampled: the draws listed failed as they did natively: passed 9, sampleMasked 2, shaderDiscarded 4, viewClipped 1
...
## Tests complete summary: 20 passed out of 20 run from 211 total in 0:01:52.938816 ##
```

Basic's counts include the depth stencil's histories, where a culled or depth failing draw is
listed again. The multisampled batch's values are resolved, from the 1x1 copy, and match the native
run's whole-target resolve exactly.

All 20 D3D9 tests (and `D3D11_D3DPERF_Markers`) pass on x64 and on the 32-bit demos
(`tests_x86.txt`, 20 of 20, the captures recorded as `Windows x86 32-bit`). The x86 run's times are
about double because qrenderdoc was replaying at the same time. The `[d3d9]` unit tests pass:
20184 assertions in 26 test cases.

`hist_dump.txt` has the histories of the failing draws' probes. At (170, 140) and (170, 160) they
are what upstream's shared `Pixel_History` test expects of D3D11:
- (170, 140): "Background" fails the depth test and "Simple Test" the stencil test.
- (170, 160): "Cull Front" is culled and "Simple Test" fails the depth test.

The pixel the 300 draws cover lists 301 events in 0.48 s, up from 0.46 s in 6.2. Its draws cull
and test depth, so each needs three more queries.

## Builds

`renderdoc.sln` and `demos.sln`, Development x64 and x86: 0 warnings, 0 errors. Changed files are
formatted with clang-format 15.

## The UI (RenderDoc)

qrenderdoc on the test's capture, driven by `ui_driver.py` on the UI thread, with no mouse or
keyboard input. As in 6.2, the window is opened through `ViewPixelHistory` and `SetHistory`, as the
History button does. Failed modifications are the red rows, with the reason under the call:
- `01_depth_stencil_failures.png`: Basic, (170, 140). EID 138 "Background" says "Depth test
  failed" and EID 1138 "Simple Test" says "Stencil test failed". The earlier draws passed (green).
- `02_clip_plane.png`: Basic, (126, 268). EID 1678 "Clip Plane" says "View clipped", the new line.
  The pixel stays "Background"'s magenta.
- `03_sample_mask.png`: Multisampled, (146, 268). EID 3732 "Sample Mask 0" says "Masked by
  SampleMask", and the pixel keeps the clear colour. In the image, column 6's squares are missing
  and column 7's (mask 0x5) are a darker orange.
- `04_alpha_test_ff.png`: Basic, (26, 268). EID 1538 "Alpha Test FF" says "Shader executed a
  discard", the UI's wording for the alpha test too.

## Unexpected

- The sample mask has no effect on single sampled targets. This was measured, and matches the
  documentation of `D3DRS_MULTISAMPLEMASK`. So `sampleMasked` only appears on multisampled targets.
- No flag is meant for user clip planes. `viewClipped` ("viewport clipping") is the closest, and
  the pixel history window didn't show it, since no API set it.

## Not covered

- **Draws with mixed results.** A draw is failed only if all its fragments at the pixel are. One
  with a fragment that passes and one that fails is "passed", until 6.4 splits it into fragments.
- **A chosen sample.** Whether `sub.sample` itself was masked isn't checked: 6.6, with per sample
  values. The test reads the multisampled target with sample 0, which gives the resolve.
- **Untested state.** Alpha to coverage (NVIDIA's `ADAPTIVETESS_Y` switch) is restored with the
  shader stage, but no draw uses it. `D3DZB_USEW` isn't tested either.
- **Depth clipping** can't be turned off or reported (6.1).
- **Upstream's shared `Pixel_History` checks:** chunk 6.7.
- **Other GPUs.**
