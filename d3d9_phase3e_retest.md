# D3D9 phase 3e retest (3231359b3)

Windows 11, RTX 4070 Ti, Development x64 (demos Release x64). Same setup as `d3d9_phase3e_test.md`.
Both demos were captured again.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: renderdoc and the demos built with 0 errors and no fixes (nothing committed) |
| 1 | `d3d9_overlay_check.py` | **PASS**: 101 passed, 0 failed. The replay log's only warning is the expected one for quad overdraw |
| 1 | UI: overlays | **PASS**: at "Triangle Size", triangle size and triangle size (pass) show the strip in three colours: violet, dark purple and blue (`d3d9_phase3e_retest/triangle_size_strip.png`). Quad overdraw now shows the target with no overlay. Everything else is as STATUS.md describes |
| 2 | `D3D9_Mesh_Output` native run | **FAIL** (demo): the vs_1_1 shader is rejected, two shaders still don't compile, and the point readback of the blending draws covers 81 pixels, see below |
| 2 | `d3d9_mesh_check.py` | **FAIL**: 11 passed, 2 failed (FFP spot light, FFP texgen and fog) |
| 2 | UI: VS Out | **PASS, one note**: "FFP texgen and fog" shows the VS Out columns and the preview correctly (`d3d9_phase3e_retest/mesh_texgen_vsout.webp`). "vs_2_0 flow control" isn't in the capture, because the demo left it out. The note: the mesh viewer still has the "GS/DS Output" table and "GS/DS Out" preview tab, which D3D9 doesn't have |
| 3 | Event Browser | **FAIL**: link rows are clipped with every style. `line-height: 100%`, `none` and `10px` all fix the params rows. Log lines and details below |
| 4 | `renderdoccmd test unit "[d3d9]"` | **PASS**: 6052 assertions in 13 test cases |
| 5 | `d3d9_replay_check.py` Simple_Triangle, Edge_Cases | **PASS**: the output matches last round except for load times, and both have alpha 255-255 |

## D3D9_Mesh_Output demo log

The native run and the captured run wrote the same log and identical references. Every Error and
Warning line:

```
d3d9_mesh_output.cpp:723 Error: The vs_1_1 shader is rejected at instruction 21, 'mov oFog, v2.z'
d3d9_test.cpp:273 Error: Failed to compile shader, error 80004005 / (6,5-9): error X4509: invalid register semantic 'i0', or variable must be bound to multiple register banks (c register binding required)
d3d9_test.cpp:273 Error: Failed to compile shader, error 80004005 / (4,5-9): error X4509: invalid register semantic 'i0', or variable must be bound to multiple register banks (c register binding required)
d3d9_mesh_output.cpp:1413 Error: Leaving out vs_1_1: its shader didn't compile
d3d9_mesh_output.cpp:1413 Error: Leaving out vs_2_0 flow control: its shader didn't compile
d3d9_mesh_output.cpp:1413 Error: Leaving out vs_3_0 loops and texldl: its shader didn't compile
d3d9_mesh_output.cpp:1413 Error: Leaving out vs_2_0 DrawPrimitiveUP: its shader didn't compile
d3d9_mesh_output.cpp:590 Warning: FFP vertex blending row 0: the point at (150.7, 159.0) covers 81, 81 and 81 pixels, not 1
  (the same for rows 1-5)
d3d9_mesh_output.cpp:590 Warning: FFP indexed vertex blending row 0: the point at (179.7, 162.8) covers 81, 81 and 81 pixels, not 1
  (the same for rows 1-5)
```

There's no "software vertex processing" line or "ProcessVertices failed" line.

- **vs_1_1:** the first 20 instructions are accepted, and adding `mov oFog, v2.z` gets the shader
  rejected. The token is `Dst(RRast, 1, 0x1)`, oFog with the .x mask, with the source swizzle .zzzz.
- **X4509:** `int count : register(i0)` (lines 1161 and 1258) still doesn't compile with this machine's
  compiler, even though `count` is now only used as a loop counter.
- **81 pixels:** 81 is 9×9. The "FFP point size" case sets `D3DRS_POINTSIZE_MIN` to 9 and
  `POINTSIZE_MAX` to 14, and the default state (`d3d9_mesh_output.cpp:231`) never resets them. So the
  1×1 points of the later blending draws are drawn 9×9, and those draws get no `hw` lines. They
  still pass against ProcessVertices.

## Mesh check output

```
Reference ...\p3r\native\D3D9_Mesh_Output_reference.txt: 11 draws, viewport [0.0, 0.0, 400.0, 300.0, 0.0, 1.0]
PASS FFP transform (VS In 6 vertices, VS Out 6 rows (6 unique), unproject True)
PASS FFP directional light (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True, 24 rows checked against the GPU)
PASS FFP point light (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True, 24 rows checked against the GPU)
FAIL FFP spot light (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True, 24 rows checked against the GPU): 12 problems
    instance 0 row 5: (GPU) COLOR.0 ours ['0.721742', '0.655517', '0.270273', '0.784314'] reference ['0.803922', '0.737255', '0.290196', '0.784314']
    instance 0 row 5: (GPU) COLOR.1 ours ['0.35', '0.35', '0.7'] reference ['0.396078', '0.396078', '0.796078']
    instance 0 row 7: (GPU) COLOR.0 ours ['0.721742', '0.655517', '0.270273', '0.784314'] reference ['0.803922', '0.737255', '0.290196', '0.784314']
    instance 0 row 7: (GPU) COLOR.1 ours ['0.35', '0.35', '0.7'] reference ['0.396078', '0.396078', '0.796078']
    instance 0 row 10: (GPU) COLOR.0 ours ['0.721742', '0.655517', '0.270273', '0.784314'] reference ['0.803922', '0.737255', '0.290196', '0.784314']
    instance 0 row 10: (GPU) COLOR.1 ours ['0.35', '0.35', '0.7'] reference ['0.396078', '0.396078', '0.796078']
    instance 0 row 14: (GPU) COLOR.0 ours ['0.721742', '0.655517', '0.270273', '0.784314'] reference ['0.803922', '0.737255', '0.290196', '0.784314']
    instance 0 row 14: (GPU) COLOR.1 ours ['0.35', '0.35', '0.7'] reference ['0.396078', '0.396078', '0.796078']
FAIL FFP texgen and fog (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True): 24 problems
    instance 0 row 0: TEXCOORD.3 ours ['0.668524', '0.0529858'] reference ['0.552058', '0.361914']
    instance 0 row 1: TEXCOORD.3 ours ['0.997057', '0.5'] reference ['0.554165', '0.5']
    instance 0 row 2: TEXCOORD.3 ours ['0.849439', '0.230799'] reference ['0.686498', '0.356326']
    instance 0 row 3: TEXCOORD.3 ours ['0.849439', '0.230799'] reference ['0.686498', '0.356326']
    instance 0 row 4: TEXCOORD.3 ours ['0.997057', '0.5'] reference ['0.554165', '0.5']
    instance 0 row 5: TEXCOORD.3 ours ['0.960531', '0.5'] reference ['0.694709', '0.5']
    instance 0 row 6: TEXCOORD.3 ours ['0.849439', '0.230799'] reference ['0.686498', '0.356326']
    instance 0 row 7: TEXCOORD.3 ours ['0.960531', '0.5'] reference ['0.694709', '0.5']
PASS FFP point size (VS In 6 vertices, VS Out 6 rows (6 unique), unproject True, 6 rows checked against the GPU)
PASS FFP vertex blending (VS In 6 vertices, VS Out 6 rows (6 unique), unproject True)
PASS FFP indexed vertex blending (VS In 6 vertices, VS Out 6 rows (6 unique), unproject True)
PASS FFP XYZRHW DrawPrimitiveUP (VS In 4 vertices, VS Out 4 rows (4 unique), unproject False)
PASS FFP XYZRHW DrawPrimitiveUP VS In (112 vertex bytes, stride 28)
PASS FFP DrawIndexedPrimitiveUP (VS In 24 vertices, VS Out 24 rows (9 unique), unproject True)
PASS FFP DrawIndexedPrimitiveUP VS In (324 vertex bytes, stride 36, 48 index bytes)
PASS vs_3_0 instancing (VS In 6 vertices, VS Out 6 rows (4 unique), unproject True)

11 passed, 2 failed
```

It printed no NOTE lines, so ProcessVertices agreed with the GPU wherever both were read.

**FFP spot light:** the GPU doesn't clamp the spot factor inside the inner cone either. The `hw` line
for the centre vertex is exactly the ProcessVertices row: COLOR.0 (0.803922, 0.737255, 0.290196,
0.784314), COLOR.1 (0.396078, 0.396078, 0.796078). The neighbouring vertices (hw rows 4 and 6, outside
the inner cone) match ours. So this GPU applies the falloff formula inside θ/2 too, giving about
1.136× there. The decision "1 inside the inner cone" doesn't hold on this hardware.

**FFP texgen and fog:** only TEXCOORD.3, stage 3's `D3DTSS_TCI_SPHEREMAP`, differs. The other stages
pass now, including the reflection vector (TEXCOORD.0) and the camera space normal (TEXCOORD.2). From
the reference rows:
- The reference sphere map is **N.xy / 2 + 0.5**, with N the camera space normal (TEXCOORD.2). Row 0:
  N = (0.104117, -0.276172) gives (0.552058, 0.361914). Row 2: N = (0.372995, -0.287348) gives
  (0.686498, 0.356326). Both are exact.
- The reflection vector gives the same result: r / (2|r − (0, 0, 1)|) + 0.5. With V = (0, 0, -1),
  r = 2(N·V)N − V, so r − (0, 0, 1) = r + V = 2(N·V)N. Its length is 2|N·V|, and u reduces to
  N.x / 2 + 0.5 (N·V > 0 here).
- Ours is Wine's r / (2|r + (0, 0, 1)|) + 0.5. Row 0: r = (0.19896, -0.52774, -0.82578) gives
  (0.668524, 0.0529858).
- So with the non-local viewer, the sign of the z term in the sphere map denominator should follow
  `NonLocalViewer`. Only the non-local viewer case is tested here. This is ProcessVertices only:
  the GPU doesn't read back texture coordinates.

## Event Browser

The probe from last round was right about this machine after all. qrenderdoc's rows log the same
font (MS Shell Dlg 2 8pt, height 10, DPR 2, logical DPI 72) and the same doc heights. The difference
is the row: **qrenderdoc's rect height is 14, not the probe's 18.**

`RichResourceTextPaint` only centres the document when it's shorter than the row. A taller
document is drawn from the top, so its extra height is cut off at the bottom.

| `RENDERDOC_RICHTEXT_STYLE` | Params rows (3 blocks) | Link rows (6 blocks) | Looks |
|---|---|---|---|
| unset (`line-height: 14px`) | doc height 18 in 14 | doc height 20 in 14 | params rows low and clipped, link rows clipped (`richtext_default.png`) |
| `line-height: 100%` | 14 in 14 | 20 in 14 | params rows almost perfect, link rows still broken (`richtext_lh100.png`) |
| `none` | 14 in 14 | 20 in 14 | the same as 100% (`richtext_none.png`) |
| `line-height: 10px` | 14 in 14 | 20 in 14 | the same as 100% (`richtext_10px.png`) |

- For params rows, the 14px line height is what adds the 4 px. Any of the three overrides fixes them.
- The link rows' 20 px doesn't depend on the style. It comes from the link part of the document:
  those rows have 6 blocks instead of 3, at 20 px with or without parameter names. So the link
  icon or the resource name's cell makes the document 6 px taller than the row.

All four runs: Edge_Cases (frame 75), Show parameter names turned on after the capture loaded,
font scale 1. The screenshots and every run's "Rich text row" lines are in `d3d9_phase3e_retest/`.

### 3a log lines

These are from the unset style. Each distinct line is shown once, with its count; the full lines
are in `d3d9_phase3e_retest/richtext_log_default_params.txt`. The first run had Show parameter
names off, by mistake. It only logged the two link rows (`ReleaseDC(Offscreen Surface 32)` ×30 and
`GetFrontBufferData(Offscreen Surface 33)` ×29, identical to the lines below), so it was run again
with them on:

```
 4x Rich text row '"DrawPrimitiveUP(PrimitiveCount=2)"': rect height 14, doc height 18, lines 1, blocks 3, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
22x Rich text row '"GetFrontBufferData(Offscreen Surface 33)"': rect height 14, doc height 20, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
 4x Rich text row '"GetFrontBufferData(pDestSurface=Offscreen Surface 33)"': rect height 14, doc height 20, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
 4x Rich text row '"Present(pSourceRect=NULL, pDestRect=NULL, DestWindowOverride"': rect height 14, doc height 18, lines 1, blocks 3, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
22x Rich text row '"ReleaseDC(Offscreen Surface 32)"': rect height 14, doc height 20, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
 4x Rich text row '"ReleaseDC(pSurface=Offscreen Surface 32)"': rect height 14, doc height 20, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2
```

The 60-line limit is mostly used up by the link rows, which repaint far more often than the others.

## Files

The scratch directory is
`C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3r`.

- `cap\`: the new captures and the check outputs, with their replay logs (`*.txt.log`).
- `native\`: the reference and the demo log.
- `reg\`: the regression replay outputs.
- `richtext_log_*.log`: the full qrenderdoc logs of the Event Browser runs.
