# D3D9 pixel shader constants retest on afe200f5f

Everything passes.

- **What I built:** afe200f5f, `renderdoc.sln` and `util\test\demos.sln`, Development x64, with 0 errors and 0 warnings.
- **Machine:** the same as before (Windows 11, RTX 4070 Ti).
- **Crops:** in `d3d9_ps_constants_retest\`.

| # | Item | Result |
|---|---|---|
| 1 | D3D9_Simple_Triangle natively: log and pixel colour | **PASS** |
| 2 | `d3d9_pipestate_check.py` on a Simple_Triangle capture | **PASS** |
| 3 | qrenderdoc at the "Pixel shader constants" draw | **PASS** (see the note on the shader viewer) |
| 4 | D3D9_Mesh_Output recapture, `d3d9_mesh_check.py` | **PASS** |

## 1. Native run

- **Demo log:** only the "Running D3D9 test on NVIDIA GeForce RTX 4070 Ti" line. There is no TEST_WARN or TEST_ERROR, so no "loop shader" warning and no CreatePixelShader failure.
- **Pixel colour:** the centre of the bottom-right triangle is **96, 191, 0**. So are the pixels 3 px away on each side. Screenshot: `native_D3D9_Simple_Triangle.png`.

## 2. `d3d9_pipestate_check.py`

The capture is `D3D9_Simple_Triangle_frame26696.rdc`, made with ExecuteAndInject and TriggerCapture as before. The demo log under RenderDoc is clean too. The script exited 0. The "Pixel shader constants" draw is event 30, and its PixelShader lines are:

```
  ShaderStage.Pixel: Pixel Shader 22 ps_3_0 reflection yes
    constant block 0 length 0: same 3 variables (3 non-zero)
    constant block 1 length 0: same 1 variables (1 non-zero)
    constant block 2 length 0: same 1 variables (1 non-zero)

GetCBufferVariableContents length 0 matched the descriptor size everywhere
```

- **Block 0's 3 variables:** `c0`, `tint` and `stepColour`. `c0` is the shader's `def c0, 0, 0, 0.5, 1`.
- **Values:** identical at the descriptor size and at length 0, both from `GetCBufferVariableContents`:
  - `tint` = 0.5, 2, 1, 1;
  - `stepColour` = 0.4, 0.2, 0, 0;
  - `i0` = 4, 0, 1, 0;
  - `useTint` = 1.

## 3. qrenderdoc at event 30

- **Pipeline State → Pixel Shader → Constants** lists Float (c#) 224, Integer (i#) 16 and Boolean (b#) 16. Crop: `zoom_pipeline_ps_constants.png`.
- **The three constant buffer viewers (Go):** each row is named after the variable, not the register:
  - Float: `c0` = 0.00, 0.00, 0.50, 1.00, `tint` = 0.50, 2.00, 1.00, 1.00 (c5), `stepColour` = 0.40, 0.20, 0.00, 0.00 (c9). Crop: `zoom_ps_cb0_float.png`.
  - Integer: `i0` = 4, 0, 1, 0. Crop: `zoom_ps_cb1_int.png`.
  - Boolean: `useTint` = true (b3). Crop: `zoom_ps_cb2_bool.png`.
- **Shader viewer (View):** the disassembly's constant table lists `stepColour`, `tint` and `useTint` under Parameters, and `useTint b3`, `tint c5`, `stepColour c9` under Registers. The code is `rep i0` with no `defi`, so CompileLoop did remove it. Crop: `zoom_shader_viewer.png`.
  - **Note:** the shader viewer only has a separate Constants pane while debugging, and D3D9 offers no debugging. So the names appear in the disassembly header rather than in a pane.
- **Texture viewer:** I picked the back buffer at 340, 265 (the triangle's centre). The status bar reads `0.37647, 0.74902, 0.00, 1.00`, which is 96/255 and 191/255. `PickPixel` from Python gives the same value at the centre and 3 px around it. Crops: `zoom_pick_statusbar.png` and `texture_viewer.png`.

## 4. D3D9_Mesh_Output

- **Demo logs:** the native run's and the capture run's logs are the same as last round's. They have the "Running D3D9 test" line and "Wrote the reference", and no TEST_WARN or TEST_ERROR.
- **References:** the reference written natively and the one written under RenderDoc are byte-identical. Both match the native reference from `d3d9_phase3e_retest2.md`, so moving the loop code into CompileLoop changed no output.
- **`d3d9_mesh_check.py`:** on the new capture (`D3D9_Mesh_Output_frame11382.rdc`) it gives **18 passed, 0 failed**. Its output is identical to that round's apart from the reference path.
