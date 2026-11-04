# D3D9 phase 3e test (b19accc98)

Windows 11, RTX 4070 Ti, Development x64 (demos Release x64). Same setup as `d3d9_phase3_retest3.md`.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS after a fix**: renderdoc built with 0 errors. The demos failed on `d3d9_mesh_output.cpp`: C4245 on lines 312 and 355, and C4996 `fopen`, both treated as errors. Fixed in 09de6b175 |
| 1 | `d3d9_overlay_check.py` | **PASS**: 62 passed, 0 failed. The replay log has only the two expected warnings (quad overdraw, triangle size) |
| 1 | UI: overlays | **PASS, one note**: the overlays draw, and "save overlay instead" saves the overlay. Quad overdraw and triangle size leave the previous overlay showing (see below). Switching overlays and events doesn't change the target, the pipeline state or later events |
| 2 | `D3D9_Mesh_Output` native run | **FAIL** (demo): shader compile errors and a "Couldn't" for vs_1_1, see below. The native and captured runs wrote identical references |
| 2 | `d3d9_mesh_check.py` | **FAIL**: 11 passed, 6 failed, 1 skipped. Three of the failures come from the demo's shaders not compiling |
| 2 | UI: mesh output | **PASS**: VS Out columns, preview and vertex picking look right |
| 3 | Event Browser probe | Output below. It doesn't seem to reproduce the bug |
| 4 | `renderdoccmd test unit "[d3d9]"` | **PASS**: 6052 assertions in 13 test cases |
| 5 | `d3d9_replay_check.py` Simple_Triangle, Edge_Cases | **PASS**: both match last round's output, and both have alpha 255-255 |

## Overlays (UI)

Quad overdraw and triangle size don't show nothing. They keep showing whichever overlay was
selected before, at the current event, so the previous overlay stays over the target. This may
be intended (no texture returned, so the viewer keeps its last one), but it looks like a working
overlay. Clearing the overlay, or showing nothing, would be clearer.

Expected, as on other APIs:
- **NaN/INF/-ve display:** turns the target black and white and draws nothing else. This capture
  probably has no NaNs, infinities or negative values.
- **"Save overlay instead"** saves just the overlay. It writes nothing for NaN/INF/-ve, histogram
  clipping, quad overdraw or triangle size. The first two are display modes, not overlay
  textures, and the last two have no texture on D3D9.

## D3D9_Mesh_Output demo log

These lines are the same natively and under RenderDoc:

```
d3d9_test.cpp:273 Error: Failed to compile shader, error 80004005 / (6,5-9): error X4509: invalid register semantic 'i0', or variable must be bound to multiple register banks (c register binding required)
d3d9_test.cpp:273 Error: Failed to compile shader, error 80004005 / (4,5-9): error X4509: invalid register semantic 'i0', or variable must be bound to multiple register banks (c register binding required)
d3d9_mesh_output.cpp:397 Warning: Couldn't set up vs_1_1 on the software vertex processing device
d3d9_mesh_output.cpp:1215 Log: Wrote the reference to E:\Projects\renderdoc\util\test\D3D9_Mesh_Output_reference.txt
d3d9_mesh_output.cpp:1229 Error: Couldn't create the resources for vs_1_1
```

- **The `int count : register(i0)` declarations don't compile** with this machine's compiler. They're
  in the "vs_2_0 flow control" shader, which "vs_2_0 DrawPrimitiveUP" also uses, and in
  "vs_3_0 loops and texldl".
- **The hand-assembled vs_1_1 is rejected** on both the software device (Bind fails, so no reference)
  and the main device. Its tokens are probably rejected by CreateVertexShader.

No "ProcessVertices failed" lines.

## Mesh check failures

| Draw | Outputs that differ |
|---|---|
| vs_2_0 flow control | No VS Out TEXCOORD.1 at any vertex. Ours has the fixed function columns (COLOR.0, COLOR.1, POSITION, TEXCOORD.0), because the shader didn't compile and the draw ran without one. **Invalid test** (demo bug above) |
| vs_2_0 DrawPrimitiveUP | Same as vs_2_0 flow control. **Invalid test** |
| vs_3_0 loops and texldl | Same reason, so the position, TEXCOORD.0 and TEXCOORD.1 differ from the C++ reference. Example, row 0: POSITION ours (-1, -1, -0.25, 1), ref (-1.29904, -1.73205, 4, 4.8). **Invalid test** |
| FFP spot light | See below |
| FFP texgen and fog | See below |
| FFP point size | See below |

**FFP spot light:** only vertex 4, the grid's centre (rows 5, 7, 10, 14, 15 and 18), differs. Every
other vertex passes.

| Output | Ours | Reference |
|---|---|---|
| COLOR.0 | (0.721742, 0.655517, 0.270273, 0.784314) | (0.803922, 0.737255, 0.290196, 0.784314) |
| COLOR.1 | (0.35, 0.35, 0.7) | (0.396078, 0.396078, 0.796078) |

- The vertex is inside the inner cone: its cos α to the spot direction is 0.9938, and cos(θ/2) is
  0.9801. So ours applies full intensity: COLOR.1 is light specular × material specular = 0.35.
- The reference is about 1.13× that. This matches the falloff formula without the clamp to 1
  inside θ/2: ((0.9938 − cos 0.6) / (cos 0.2 − cos 0.6))^1.5 = 1.088^1.5 ≈ 1.136.
- So ProcessVertices doesn't clamp the spot factor to 1 inside the inner cone. Whether the hardware
  pipeline clamps it isn't known from this.

**FFP texgen and fog:** TEXCOORD.1–4 are 0 in the reference at all 24 rows. Ours has values, for
example row 0:

| Output | Ours |
|---|---|
| TEXCOORD.1 | (0.25, -0.5, 2) |
| TEXCOORD.2 | (0.104117, -0.276172, -0.955452) |
| TEXCOORD.3 | (0.668524, 0.0529858) |
| TEXCOORD.4 | (-0.823706, -1, 4.38032) |

The FVF has one texture coordinate set (`D3DFVF_TEX1`), and ProcessVertices apparently doesn't
generate the coordinates for stages 1–4. TEXCOORD.0 and FOG pass. TEXCOORD.0 is the camera space
reflection vector without the local viewer, with a texture transform, and FOG is range fog. So
`NonLocalViewer` = (0, 0, -1) agrees with ProcessVertices for the reflection vector.

**FFP point size:** PSIZE.0 is 50 in ours (the `POINTSIZE_MAX` clamp) and 0 in the reference at
all 6 vertices. ProcessVertices doesn't seem to write PSIZE here. COLOR.0 and FOG pass.

## Event Browser probe

The font scale is 1 (`Font_GlobalScale` in UI.config). Built with `build.bat` from an x64 VS 2022
environment and run as `out\richtext_probe.exe 1 out`:

```
Qt 5.9.4, font 'MS Shell Dlg 2' 8.00pt (scale 1.00), devicePixelRatio 2.00, logical DPI 72
font height 10, ascent 8, descent 2, lineSpacing 10; row height 18
RichResourceTextHeightHint (1 line) = 14

plain row text ink: rows 8..15 of 0..17

current: line-height 14px                        link             doc height  20  ink   2.. 18  CLIPPED, -1 px from plain row
current: line-height 14px                        params, no link  doc height  18  ink  10.. 17  OK     , +2 px from plain row
no line-height                                   link             doc height  20  ink   2.. 16  OK     , -2 px from plain row
no line-height                                   params, no link  doc height  14  ink   8.. 15  OK     , +0 px from plain row
line-height = font lineSpacing px                link             doc height  20  ink   2.. 16  OK     , -2 px from plain row
line-height = font lineSpacing px                params, no link  doc height  14  ink   8.. 15  OK     , +0 px from plain row
line-height 14px, -qt-line-height-type minimum   link             doc height  20  ink   2.. 18  CLIPPED, -1 px from plain row
line-height 14px, -qt-line-height-type minimum   params, no link  doc height  18  ink  10.. 17  OK     , +2 px from plain row
line-height 100%                                 link             doc height  20  ink   2.. 16  OK     , -2 px from plain row
line-height 100%                                 params, no link  doc height  14  ink   8.. 15  OK     , +0 px from plain row
no table, image vertical-align middle            link             doc height  16  ink   1.. 15  SHIFTED, -3 px from plain row
no table, image vertical-align middle            params, no link  doc height  10  ink   8.. 15  OK     , +0 px from plain row
(5.14+ only) 14px, forced fixed                  link             doc height  18  ink   5.. 19  CLIPPED, +1 px from plain row
(5.14+ only) 14px, forced fixed                  params, no link  doc height  18  ink   9.. 16  OK     , +1 px from plain row

A variant works if both its rows say OK: the text fits in the row and is within 2px
of where a plain row's text is. The PNGs show each row under a plain one.
```

Variants 1 (no line-height), 2 (line-height = lineSpacing) and 4 (100%) say OK on both rows.

**The probe probably doesn't match qrenderdoc's conditions:**
- It rates the current params row OK (+2 px), but in qrenderdoc with parameter names on, every
  params row is clearly shifted and clipped (retest 3).
- Its text is very small (8pt at logical DPI 72, font height 10), much smaller than qrenderdoc's
  Event Browser text on this machine.
- Its devicePixelRatio is 2.00, so its DPI handling likely differs from qrenderdoc's.

The PNGs are in `d3d9_phase3e_probe/`: `variant0_link.png`, plus the link and params PNGs of
variants 0, 1, 2 and 4.
