# D3D9 phase 3e retest 3 (e409786bd)

Windows 11, RTX 4070 Ti, Development x64. Same machine and setup as `d3d9_phase3e_retest2.md`.
`RENDERDOC_RICHTEXT_STYLE` was not set and `RENDERDOC_RICHTEXT_LOG=1` was. The screen is 4K at DPR 2.
This time I drove qrenderdoc myself: a `--ui-python` script toggled the parameter names, and injected
mouse input did the hovering and clicking. The screenshots are in `d3d9_phase3e_retest3\`.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: 0 errors, no fixes needed |
| 1 | Event Browser rows, D3D9 Edge_Cases frame 75 | **PASS** |
| 2 | Link hover underline and click | **PASS** |
| 3 | D3D11 Event Browser (D3D11_Simple_Triangle) | **PASS** |
| 4 | Diagnostic log | Done: the lines are below |
| 5 | D3D9 pipeline state panel and resource inspector | **PASS**, with one note below |

## 1. Event Browser rows

These are the measurements in screen pixels. Rows are 23–24 px between their separators. The offsets
count from the top of the row.

| Row | Text top | Baseline | Descender bottom |
|---|---|---|---|
| Plain rows (`Capture Start`, markers) | 7–8 | 18 / 19 | 21 / 22 |
| Names off: `ReleaseDC(`, `GetFrontBufferData(` | 7 | 19 | 22 |
| Names on: `Clear(Count=…)`, `DrawPrimitiveUP(PrimitiveCount=2)`, link rows, `Present(…)` | 6–7 | 18 / 19 | 21 / 22 |

A slash such as 18 / 19 gives the value in 23 px rows / 24 px rows.

- **Rich text rows now sit at the same height as plain rows.** In every row, with names off and on, the
  baseline is 4 px above the bottom separator.
- **Descenders are whole.** They end 1 px above the separator, the same as in plain rows.
- **The link icon is 16 × 16 in a 24 px row.** It spans y 7–22, the same extent as the parentheses
  around it. That is about 1 px below the centre of the capitals, so it reads as centred. Its edges are
  smoothly anti-aliased, not jagged. Crops: `zoom_names_off_links.png`, `zoom_names_on_links.png` and
  `zoom_names_on_params.png`.

## 2. Hover and click

- **Hovering a link draws a visible underline:** a 2 px light line at row offsets 21–22, under the
  resource name and its icon. `zoom_hover_compare.png` shows the row before and during hover. It appears
  with the mouse anywhere from the top edge of the row to its bottom edge (offsets 0 to 23), with names on
  and off.
- **Clicking opens the resource.** I alternated between `Offscreen Surface 32` and
  `Offscreen Surface 33`, so each click had to change the inspected resource. Every click opened the
  right one in the resource inspector, near the top edge, in the middle and near the bottom edge, with
  names on and off.

## 3. D3D11

I used D3D11_Simple_Triangle. Its only resource link is `Present( Swapchain Image 71 )`.

- Every row, including the link row and the `ClearRenderTargetView(ColorRGBA=…)` and
  `Draw(VertexCount=3)` rows, measures the same as in D3D9.
- Hovering the link shows the underline (`zoom_d3d11_hover.png`).
- I closed the resource inspector before each click. Clicking near the top edge, in the middle and near
  the bottom edge each reopened it on Swapchain Image 71.

## 4. First 5 "Rich text row" lines

All 60 logged rows have the same sizes: `rect height 14, doc height 18, lines 1`, `baseline 14` and
`y offset -3.82031`. Only the text and the block count differ.

```
QTRD 015684: [22:28:10]         QRDUtils.cpp( 977) - Log     - Rich text row '"ReleaseDC(Offscreen Surface 32)"': rect height 14, doc height 18, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2, baseline 14, y offset -3.82031
QTRD 015684: [22:28:10]         QRDUtils.cpp( 977) - Log     - Rich text row '"GetFrontBufferData(Offscreen Surface 33)"': rect height 14, doc height 18, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2, baseline 14, y offset -3.82031
QTRD 015684: [22:28:10]         QRDUtils.cpp( 977) - Log     - Rich text row '"ReleaseDC(Offscreen Surface 32)"': rect height 14, doc height 18, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2, baseline 14, y offset -3.82031
QTRD 015684: [22:28:10]         QRDUtils.cpp( 977) - Log     - Rich text row '"GetFrontBufferData(Offscreen Surface 33)"': rect height 14, doc height 18, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2, baseline 14, y offset -3.82031
QTRD 015684: [22:28:50]         QRDUtils.cpp( 977) - Log     - Rich text row '"ReleaseDC(Offscreen Surface 32)"': rect height 14, doc height 18, lines 1, blocks 6, font "MS Shell Dlg 2" 8pt height 10 lineSpacing 10 ascent 8, doc font 10, painter DPR 2 logical DPI 72, primary screen logical DPI 72 DPR 2, baseline 14, y offset -3.82031
```

## 5. Pipeline state and resource inspector

Nothing looks broken. In the pipeline state, the IA vertex stream and OM render target rows line up
their text with the plain cells beside them. The resource inspector's initialisation parameters line up
too.

**Note: link icons now come in two sizes.** Cells whose value is a lone ResourceId use the old icon:
`QRDUtils.cpp:934-946` draws `Pixmaps::link()` unscaled, 16 logical px. Rich text cells now use the
smaller one, 8 logical px here, from 1.5× the cap height. So in the resource inspector's
initialisation parameters (`resource_inspector_params.png`), the `CreateOffscreenPlainSurface(…)` row
has the small icon, while the `pSurface` child row right under it has an icon twice that size. That
larger icon is 27 px tall and reaches the bottom of its 36 px row. The pipeline state's resource cells
(`Backbuffer 17`, `Auto Depth Stencil 16` and `DrawPrimitiveUP vertices`, `zoom_pipeline_ia.png`) all
use the large icon too. Those large icons look the same as before; it's only the mismatch that is new.

## Follow-up: a slightly bigger link icon

I tried other rich text icon sizes with a temporary, reverted override of `iconSize` in
`RichResourceText::cacheDocument`. These are the D3D9 link rows with names on, in screen pixels: the
row is 23 px, its capitals span y 6–18, and the baseline is at 18.

| `iconSize` (logical) | Icon y | Icon centre | Text baseline |
|---|---|---|---|
| 8 (current) | 8–21 | 14.5 (2.5 px below the capitals' centre) | 18 |
| **10** | **4–20** | **12.0 (on the capitals' centre)** | 18 |
| 12 | 0–20 | 10.0 (reaches the separator above) | 18 |
| 14 | 0–20 | 10.0 (cut off at the top) | 18 |

- **The text doesn't move at any size.** Plain and rich rows keep the same baseline, because
  `RichResourceTextYOffset` places the text by its own baseline, whatever the icon does to the
  document's height.
- **The icon grows upward from a nearly fixed bottom, not about its centre.** So the limit is the
  separator above the row.
- **10 logical px (20 screen px here) is the sweet spot.** The icon then sits exactly on the capitals'
  centre, with 4 px clear above it and 2 px below, and it's closer to the 16 px lone-ResourceId icon.
  At this font, MS Shell Dlg 2 8pt (cap height about 5.3, ascent 8), that size comes from about
  1.9 × the cap height or 1.25 × the ascent. Bounding it at row height minus 4 would keep it off the
  separators at other font sizes.
- `icon_sizes_compare.png` shows sizes 8, 10 and 12 stacked.
