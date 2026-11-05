# D3D9 phase 3 retest (HEAD `ea95bb75e`)

Retest of the four fixes that replaced the phase 3 workarounds, the `_first.png` change, and the
disassembly changes. Same machine as `d3d9_phase3_report.md`: RTX 4070 Ti, driver 610.88
(32.0.16.1088), Windows 11 Pro 10.0.26300, VS 2022 Community, scripts in qrenderdoc's embedded
Python 3.6.

## Summary

| # | Item | Result |
|---|---|---|
| 1 | Build Development x64 | **PASS**: 0 errors and no fixes, so `d3d9_phase3_fixes.patch` would be empty and wasn't written |
| 2 | Recapture every D3D9 demo | **PASS**: 8 captures (Reset frames 30 and 61 included), no errors in the demo logs |
| 3 | 3a–3d Python checks | **PASS**: the same as with the workarounds, apart from the expected changes below |
| 3a | `_first.png` is the previous frame with the overlay | **PASS** |
| 3a | Edge_Cases depth values at event 0 | **PASS**: identical to the earlier run |
| 3a | Clear `copyDestination` | **PASS**: render target 0, or the depth stencil for a depth-only clear |
| 4 | fxc comparison: the three differences are gone | **PASS**: 23/23 identical to fxc |
| 4 | ps_1_4 `texcrd`/`texld` with `.xyz`/`.xyw` | **FAIL**: fxc prints all four letters (`.xyzz`, `.xyww`) |
| 5 | `renderdoccmd test unit "[d3d9]"` | **PASS**: "All tests passed (5331 assertions in 2 test cases)", exit 0 |
| 6 | `D3D9Ex_Wrapping_Check` 10 more times | **PASS**: 10/10 clean, no crash |
| UI | The checklist from `d3d9_phase3_report.md`, done by hand | **PASS**, except Save Texture as PNG (**FAIL**: alpha is 0) and two layout issues (see "UI checks") |

One new observation (not a failure): in one of three Reset recaptures, frame 30's back buffer and
depth stencil kept their pre-reset IDs (see item 2).

**Correction to `d3d9_phase3_report.md`:** that report passed 3b's Python `SaveTexture` PNG, but
the file has alpha 0 everywhere, the same bug as Save Texture in the UI (see "UI checks"). I had
only checked that the save succeeded and the file size.

## 1. Build

`renderdoc.sln` Development | x64 built with 0 errors and no source changes.

## 2. Recapture

All 7 demos were recaptured with this build, through the same script as last time:

| Capture | Size (bytes) |
|---|---|
| D3D9_Simple_Triangle_frame30842 | 32766 |
| D3D9_Wrapping_Check_frame35923 | 23358 |
| D3D9Ex_Wrapping_Check_frame42274 | 23070 |
| D3D9_Capture_Edge_Cases_frame83 | 14210380 |
| D3D9_Frame_Start_State_frame31865 | 33125 |
| D3D9_Resource_Contents_frame22895 | 39619 |
| D3D9_Reset_Capture_capture (frame 30) | 15923 |
| D3D9_Reset_Capture_frame61 | 31327 |

The demo logs have no errors.

**Observation, Reset frame 30 IDs.** In this recapture, frame 30's back buffer and depth stencil
are `Backbuffer 17` and `Auto Depth Stencil 16`, the IDs from before the reset. The earlier round
had `Backbuffer 23` and `Auto Depth Stencil 22`, the new objects. The chunk order differs too:
`IDirect3DSwapChain9::GetBackBuffer` comes right after DeviceInitialisation (chunk 2) instead of
after CreateVertexBuffer (chunk 6). I recaptured Reset twice more, and both runs gave 22/23 and the
old chunk order, so it varies from run to run. This commit doesn't touch capturing. Both kinds
replay correctly: "After the reset" > Clear (blue) > DrawPrimitive, with rt0 the back buffer.
Frame 61 had 22/23 in all three runs. The 16/17 capture is kept (see Files).

## 3. Python checks

I ran `d3d9_replay_check.py --all`, `d3d9_pipestate_check.py`, and my 3b/3c scripts (`tex_check.py`,
`pipe_extra.py`) on all 8 captures, then diffed each output against the run with the workarounds,
with paths, frame numbers and timings normalised. Every capture loads, and `--all` steps through
every action, all with exit code 0.

Expected differences:
- **The label:** "At the first event (1)" is now "At the start of the frame (event 0)", in every
  capture.
- **Edge_Cases was captured on an odd frame (83, previously 62).** Its per-frame alternation shows:
  - the alpha ref is 0x10 instead of 0x20 at the events before the state block region, and 0x20
    instead of 0x10 at event 81;
  - the span texture (`Texture 31`) is green instead of blue, so the back buffer's maximum at the
    last event is (1, 1, 0.2, 1) instead of (1, 1, 1, 1);
  - the ReleaseDC surface values swap between the first and last event.

  These are the demo's two variants, not a change in RenderDoc.
- **Resource_Contents:** the recycled textures depend on the frame number. They are now
  `Texture 22932`/`22933` = #C7C76E/#C8C86F, previously 21388/21389 = #B9B966/#BABA67. As before,
  the new texture is one step above the old one.
- **Reset frame 30 IDs:** see item 2.

Everything else is identical, including every texture value in Simple_Triangle,
Resource_Contents and Edge_Cases, and every value in the D3D9State dumps.

### `_first.png` — PASS

`_first.png` is now the previous frame with the overlay:
- **Simple_Triangle:** grey background, both triangles, and the overlay text.
- **Reset frame 61:** frame 60's blue background with the triangle and overlay.
- **Edge_Cases:** the overlay over the previous frame's red and green blocks.

`_last.png` is unchanged: no overlay, and Reset frame 61 is red.

### Edge_Cases depth at event 0 — PASS

These are identical to the run with the workarounds:

| Depth stencil | Values |
|---|---|
| Auto Depth Stencil 16 | 0xFFFFFF00 ×100000, 0x4000005A ×10000, 0xBFFFFFA5 ×10000 |
| Depth Stencil 20 (MSAA), samples 0–1 | 0xFFFFFF00 ×2080, 0x80000033 ×2016 |
| Depth Stencil 20 (MSAA), samples 2–3 | 0x80000033 ×2080, 0xFFFFFF00 ×2016 |
| Depth Stencil 23 (D16) | 0x7FFF ×1024 |
| Depth Texture 24 (INTZ) | 0x5FFFFF0F ×1024 |

### Clear `copyDestination` — PASS

Every Clear in the demos clears colour, so each one's `copyDestination` is render target 0:

| Capture | Clears |
|---|---|
| Simple_Triangle | event 1 (colour and depth stencil) → Backbuffer 17 |
| Frame_Start_State | event 1 (colour and depth stencil) → Backbuffer 17 |
| Resource_Contents | event 1 (colour and depth stencil) → Backbuffer 17 |
| Edge_Cases | event 3 → Backbuffer 17; event 35 (MSAA) → Render Target 19 |
| Wrapping_Check | event 1 → Backbuffer 60 |
| D3D9Ex_Wrapping_Check | event 1 → Backbuffer 59 |
| Reset frame 30 | event 2 → Backbuffer 17 |
| Reset frame 61 | event 2 → Backbuffer 23 |

In each case `outputs` and `depthOut` are the targets bound at the time.
Resource_Contents' `ColorFill` (event 51) keeps Render Target Texture 31 as its destination.

None of the demos clears only depth or stencil, so I tested that in the scratch shader app (item 4).
I added an automatic D24S8 depth stencil and three more Clears after the colour one:
- `Clear(ZBUFFER, 0.5)`
- `Clear(STENCIL, 7)`
- `Clear(ZBUFFER | STENCIL, 0.25, 9)`

All three are listed with ClearDepthStencil|Clear and `copyDestination` = Auto Depth Stencil 16. The
colour Clear has Backbuffer 17.

## 4. Disassembly against fxc

The same scratch app, with the 23 shaders from the earlier round plus one new ps_1_4 shader for the
`texcrd`/`texld` selectors, was captured with this build. The reference is `D3DDisassemble`, which
gives the same output as `fxc /dumpbin /Fc`; I checked again with fxc.exe from SDK 10.0.26100.0.

**The 23 shaders from the earlier round are 23/23 identical to fxc (PASS).** That holds even when
leading whitespace is compared exactly. The lines that differed last time:
- `combine.ps_3_0`: `texldl r0, r0.xyww, s0`
- `asm.vs_1_1`: `mov oT1.xy, v2.yxxx`
- `asm11.ps_1_1`:
  - `  + mov r0.w, t0_bias.w`, with the `+` two columns to the left;
  - `sub_d2 r1.w, t1.w, 1-v0.w`
- `asm14.ps_1_4`: `  + mov_d4 r0.w, r4.w`

The constant table comments differ in formatting only, as before: the Name column padding and
"// N instructions". The D3DX-only `o1[x0.x], aL` quirk in asm.vs_3_0 is unchanged.

**ps_1_4 `.xyz`/`.xyw` selectors: FAIL.** The new shader (`asm14sel.ps_1_4`, assembled with
D3DXAssembleShader):

```
ps_1_4
def c0, 0.25, 0.25, 0.25, 1.0
texcrd r0.rgb, t0.xyz
texcrd r1.rgb, t1.xyw
texld r2, t2.xyz
texld r3, t3.xyw
texcrd r4.rg, t4_dw.xyw
texld r5, t5
phase
texld r0, r0.xyz
texld r1, r1_dz.xyz
add r0.rgb, r0, r1
add r0.rgb, r0, r2
add r0.rgb, r0, r3
add r0.rg, r0, r4
mad r0.rgb, r0, c0, r5
+mov r0.a, c0.a
```

| fxc (`D3DDisassemble` and `fxc /dumpbin`) | RenderDoc |
|---|---|
| `texcrd r0.xyz, t0.xyzz` | `texcrd r0.xyz, t0.xyz` |
| `texcrd r1.xyz, t1.xyww` | `texcrd r1.xyz, t1.xyw` |
| `texld r2, t2.xyzz` | `texld r2, t2.xyz` |
| `texld r3, t3.xyww` | `texld r3, t3.xyw` |
| `texcrd r4.xy, t4_dw.xyww` | `texcrd r4.xy, t4_dw.xyw` |

Everything else in that shader matches, including the following:
- `texld r5, t5` (identity);
- phase 2 `texld r0, r0.xyzz` and `texld r1, r1_dz.xyzz`, where the source is r#, so the exception
  doesn't apply;
- `  + mov r0.w, c0.w`.

So fxc has no ps_1_4 exception: it prints one letter for a replicate swizzle and all four
otherwise, for t# too. Removing the `ps_1_4 && AddrOrTexture` branch from `SwizzleString`
(`d3d9_bytecode.cpp`) would make all 24 shaders match. I haven't changed it.

Assembler limits I hit while writing the shader, for reference:
- `texcrd` with `_dw` needs an `.rg` destination;
- `texld` with `_dz` is only allowed on r# sources;
- a `_dz` before `phase` is an error when the shader has a `phase`.

**Reflection and constants:** `sh_check.py` gives the same output as last round, apart from the
resource IDs, which moved because of the added depth stencil and shader. The new shader reflects as
TEXCOORD0–5 inputs (used masks 0x7, 0xb, 0x7, 0xb, 0x3, 0xf), COLOR0 output, c0, and samplers
s0–s3 and s5.

## 5. Unit tests — PASS

`x64\Development\renderdoccmd.exe test unit "[d3d9]"`: "All tests passed (5331 assertions in 2 test
cases)", exit 0.

## 6. D3D9Ex_Wrapping_Check, 10 more runs — PASS

I ran `pipe_extra.py`, the script that crashed the one time it happened, 10 times on the new
D3D9Ex_Wrapping_Check capture, each run in a new qrenderdoc process. Each run loads the capture,
calls SetFrameEvent and GetD3D9PipelineState at every event, and dumps the D3D9State.

All 10 exited 0 with identical output. There are no Application Error events for qrenderdoc since
the start of the runs. Counting all rounds, the null-call crash has happened once in 18 loads.

## Logs

None of the runs logged an error or a warning. In particular, there were no "Couldn't write the
contents", "Couldn't decode", "Couldn't copy", "debug shader" or "Couldn't fully parse shader"
messages. The `d3d12_replay.cpp` assertion seen once last round didn't come back.

## UI checks

These were done in qrenderdoc, with this build and the captures from item 2. The steps follow the
checklist at the end of `d3d9_phase3_report.md`.

### Failures and issues

**1. FAIL: Save Texture as PNG writes alpha 0.** This affects both the UI and Python's `SaveTexture`.

- **Steps:** open Simple_Triangle, select the last event, open Backbuffer 17 and use Save Texture to
  save a PNG.
- **What you get:** a valid 400×300 RGBA PNG with the right colours (0x333333 background, green and
  magenta triangles), but every alpha byte is 0, so viewers show it as fully transparent or blank.
  The UI's file is byte-for-byte identical to the one Python saved last round with
  `alpha = AlphaMapping.Discard`.
- **Likely cause:** the back buffer is `D3DFMT_X8R8G8B8`. It's mapped to
  `DXGI_FORMAT_B8G8R8X8_UNORM` (`d3d9_replay.cpp:44`), which is a 4-component BGRA8
  ResourceFormat. The texture data returned for it is the raw bytes, where X is 0.
  `SaveTexture` writes the source alpha for PNG, since the alpha mapping is only applied for BMP
  and JPG (`replay_controller.cpp:1125`). Picking and display show alpha 1 because they sample
  the X8 format.
- **Suggested fix:** fill the X bits with 1 in D3D9's texture data readback, for
  `X8R8G8B8`, `X8B8G8R8`, `X1R5G5B5` and `X4R4G4B4`. The last two are mapped to formats with
  alpha (`d3d9_replay.cpp:49`, `:51`).

**2. Layout: the D3D9 Pipeline State panel.**
- **The cube:** a large wireframe cube image sits in the toolbar area, next to Show Unused Items,
  Show Empty Items and Export. It takes up about a fifth of the panel's height.
- **The topology diagram:** three triangles sit at the far right of the IA page, under the Render
  States box, at about the height of the declaration row. D3D11 shows its diagram small, next to
  the Topology label.

**3. Event Browser rows with resource links are clipped.** In Edge_Cases, rows 84
`ReleaseDC(Offscreen Surface 32)` and 85 `GetFrontBufferData(Offscreen Surface 33)` are drawn
shifted down and clipped, so they're barely readable. The other rows are fine. It isn't known
whether D3D11 or Vulkan captures do the same on this machine, so this may be a general qrenderdoc
problem rather than a D3D9 one.

**4. Minor: the VS raw constants view doesn't default to `float4`.** On the VS page, the Buffer
link "VS float constants (c#)" opens the raw buffer viewer without the `float4` format, so the
format has to be switched by hand. The PS float constants, opened with Go, fall back to the same
viewer and already show `float4 c;`.

### Passed

**3a**
- All 8 captures open as D3D9.
- The Event Browser shows the regions, markers, Clears, draws, copies and presents. The content
  matches; see issue 3 for the clipped rows.
- The Resource Inspector lists the resources and opens them.

**3b**
- Simple_Triangle:
  - the background picks as 0.2, 0.2, 0.2, 1;
  - zoom, flip, channel toggles, range, auto-fit and the histogram work;
  - the thumbnail strip shows the back buffer;
  - stepping 1 → 11 → 19 adds the triangles, and returning to the last event looks the same;
  - selecting the Clear (event 1) shows Backbuffer 17.
- Mesh viewer, VS Input at event 11: the 3 vertices are listed and the 3D view shows the triangle.
- Edge_Cases at Capture Start: every depth and stencil value in the table matches, including the
  MSAA samples, D16 and INTZ. The range is 0.25–1.0, and selecting Clear 35 shows Render Target 19.

**3c**
- The panel has IA, VS, RS, PS, OM and Render States pages. Export to HTML matches the panel.
- Simple_Triangle event 11:
  - TriangleList;
  - the declaration is POSITION0 FLOAT3 at +0, COLOR0 FLOAT4 at +12 and TEXCOORD0 FLOAT2 at +28;
  - Vertex Buffer 21 has stride 36 and byte length 108, and double-clicking it opens the 3
    vertices;
  - the vs_3_0 and ps_3_0 rows open the disassembly.
- **Constants:** with Show Unused Items, the counts are 256/16/16 (VS) and 224/16/16 (PS). The rows
  the shader doesn't read are in italics.
  - The VS Go arrow opens the constant viewer with only the registers the shader declares (c0 on
    Frame_Start_State).
  - The PS has no reflected block, so its Go arrow opens the raw buffer.
  - Frame_Start_State: c10 = (1,2,3,4), c200 = (13,14,15,16), c20 = (9,10,11,12) and
    c180 = (17,18,19,20) through the raw view. The integer and boolean constants are right.
- **Italics:** the fixed-function tables are in italics at event 11, where shaders are bound, and
  not at event 19, the fixed-function draw.
- Frame_Start_State: the transforms, material, lights (7 enabled, and 42 with Show Unused Items),
  samplers (PS s3 anisotropy 7, VS sampler 1 Mirror), texture stages (stage 0 Constant, stage 5
  TexCoord 3), fog, clip plane 1, scissor, stencil ref and the raw render states all match.
- Edge_Cases OM:
  - the stencil states at events 22, 31 and 44 match;
  - the targets at 52 and 54 match;
  - event 81 shows Texture 31 in the PS textures table and the inputs strip, and double-clicking it
    opens it;
  - the Texture Viewer follows the bound outputs.
- With Show Empty Items, empty sampler slots are red and in italics, the same as D3D11.

**3d**
- The Simple_Triangle shaders open from the VS and PS rows.
- The scratch capture:
  - the depth-only Clears at events 2–4 show Auto Depth Stencil 16;
  - the shader viewer's inputs, outputs and constants panes are filled in;
  - the uniforms constants show by name with the app's values;
  - the `def` literals c7 and c11 show in the constant viewer;
  - the `asm14sel.ps_1_4` disassembly shows the `.xyz`/`.xyw` difference from item 4.
- SysValue is Undefined for every attribute except vs oPos (Position), PS VPOS/VFACE and the PS
  colour outputs. That's correct for D3D9.

## Still open

- The `GetCBufferVariableContents` length-0 behaviour is unchanged (not retested).

## Files (test machine only)

`<scratch>` = `C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3`

- `<scratch>\cap\<demo>\`: the new captures and every script output (`_replay`, `_pipe`, `_pipex`,
  `_tex`, `_clears`, `_exrun1`–`10`), plus the PNGs.
- `<scratch>\cap_wa\`: the previous round's captures and outputs, with the workarounds.
- `<scratch>\reset_runs\r1`, `r2`: the two extra Reset recaptures.
- `<scratch>\sh\`:
  - `shtest.cpp`: the scratch app, now with the depth stencil and depth-only Clears;
  - `src\asm14sel.ps_1_4.asm`: the new shader;
  - `out\`: RenderDoc and D3DDisassemble output;
  - `fxc\asm14sel.ps_1_4.dumpbin.asm`: fxc.exe's output;
  - `compare_out_retest.txt`, `sh_check_out_retest.txt`, `clears_out.txt`.
