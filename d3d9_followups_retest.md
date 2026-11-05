# D3D9 follow-ups retest on 3cff00146

Everything passes, apart from the D3D9 game capture check, which I couldn't run.

- **What I built:** the branch tip d44ff43e9, Development x64, with 0 errors. That is 3cff00146 plus the previous retest report, with no code changes.
- **Machine:** the same as before (Windows 11, RTX 4070 Ti).

| # | Item | Result |
|---|---|---|
| 1 | `gpu_race.py` ×20, new qrenderdoc each run | **PASS** |
| 2 | `pipe_extra.py` on D3D9Ex_Wrapping_Check ×20, plus log checks | **PASS** |
| 3 | Normal qrenderdoc: GPU list in Settings, D3D11 capture loads | **PASS** |
| 4a | `d3d9_pipestate_check.py` on D3D9_Mesh_Output | **PASS** |
| 4b | D3D9 game capture with PS c#/i#/b# | **Not run**: I don't have one |
| 4c | Constant buffer viewer, D3D9 VS and D3D11 cbuffer | **PASS** |

## 1. `gpu_race.py`

- All 20 runs exited 0, and every run reached "done".
- `%LOCALAPPDATA%\CrashDumps` got no new dumps.
- **Before the fix:** the crash hunt saw 9 crashes in 20 runs.

## 2. `pipe_extra.py` on D3D9Ex_Wrapping_Check

- All 20 runs exited 0, and there were no new dumps.
- **Output:** identical to the 60 crash hunt runs, apart from the log file name and the log's line count. The log is now 40 lines, down from 52–54.
- **Every log has:**
  - exactly one D3D11, one D3D12 and one Vulkan proxy device;
  - one "GPU:" group;
  - no overlapping "Creating a VulkanReplay replay device";
  - no `d3d12_replay.cpp` assertion.
- **Before the fix:** every one of the 60 crash hunt logs had 2 of each proxy device. 3 of them had an overlapping Vulkan creation, and 3 had the `d3d12_replay.cpp` assertions.
- **The 20 `gpu_race.py` logs** also each show one set of proxy devices and one "GPU:" group, with no overlaps and no assertions. Before the fix each log had 6–7 Vulkan creations, 2–5 of them overlapping, and 6–10 assertions.

## 3. Normal qrenderdoc

- **Settings → Replay → GPU Selection Override** lists:
  - Default GPU selection;
  - NVIDIA GeForce RTX 4070 Ti [D3D11, D3D12, Vulkan];
  - Software WARP Rasterizer [D3D11, D3D12].

  Each API is listed once.
- **D3D11 capture:** D3D11_CBuffer_Zoo, opened with File → Open, loaded with "No problems detected".
- **Log:** one D3D12 and one Vulkan proxy device, and one "GPU:" group. It has two D3D11 devices: the proxy and the capture's own replay device.

## 4. GetCBufferVariableContents length 0

- **a.** I used the D3D9_Mesh_Output capture from the f5fd4ce7f retest. The script exited 0, and its output is identical to that round's apart from the log file name. All 10 "constant block N length 0:" lines say "same", with non-zero counts. The last line is "GetCBufferVariableContents length 0 matched the descriptor size everywhere".
- **b.** Not run. I have no D3D9 game capture, and no demo uses PS constants, so the PS c#/i#/b# path is still untested.
- **c.** I opened the viewers the way the Pipeline State "Go" button does, as in the last round:
  - **D3D9, event 1637, Vertex CB 0/1/2:** the values match the replay at full size and at length 0, and are identical to the last round. For example:
    - `tint` = 0.20, 0.30, 0.40, 1.00;
    - `i0` = 3, 0, 1, 0;
    - `useTint` true, `other` false.
  - **D3D11_CBuffer_Zoo, EID 11, Pixel CB 0 `consts`:** a = 0, 1, 2, 3; b = 4, 5, 6 … These match the full-size read, which is identical to the last round.
