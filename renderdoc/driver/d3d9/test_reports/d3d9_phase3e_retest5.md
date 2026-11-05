# D3D9 phase 3e retest 5 (c1049db4f, icon cap rounding and resource cell hover area)

The machine, setup and method are the same as retest 4: Windows 11, RTX 4070 Ti, Development x64, 4K at
DPR 2, with no `RENDERDOC_*` variables set.

- I matched icon sizes against `link@2x.png`. On retest 3's size 10 shot, that gives 20 px, and on retest 4's
  shot it gives 18 px.
- To check each click, I asked the resource inspector which resource it showed.
- The clicks alternated targets, so each click had to change the inspected resource.
- Measurements are in screen pixels.
- Crops are in `d3d9_phase3e_retest5\`. In each one, the top panel is retest 4, then this build, then this
  build during hover.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: 0 errors, no fixes needed |
| 1 | Event Browser icons, D3D9 Edge_Cases frame 75, names on | **PASS** |
| 2 | D3D9 pipeline state: OM Backbuffer 17 / Auto Depth Stencil 16, IA DrawPrimitiveUP vertices | **PASS** |
| 3 | D3D11_Simple_Triangle EID 11: IA Buffer 91, OM Swapchain Image 71 | **PASS** |
| 4 | Resource inspector, `pSurface` child row | **PASS** |

There are no failures. These are the measurements behind each pass:

1. **Event Browser**
   - **Icon size:** both link icons are 20 px (10 logical). The `ReleaseDC` icon is pixel-identical to
     retest 3's size 10 test.
   - **Separators:** the icon's ink spans y 742–759 in a row that ends at 761. That leaves 2 px clear above
     the separator at 762, and 5 px clear below the separator above the row.
   - **Baselines:** every row has its baseline 4 px above the bottom separator (18 in 23 px rows, 19 in
     24 px rows), and its descenders end 1 px above it, the same as plain rows.
   - Crop: `zoom_eb_icon_sizes.png`.
2. **D3D9 pipeline state**
   - **Underline:** it is 1 px below the text's baseline:
     - Backbuffer 17: text to 744, line 746–747.
     - Auto Depth Stencil 16: capitals to 780, line 782–783. The `p` descender crosses it, as in rich text
       rows.
     - DrawPrimitiveUP vertices: text to 1270, line 1272–1273.
   - **Hover and click:**
     - The underline appears, and every click opened the right resource, at these points:
       - 2 px inside the top and bottom edges
       - mid-row
       - on the icon
       - on the first and last pixel rows of the row
     - In the OM, I alternated Backbuffer 17 and Auto Depth Stencil 16. In the IA, I reset the inspector to
       Backbuffer 17 before each click.
   - **Icons:** 20 px, with the icon centre within 0.5 px of the row centre, and not clipped.
   - **Row heights:** unchanged from retest 4. The rows are at 721–753, 756–789 and 1247–1279: 33, 34 and
     33 px.
   - Crops: `zoom_d3d9_om_hover.png` and `zoom_d3d9_ia_hover.png`.
3. **D3D11**
   - **Underline:**
     - Swapchain Image 71: capitals to 746, line 748–749. The `p` and `g` descenders cross it.
     - Buffer 91: text to 1492, line 1494–1495.
   - **Hover and click:** all 12 hover and click points work, alternating Buffer 91 and Swapchain Image 71.
     They were the same points as item 2.
   - **Icons:** 20 px, centred within 0.5 px, and not clipped.
   - **Row heights:** unchanged from retest 4, at 33 px.
   - Crops: `zoom_d3d11_om_hover.png` and `zoom_d3d11_ia_hover.png`.
4. **Resource inspector, `pSurface` row**
   - **Underline:** text 1530–1542, line 1544–1545. That is the same 1 px gap as the
     `CreateOffscreenPlainSurface(…)` rich text row above it, whose text ends at 1362 and line is at
     1364–1365.
   - **Hover:** the underline appears at every height from the row's first pixel row (1518) to its last
     (1551), and on the icon at the top, middle and bottom.
   - **Icon:** both icons in the parameters are 20 px.
   - Crop: `zoom_inspector_hover.png`. The panels from top to bottom are:
     - the `pSurface` row before hover
     - the `pSurface` row hovered at mid-row
     - the `pSurface` row hovered at the top pixel row
     - the `pSurface` row hovered at the bottom pixel row
     - the rich text row before hover
     - the rich text row during hover
