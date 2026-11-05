# D3D9 phase 3 retest 3 (adae018aa)

Windows 11, RTX 4070 Ti, Development x64. Same setup as `d3d9_phase3_retest2.md`. The build had 0 errors.

| # | Item | Result |
|---|---|---|
| 1 | D3D9 IA page topology diagram | **PASS** |
| 2 | Event Browser rows with resource links / parameter names | **FAIL**, see below |
| 3 | `D3D9_Reset_Capture` ×4 | **PASS**: no "already has a wrapper" warning in any of the 4 logs, and no "still referenced across Reset". IDs are the same as last round (frame 30: Backbuffer 23 / Auto Depth Stencil 22; frame 61: 25/24). The replays match last round's line for line apart from timings and paths |

## Failure

**Rich text rows in the Event Browser are still drawn low and clipped** (D3D9 Edge_Cases, frame 64).

- **Show parameter names off:** it's the same as last round. Rows 84 `ReleaseDC(Offscreen Surface 32)` and
  85 `GetFrontBufferData(Offscreen Surface 33)` have their text, link icon included, drawn about
  half a line lower than the plain rows. The bottom half is cut off by the next row.
- **Show parameter names on:** this looks worse. Every row with parameters is shifted down and clipped
  the same way, whether or not it has a resource link: `Clear(Count=0, ...)`, `DrawPrimitiveUP(PrimitiveCount=2)`,
  `Present(pSourceRect=NULL, ...)`, and the selected row 44. Rows without parameters draw
  normally: `Capture Start`, the `Depth` / `Multisampled` / `State blocks` markers, `Frame #64`.
  So it seems to affect every row painted through RichResourceText, not just links.
- The rows themselves don't look any taller than the plain rows. The `line-height` change in
  `RichResourceText` (691e3220a) made no visible difference at this machine's DPI.

The D3D11 Simple_Triangle capture has the same problem, so this isn't specific to D3D9.

## Files

The scratch directory is
`C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3`.

- `reset_runs3\r1`–`r4\`: the Reset runs and their replay outputs.
- `build_retest3.txt`: the build log.
