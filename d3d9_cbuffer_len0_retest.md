# D3D9 retest on f5fd4ce7f (GetCBufferVariableContents length 0)

I built the branch tip at 2fc85e798, which is f5fd4ce7f plus the crash report and has no other code changes. The machine is the same: Windows 11, RTX 4070 Ti, Development x64. Crops are in `d3d9_cbuffer_len0_retest\`.

| # | Item | Result |
|---|---|---|
| 0 | Build | **PASS**: 0 errors, no fixes needed |
| 1 | D3D9_Mesh_Output, `d3d9_pipestate_check.py` | **PASS** |
| 2 | D3D9 game capture with PS c#/i#/b# | **Not run**: I don't have a D3D9 game capture |
| 3 | UI constant buffer viewer, D3D9 VS and D3D11 cbuffer | **PASS** |

## 1. D3D9_Mesh_Output

- **Capture:** I recaptured `D3D9_Mesh_Output_frame12114.rdc` the same way as before, with qrenderdoc's Python running ExecuteAndInject and TriggerCapture on the existing `demos_x64.exe`.
  - That exe was built after the last demo change.
  - The demo log has no errors.
- **Script run:** I ran `util\test\d3d9_pipestate_check.py` on it in qrenderdoc's embedded Python, as in earlier rounds. It exited 0.

Every "constant block N length 0:" line says "same", and every block has non-zero values:

```
Event 1507 (vs_1_1):               constant block 0 length 0: same 9 variables (9 non-zero)
Event 1637 (vs_2_0 flow control):  constant block 0 length 0: same 8 variables (8 non-zero)
                                   constant block 1 length 0: same 1 variables (1 non-zero)
                                   constant block 2 length 0: same 2 variables (1 non-zero)
Event 1771 (vs_3_0 loops, texldl): constant block 0 length 0: same 8 variables (8 non-zero)
                                   constant block 1 length 0: same 1 variables (1 non-zero)
Event 1902 (vs_3_0 instancing):    constant block 0 length 0: same 2 variables (2 non-zero)
Event 2031 (vs_2_0 DrawPrimitiveUP): constant block 0 length 0: same 8 variables (8 non-zero)
                                   constant block 1 length 0: same 1 variables (1 non-zero)
                                   constant block 2 length 0: same 2 variables (1 non-zero)

GetCBufferVariableContents length 0 matched the descriptor size everywhere
```

- **The vs_2_0 flow control draw has the float, int and bool blocks.** Bool b0/b1 shows 1 non-zero of 2, which is correct: the demo sets `{TRUE, FALSE}`.
- **The values at length 0 are the demo's values**, read through the UI driver at event 1637 (section 3):
  - `tint` = (0.2, 0.3, 0.4, 1);
  - `i0` = (3, 0, 1, 0);
  - `useTint` = 1, `other` = 0;
  - c5–c10 are the shader's `def` literals, for example c5 = (0, 0, 1, 0.1).
- **The other 11 D3D9 captures from earlier rounds** (Simple_Triangle, Wrapping_Check, D3D9Ex_Wrapping_Check, Edge_Cases ×2, Frame_Start_State, Reset ×2, Resource_Contents and Overlay_Test) also all end with "GetCBufferVariableContents length 0 matched the descriptor size everywhere", with every block "same". None of them has a PS constant block, so they add no PS coverage.

## 2. D3D9 game capture

**Not run.** I searched every drive for `.rdc` files and found none from a D3D9 game.
- The large captures are from D3D11/D3D12/Vulkan games.
- The two small ones date from 2020, before the D3D9 driver existed.
- None of the demo captures uses PS c#/i#/b# either, so the PS constant path is still untested.

## 3. UI constant buffer viewer

- **How I opened the viewers:** with `ViewConstantBuffer(stage, block, 0)`, which is what the D3D9 Pipeline State "Go" button calls (`D3D9PipelineStateViewer.cpp:1163`).
- **What I compared:** each viewer's values against `GetCBufferVariableContents`, read at the descriptor's size and at length 0.
- **Why the fix shouldn't change the UI:** the viewer requests the descriptor's size, not 0 (`BufferViewer.cpp:3820`).

Findings:
- **D3D9, Mesh_Output event 1637 (vs_2_0 flow control):** the "Vertex CB 0/1/2" tabs show the same values as the replay at both lengths and the demo source. Crops: `zoom_d3d9_cb0.png`, `zoom_d3d9_cb1.png` and `zoom_d3d9_cb2.png`.
  - **c#:** `mvp` is the matrix, `tint` = 0.20, 0.30, 0.40, 1.00, then c5 = 0, 0, 1, 0.10, c6 and c7.
  - **i#:** `i0` = 3, 0, 1, 0.
  - **b#:** `useTint` = true, `other` = false.
- **D3D11, D3D11_CBuffer_Zoo EID 11 (new capture), PS cbuffer 0 `consts`:** the "Pixel CB 0" viewer shows the demo's incrementing values. That is the same as `GetCBufferVariableContents` at the descriptor size (65536). Crop: `zoom_d3d11_cb0.png`.
  - a = 0–3, b = 4–6, c = 8, 9 … p = 61, dummy2 = 64–67, …
  - q (column_major) starts {76, 80, 84, 88}, r (row_major) starts {92, 93, 94, 95}, s …
- **D3D11 at length 0:** this still reads all zeros, the unchanged shared behaviour that the commit says a cbuffer bound with a 0 range should get.
