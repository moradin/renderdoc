# D3D9 phase 3e retest 2 (493802b34)

Windows 11, RTX 4070 Ti, Development x64 (demos Release x64). Same setup as `d3d9_phase3e_retest.md`.
`RENDERDOC_RICHTEXT_STYLE` was not set.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: 0 errors, no fixes needed |
| 1 | `D3D9_Mesh_Output` native run | **PASS**: the log has no Error or Warning lines. The captured run's log is the same, and it wrote an identical reference |
| 1 | `d3d9_mesh_check.py` | **PASS**: 18 passed, 0 failed. vs_1_1, vs_2_0 flow control, vs_3_0 loops and texldl, vs_2_0 DrawPrimitiveUP and its VS In check are all in the capture and pass. FFP spot light and FFP texgen and fog pass too |
| 2 | Mesh viewer GS/DS | **PASS**: the D3D9 capture has no GS/DS Output table and no GS/DS Out tab (`mesh_d3d9_no_gsds.webp`). Opening a D3D11 capture in the same session brings both back: the table says "No geometry and no tessellation shader bound." (`mesh_d3d11_gsds.webp`) |
| 3 | Event Browser rich text rows | **FAIL**: better than before, but the rich text rows still sit about 4 px low and their descenders are clipped. The link icon is as tall as the row. Hovering a link doesn't visibly underline it. Clicking a link still opens the resource |
| 4 | `renderdoccmd test unit "[d3d9]"` | **PASS**: 6057 assertions in 13 test cases |
| 5 | Regression | **PASS**: `d3d9_overlay_check.py` on last round's D3D9_Overlay_Test capture gives 101 passed, 0 failed, and every overlay PNG is byte-identical to last round's. `d3d9_replay_check.py` on Simple_Triangle and Edge_Cases matches last round's output (only the paths and timings differ), with identical PNGs and alpha 255-255 |

## Event Browser (item 3)

D3D9 Edge_Cases, frame 75. Screenshots are `eventbrowser_names_off.png` and `eventbrowser_names_on.png`.
The `zoom_*.png` files are 3× crops of the same screenshots.

These measurements are in screenshot pixels. Rows are 23–24 px. "Gap" is the space between a row's
separators and the top or bottom of its text:

| Row | Gap top | Gap bottom |
|---|---|---|
| Plain rows (`Capture Start`, markers, names-off `DrawPrimitiveUP(2)`) | 6–7 | 1 (the bottom of the parentheses) |
| Names off: `ReleaseDC(` / `GetFrontBufferData(` text | 11–12 | 0 |
| Names on: parameter rows (`Clear(Count=…`, `DrawPrimitiveUP(PrimitiveCount=2)`) | 10–12 | 0 |
| Names on: link rows' text | 11 | 0 |
| Link icon, both modes | 0 | 0 |

- **Rich text rows sit about 4–5 px lower than plain rows.** Their text reaches the separator below, so
  descenders and the bottoms of the parentheses are cut: for example the `p` and `g` in `pRects=`,
  `Flags=`, `pSurface=` and `pDestSurface=`, and the commas. It's much better than last round's
  half-line drop, and every row is readable. The user described it as "still a little bit of clipping".
- **The link icon is as tall as the row.** It touches the separators above and below and looks
  too big next to the text (the user's words: "the link icon still looks too big to me"). The
  icon is about twice the text's cap height.
- With parameter names off, the rows without links (`Clear(0, {}, 7, …)`, `DrawPrimitiveUP(2)`,
  `Present(NULL, …)`) are plain rows and are positioned correctly.
- **Hovering a resource link doesn't visibly underline it**; the user couldn't see an underline.
  Clicking the link still opens the resource.

## Files

The scratch directory is
`C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3r2`.

- `native\`: the native demo log and reference.
- `cap\D3D9_Mesh_Output\`: the capture, the mesh check output and its log.
- `reg\`: the regression outputs.
- `build*.txt` and `unit.txt`: the build logs and the unit test output.
