# D3D9 phase 3 retest 2 (0ca2df964)

Windows 11, RTX 4070 Ti, Development x64. Same setup as `d3d9_phase3_retest.md`.

| # | Item | Result |
|---|---|---|
| 1 | Build Development x64 | **PASS**: 0 errors, no fixes needed (nothing committed) |
| 2 | `d3d9_replay_check.py` SaveTexture alpha | **PASS**: Simple_Triangle and Edge_Cases both print alpha 255-255. Both PNGs are opaque, and their RGB matches the `_last.png` exactly |
| 2 | UI: Save Texture as PNG on Backbuffer 17 | **PASS**: opaque |
| 3 | asm14sel.ps_1_4 vs fxc | **PASS**: 24/24 have the same instructions, and asm14sel matches `fxc /dumpbin` line for line (`.xyzz`/`.xyww`) |
| 4 | Reset_Capture ×3 (ran 4) | **PASS**: frame 30 is Backbuffer 23 / Auto Depth Stencil 22 in all 4 runs. The replay is "After the reset" > Clear > DrawPrimitive. No "Back buffer %u still referenced across Reset" in any demo's log, both Wrapping_Checks included |
| 5 | UI: Mesh View icon next to Export | **PASS** |
| 5 | UI: IA topology row | **FAIL** (diagram size, see below). "Topology: Triangle List" is left-aligned on its own row above the declaration, as asked |
| 5 | UI: Edge_Cases rows 84 and 85 | **FAIL**, see below. The D3D11 Simple_Triangle Event Browser looks normal |
| 5 | UI: VS float constants > View Contents | **PASS**: opens with `float4 c;`, one register per row |
| 6 | `renderdoccmd test unit "[d3d9]"` | **PASS**: 5331 assertions in 2 test cases |

## Failures

**IA topology diagram is still huge.** The row is in the right place, but the Triangle List
diagram is three triangles about 120 px tall each (in a screenshot at this machine's DPI), taller
than the IA box above them. `setTopologyDiagram` draws the SVG at its viewBox size, the same as for
D3D11, so it may need a fixed or maximum size on the D3D9 label.

**Event Browser rows 84 `ReleaseDC(Offscreen Surface 32)` and 85
`GetFrontBufferData(Offscreen Surface 33)` are still clipped.** The rows look the same height as
the plain rows around them (`DrawPrimitiveUP(2)`, `Present(...)`). The text with the link is drawn
lower than the plain rows' text, so its bottom half is cut off by the next row, link icon included.
The `EventRowDelegate` minimum height doesn't seem to apply. Either the rows didn't get taller, or
the offset comes from painting rather than height. The rows just above, the `Depth` and
`State blocks` markers, have a white background.

## Observations (not failures)

- **Reset frame 61 IDs** are now Backbuffer 25 / Auto Depth Stencil 24 in all 4 runs; they used to
  be 23/22. This fits Reset now creating new wrappers every time.
- **One new warning, in 1 of the 4 Reset runs:** `d3d9_resources.cpp(204) - Warning - Real D3D9
  object ... already has a wrapper, replacing it`, logged once just before frame 61 was written.
  None of the 9 Reset logs from earlier rounds on this machine have it. That run's captures replay
  the same as the others.
- **New capture frames:** Edge_Cases was captured on frame 64, an even frame like the original
  round.

## Files

The scratch directory is
`C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3`.

- `cap2\`: the new captures of every D3D9 demo and their outputs (`_replay`, `_res`, `_saved.png`).
- `cap2_other\`: the D3D11 capture used for the Event Browser comparison.
- `reset_runs2\r1`–`r3\`: the extra Reset runs.
- `sh\out2\`: the new disassembly; `sh\compare_out_retest2.txt` compares it with fxc.
- `unit_retest2.txt`: the unit test output.
