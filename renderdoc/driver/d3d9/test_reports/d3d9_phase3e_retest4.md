# D3D9 phase 3e retest 4 (22ba5031c, link icon sizes)

Windows 11, RTX 4070 Ti, Development x64, 4K at DPR 2, the same setup as retest 3. No `RENDERDOC_RICHTEXT_*`
variables were set. I drove qrenderdoc with a `--ui-python` script and injected mouse input. Every click
was checked by asking the resource inspector which resource it showed. I alternated the targets, so each
click had to change what the inspector showed.

To get icon sizes, I matched the screen pixels against `link@2x.png` scaled to every size from 12 to 33 px.
On retest 3's screenshots, this method gives the known 16 px and 20 px (8 and 10 logical px).
Measurements are in screen pixels. Crops are in `d3d9_phase3e_retest4\`.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: 0 errors, no fixes needed |
| 1 | Event Browser icons, D3D9 Edge_Cases frame 75, names on | **FAIL**: the icons are 9 logical px, not 10. The baselines still match the plain rows |
| 2 | Resource inspector, initialisation parameters | **FAIL**: both icons are now the same size, and every click works. The `pSurface` link's underline goes through the text, and its row's top and bottom edges don't respond |
| 3 | D3D9 pipeline state, IA vertex stream and OM rows | **FAIL**: the icons are the same size, centred and not clipped, and the rows are fine. The underline and edge problems from item 2 show up here too |
| 4 | D3D11 pipeline state and Event Browser Present link | **FAIL**: the same problems as item 3 in the pipeline state. The Present link passes |

## 1. Icons are 9 logical px, not 10

- Every link icon in this build measures **18 screen px, which is 9 logical px**, not 10. That includes the
  Event Browser, the resource inspector and the pipeline state, in both D3D9 and D3D11.
  `zoom_eb_icon_sizes.png` shows the `ReleaseDC` row three times: retest 3's size 8, retest 3's size 10
  test, and this build.
- **The cause is the `qFloor(fm.lineSpacing())` cap in `RichResourceIconSize`
  (`qrenderdoc/Code/QRDUtils.cpp`).** A temporary log, since reverted, gave these values for
  `QFontMetricsF` of "MS Shell Dlg 2" 8pt:

  | Metric | Value |
  |---|---|
  | `ascent` | 8 |
  | `descent` | 1.64 |
  | `height` | 9.64 |
  | `leading` | 0 |
  | `lineSpacing` | 9.64 |
  | `capHeight` | 5.81 |

  So `qRound(8 × 1.25)` is 10, but `qFloor(9.64)` is 9, and the smaller value wins. Some possible fixes:
  - Use `qCeil` in the cap.
  - Cap at `qRound(lineSpacing)`.
  - Bound the size by the row height instead.

  Retest 3's size 10 test fitted in the Event Browser's 23 px rows with 4 px clear above and 2 px below.
- **Centring:** the 9 px icon sits 1 px below the capitals' centre. In the `ReleaseDC` row (24 px), the
  capitals span y 8–19 and the icon box spans 6–23.
- **Baselines still match:**
  - Every rich text row has its baseline 4 px above the separator, and its descenders end 1 px above it.
    That is the same as the plain rows and the same as retest 3.
  - The rows measure: text top 6–8, baseline 18 in 23 px rows and 19 in 24 px rows, descender bottom 21
    in 23 px rows and 22 in 24 px rows.
  - Crop: `zoom_eb_names_on.png`.

## 2. Resource inspector

- **The icons now match.** The `CreateOffscreenPlainSurface(…)` row and its `pSurface` child both measure
  18 px (`zoom_inspector_params.png`). Both icons are within 1 px of the capitals' centre.
- **Clicks: I couldn't observe them here.** Both links point to the resource the inspector already shows,
  `Offscreen Surface 32`. `ResourceInspector::Inspect` returns early for the resource it already shows, so
  a click changes nothing. Items 3 and 4 test the lone-ResourceId clicks instead.
- **The `CreateOffscreenPlainSurface(…)` row (rich text) passes.** The underline appears with the mouse
  anywhere in the row: at the top edge, in the middle, at the bottom edge and on the icon. The line is
  drawn 2 px below the text.
- **Fail 1: the `pSurface` underline goes through the text.** It is drawn at y 250–251, but the text
  spans 241–252, so the line crosses the bottom 2 px of the glyphs. `zoom_inspector_hover.png` shows, from
  top to bottom:
  - the `pSurface` row before hover
  - the `pSurface` row during hover
  - the rich text row before hover
  - the rich text row during hover, for comparison
- **Fail 2: the `pSurface` row's top and bottom edges don't respond.**
  - The row spans y 228–261, but hover only works from 236 to 255. That is the 18 px icon box plus a
    pixel or two.
  - So the top 8 px and the bottom 6 px of the row give no underline and no click. Retest 3 had no
    dead band.
- **Cause of both fails:** the commit changed the lone-ResourceId path in `RichResourceTextPaint` and
  `RichResourceTextMouseEvent`.
  - The hover area used to be stretched to `px.height()`, the 32 px pixmap. That made it nearly as tall as
    the row and put `textRect.bottom() - margin` below the text.
  - Now the hover area is the font's bounding rect plus the icon (`textRect.united(iconRect)`), which is
    only about as tall as the text. So `underline_y = textRect.bottom() - margin` lands on the glyphs.
  - Possible fix: put the underline at the font's baseline + 1 or 2 px, as the rich text path does.
    Make the hover area span the full cell height, as the rich text cells do.

## 3. D3D9 pipeline state

- **Icons:** they are all 18 px, centred, and not clipped:

  | Row | Row y | Icon box y | Text y |
  |---|---|---|---|
  | IA `DrawPrimitiveUP vertices` | 1247–1279 | 1255–1272 | 1259–1270 |
  | OM `Backbuffer 17` | 721–754 | 729–746 | 732–744 |
  | OM `Auto Depth Stencil 16` | 756–790 | 765–782 | 768–780, without the descender |

  The icon centre is within 1 px of the row centre in every row. There are at least 6 px clear above and
  below each icon.
- **Row heights:** the link rows are now 4 px (2 logical px) shorter.
  - The OM render target rows went from 37–38 px to 33–34 px. The IA stream row went from 37 px to 33 px.
  - They now match the plain rows around them, such as the IA vertex element rows at 33–34 px.
  - Nothing looks cramped. `zoom_d3d9_om_rows_before_after.png` and `zoom_d3d9_ia_rows_before_after.png`
    show retest 3 above and this build below.
- **Clicks: mid-row and on the icon, every click opened the right resource.** Those were Backbuffer 17,
  Auto Depth Stencil 16, and the `DrawPrimitiveUP vertices` buffer.
- **Fail: the same two problems as item 2.**
  - The underline crosses the bottom of the text: Backbuffer 17 at y 742–743 against a baseline of 744,
    Auto Depth Stencil 16 at 778–779, and the IA stream at 1268–1269. Crops: `zoom_d3d9_om_hover.png` and
    `zoom_d3d9_ia_hover.png`.
  - Clicking 2 px inside the top or bottom edge of the OM rows does nothing. The inspector stayed on the
    previous resource, and hovering there gives no underline.

## 4. D3D11 (D3D11_Simple_Triangle, Draw at EID 11)

- **Icons:** they are all 18 px and centred:

  | Row | Row y | Icon box y | Text y |
  |---|---|---|---|
  | IA `Buffer 91` | 1469–1501 | 1477–1494 | 1480–1492 |
  | OM `Swapchain Image 71` | 723–755 | 731–748 | 734–746 |

  Both rows are 33 px, the same as the plain rows beside them, and neither icon is clipped.
- **Event Browser `Present( Swapchain Image 71 )` passes.**
  - The icon is 18 px and the row's text measures the same as D3D9.
  - The underline appears at the top edge, in the middle, at the bottom edge and on the icon. It sits at
    row offsets 21–22, below the text.
  - Every click opened Swapchain Image 71. Crop: `zoom_d3d11_present_hover.png`, before and during hover.
- **Pipeline state clicks: mid-row and on the icon, every click opened the right resource.** I alternated
  Buffer 91 and Swapchain Image 71.
- **Fail: the same two problems as item 3.**
  - The underline crosses the bottom of the text: OM at y 744–745 against text at 734–746, and IA at
    1490–1491 against text at 1480–1492. Crops: `zoom_d3d11_om_hover.png` and `zoom_d3d11_ia_hover.png`.
  - Clicks 2 px inside the top or bottom edge of the IA and OM rows do nothing, and there is no underline
    there either.
