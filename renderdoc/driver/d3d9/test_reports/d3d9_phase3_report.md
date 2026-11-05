# D3D9 phases 3a–3d: test report

Branch `claude/renderdoc-directx9-support-tix3lw` at `b8b377051`, built as Development | x64.

## Summary

- **Build:** no errors and no fixes, so `d3d9_phase3_fixes.patch` would be empty and wasn't created.
- **Runtime bugs:** four, found in 3a, and all four block testing.
  1. No capture loads, because the shader bytecode length isn't read back.
  2. `D3D9_Frame_Start_State` crashes in `GetPaletteEntries` while loading.
  3. Stencil is written back wrong when the initial contents are applied.
  4. Clears are neither replayed nor listed.

  A workaround for each was approved, so the rest of 3a–3d was tested with all four applied. The workarounds are committed together with this report. Each one is marked `WORKAROUND (phase 3 testing)` in the code, and the diff is included below. They are the smallest changes that unblocked testing, not reviewed fixes.
- **With the workarounds:** every automated check in 3a–3d passes, with one caveat: the 3a `_first.png` expectation no longer holds once Clears are actions (see the 3a table).
- **Disassembly:** 19 of 23 shaders match fxc instruction for instruction. The other 4 differ in three formatting details, listed under "fxc differences".
- **Not run:** the checks that need the qrenderdoc UI. They're in the checklist at the end.
- **Crashes:** one unexplained D3D9Ex crash, seen once and not reproduced since.

## Environment

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 4070 Ti |
| Driver | 610.88 (32.0.16.1088) |
| OS | Windows 11 Pro 10.0.26300 |
| Build | Visual Studio 2022, `renderdoc.sln` Development \| x64, 0 errors; `demos.sln` x64 |
| Python | No standalone 64-bit Python 3.6 is installed. All scripts, including the stock `util\test\d3d9_*.py`, ran in qrenderdoc's embedded Python 3.6 (`x64\Development\qrenderdoc.exe --python <wrapper>`, with the wrapper setting `sys.argv` and redirecting stdout). |
| fxc | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe` |

## Build

No build errors at `b8b377051`. The new Qt panel, the SWIG bindings and the new driver files all compiled first time, so nothing needed fixing.

`d3d9_phase3_fixes.patch` would be empty, so it wasn't created. The runtime workarounds below aren't build fixes.

## Runtime bugs

### Bug 1: no capture loads, because the shader bytecode length isn't read back

- **Repro:** capture any demo that creates a shader, then run `d3d9_replay_check.py <capture>.rdc`.
- **Log:**
  ```
  Failed creating vertex shader ResourceId::19, HRESULT: E_INVALIDARG   (d3d9_replay_device.cpp:184)
  ```
  A temporary log line added at the read showed `0 dwords, hr 80070057`.
- **Cause:** `SerialiseShaderBytecode` (`d3d9_device_serialise.cpp:2133`) calls `SERIALISE_ELEMENT_ARRAY(Bytecode, BytecodeLength)`. The `Serialise(name, byte *&, uint64_t byteSize)` overload takes the size by value (`serialiser.h:282`). On read, `BytecodeLength` therefore stays 0 and the array comes back empty, so the replay hits `code.empty() ? E_INVALIDARG`.
- **The capture itself is fine:** it holds the bytes (824 bytes for the vs_3_0, ending in the end token). A native test creates the shader from those bytes without error on a D3D9Ex device with mixed, hardware and software vertex processing.
- **History:** the write side dates from `eb0340f5f` and the read side from `01ad557c5`.
- **Other callers:** the typed overload (`serialiser.h:620`) is also by value. The vertex declaration and `*UP` draws only use the pointer, so they aren't affected.
- **Workaround:** `SERIALISE_ELEMENT(BytecodeLength);` before the array. This changes the capture format, so every demo was recaptured after it.

### Bug 2: `D3D9_Frame_Start_State` crashes in `GetPaletteEntries` while loading

- **Repro:** open the `D3D9_Frame_Start_State` capture. It crashes with 0xC0000005 inside `OpenCapture`.
- **Stack (cdb):**
  ```
  d3d9!Direct3DShaderValidatorCreate9+0x1b02e
  renderdoc!D3D9FetchDeviceState+0xdbb [d3d9_state.cpp @ 433]          GetPaletteEntries(3)
  renderdoc!WrappedD3DDevice9::ApplyReplayDeviceState+0x1ca [d3d9_replay_device.cpp @ 249]
  renderdoc!WrappedD3DDevice9::Serialise_BeginCaptureFrame<ReadSerialiser>+0xa3e [d3d9_device_serialise.cpp @ 2054]
  renderdoc!WrappedD3DDevice9::ContextReplayLog+0x41d
  renderdoc!WrappedD3DDevice9::ReadLogInitialisation+0xa94
  renderdoc!D3D9Replay::ReadLogInitialisation+0x4d
  renderdoc!ReplayController::PostCreateInit+0x3ac
  ... CaptureFile::OpenCapture
  ```
- **Last log lines:**
  - `Initial contents of ResourceId::16: Read back with depth and stencil tests`
  - `Applying initial contents`
  - `Applied 3`
- **Cause:** `ApplyReplayDeviceState` fetches the replay device's current state, and that fetch calls `GetPaletteEntries` for the palette numbers in the capture. The replay device has never set those palettes. A native test showed that on a D3D9Ex mixed-VP device, `GetPaletteEntries` on a palette that was never set crashes inside d3d9 instead of returning an error. That happened for palettes 0 and 3, and for palette 5 after palette 3 had been set.
- **Workaround:** in `ApplyReplayDeviceState`, call `SetPaletteEntries` for each captured palette before the fetch.

### Bug 3: stencil written back wrong when the initial contents are applied

- **Repro:** `d3d9_replay_check.py` on `D3D9_Capture_Edge_Cases`, at the first event. Depth bits are correct; stencil bits are wrong:

  | Depth stencil | Got | Expected |
  |---|---|---|
  | automatic | `0x4000007A` | `0x4000005A` |
  | MSAA, all samples | `0x80000077` | `0x80000033` |
  | INTZ | `0x5FFFFF13` | `0x5FFFFF0F` |

- **The capture's initial contents are correct:** they hold 5A, 33 and 0F, and the MSAA diagonal rule holds.
- **Cause:** `StencilWritePS` (`d3d9_depth.cpp:206`) computes `floor(s / params.x + 0.25f)`. Because the 0.25 is added after the divide, any bit where `s / bit` has a fraction of 0.75 or more rounds up. That turns 90 into 122 (0x7A), 51 into 119 (0x77) and 15 into 19 (0x13), which matches the wrong values exactly.
- **Workaround:** `floor((s + 0.25f) / params.x)`.

### Bug 4: Clears are neither replayed nor listed

- **Repro:** any capture. The event tree has no Clear actions.
- **Cause:** the replay branch of `Serialise_Clear` (`d3d9_device_serialise.cpp:152`, unchanged since `01ad557c5`) only calls `ReplayEndScene()`. It never calls `Clear()` and never adds an action.
- **Effects:**
  - `_last.png` still contains the previous frame's overlay; for `Simple_Triangle`, `_last` is identical to `_first`.
  - `Reset_Capture` frame 61 has the previous frame's blue background instead of red.
- **Workaround:** call `Clear(...)` and add an action with `Clear | ClearColor / ClearDepthStencil`. No `copyDestination` is set.

### The workarounds (committed with this report)

```diff
diff --git a/renderdoc/driver/d3d9/d3d9_depth.cpp b/renderdoc/driver/d3d9/d3d9_depth.cpp
index f20ff7ab0..0f69a524a 100644
--- a/renderdoc/driver/d3d9/d3d9_depth.cpp
+++ b/renderdoc/driver/d3d9/d3d9_depth.cpp
@@ -203,7 +203,7 @@ float4 params : register(c0);
 float4 main(float2 vpos : VPOS) : COLOR0
 {
   float s = tex2Dlod(values, float4((vpos + 0.5f) * params.zw, 0, 0)).r;
-  clip(fmod(floor(s / params.x + 0.25f), 2.0f) - 0.5f);
+  clip(fmod(floor((s + 0.25f) / params.x), 2.0f) - 0.5f);
   return 0.0f;
 }
 )";
diff --git a/renderdoc/driver/d3d9/d3d9_device_serialise.cpp b/renderdoc/driver/d3d9/d3d9_device_serialise.cpp
index 72482185e..59e75f475 100644
--- a/renderdoc/driver/d3d9/d3d9_device_serialise.cpp
+++ b/renderdoc/driver/d3d9/d3d9_device_serialise.cpp
@@ -152,6 +152,22 @@ bool WrappedD3DDevice9::Serialise_Clear(SerialiserType &ser, DWORD Count, CONST
   if(IsReplayingAndReading())
   {
     ReplayEndScene();
+
+    // WORKAROUND (phase 3 testing): the clear was neither replayed nor listed as an action
+    Clear(Count, pRects, Flags, Color, Z, Stencil);
+
+    if(IsLoading(m_State))
+    {
+      AddEvent();
+
+      ActionDescription action;
+      action.flags = ActionFlags::Clear;
+      if(Flags & D3DCLEAR_TARGET)
+        action.flags |= ActionFlags::ClearColor;
+      if(Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
+        action.flags |= ActionFlags::ClearDepthStencil;
+      AddAction(action);
+    }
   }
 
   return true;
@@ -2130,6 +2146,7 @@ static void SerialiseShaderBytecode(SerialiserType &ser, ShaderType *shader, rdc
     BytecodeLength = size;
   }
 
+  SERIALISE_ELEMENT(BytecodeLength);
   SERIALISE_ELEMENT_ARRAY(Bytecode, BytecodeLength).Important();
 
   if(ser.IsReading() && Bytecode && BytecodeLength >= sizeof(DWORD))
diff --git a/renderdoc/driver/d3d9/d3d9_replay_device.cpp b/renderdoc/driver/d3d9/d3d9_replay_device.cpp
index 0704fb9f5..9f6375cb7 100644
--- a/renderdoc/driver/d3d9/d3d9_replay_device.cpp
+++ b/renderdoc/driver/d3d9/d3d9_replay_device.cpp
@@ -238,7 +238,12 @@ void WrappedD3DDevice9::ApplyReplayDeviceState(const D3D9DeviceState &state)
     for(const D3D9LightState &l : state.Lights)
       m_LightIndices.Add(l.Index);
     for(const D3D9Palette &p : state.Palettes)
+    {
+      // WORKAROUND (phase 3 testing): GetPaletteEntries crashes in d3d9 on a palette that was
+      // never set, so set it before the fetch below asks for it
+      m_device->SetPaletteEntries(p.Number, p.Entries.data());
       m_PaletteNumbers.Add(p.Number);
+    }
   }
 
   // only setting what differs from the device's state takes care of the state that's left out
```

## Recapture

All 7 D3D9 demos were captured with this build: 8 captures in total, because `D3D9_Reset_Capture` produces two. No errors appeared in the RenderDoc or demo logs.

- **Captures:**
  - `Simple_Triangle`
  - `Frame_Start_State`
  - `Capture_Edge_Cases` (frame 62)
  - `Resource_Contents`
  - `Reset_Capture`: frame 30 (`_capture.rdc`) and frame 61
  - `Wrapping_Check`
  - `D3D9Ex_Wrapping_Check`
- **Reset_Capture:** logged `Device reset during a capture` twice, as expected.
- **Recapture after bug 1:** every demo was captured again once workaround 1 was in, since it changes the capture format.
- **Workarounds 2–4:** replay-only, so they didn't need another recapture.

## Results

The "HEAD" column shows each check at `b8b377051` with only workaround 1, which every load needs. The "With workarounds" column has all four applied.

### 3a, replay (`d3d9_replay_check.py <capture> --all`)

| Check | HEAD | With workarounds |
|---|---|---|
| Every capture loads | **FAIL**: 0/8 (bug 1); 7/8 with workaround 1, because Frame_Start_State crashes (bug 2) | **PASS** 8/8 |
| `--all` steps every action without crashing | PASS for the 7 that load | **PASS** 8/8 |
| Event tree: draws, clears, copies (`GetFrontBufferData`), presents | **FAIL**: no Clears anywhere (bug 4) | **PASS** |
| `_last.png` matches the window without the overlay | **FAIL**: overlay still there (bug 4) | **PASS**: Simple_Triangle grey background, 2 triangles, no overlay; Reset f61 red and the triangle. Edge_Cases red/green/blue blocks on grey; not compared against the live window. |
| `_first.png` is the previous frame with the overlay | PASS | **Expectation no longer holds** (see below) |
| Edge_Cases regions: "Depth" and "State blocks", with a "Multisampled" marker inside "Depth" | **PASS** | **PASS** |
| Edge_Cases depth at the first event, automatic | **FAIL**: `0x4000007A` (bug 3) | **PASS**: `0x4000005A` x10000, `0xBFFFFFA5` x10000, `0xFFFFFF00` x100000 |
| … MSAA, every sample | **FAIL**: `0x80000077` | **PASS**: samples 0–1 `0xFFFFFF00` x2080 / `0x80000033` x2016; samples 2–3 `0x80000033` x2080 / `0xFFFFFF00` x2016 |
| … D16 | **PASS** `0x7FFF` x1024 | **PASS** `0x7FFF` x1024 |
| … INTZ | **FAIL**: `0x5FFFFF13` | **PASS**: `0x5FFFFF0F` x1024 |
| Reset frame 30: "After the reset", a blue Clear and the triangle | **FAIL**: region and triangle present, no Clear | **PASS**: "After the reset" > Clear, DrawPrimitive. The frame has no Present, so the script saves no PNG, and the Clear's colour wasn't checked from an image. |
| Reset frame 61: a "Frame" marker, a red Clear and no reset | **FAIL**: no Clear, blue background | **PASS**: "Frame" > Clear, DrawPrimitive, Present; red background; no reset |
| UI: opens as D3D9; Event Browser and Resource Inspector work | NOT RUN | **NOT RUN** (UI checklist). A UI Python script confirmed Edge_Cases opens in qrenderdoc with API D3D9. |

`Wrapping_Check` and `D3D9Ex_Wrapping_Check` contain only a Clear and a Present.

**Why `_first.png` no longer matches:** with Clears listed as actions, the first event (1) is the frame's first Clear. `SetFrameEvent(1)` includes that event, so `_first.png` shows only the clear colour. The "previous frame with the overlay" expectation assumes event 1 does nothing. The script would have to look at the state before the first event to show it. This isn't a driver bug.

### 3b, texture viewer

All of these ran from Python. Output windows were tested headless with `CreateHeadlessWindowingData` and `ReadbackOutputTexture`, which exercises `RenderTexture`, `RenderMesh` and `GetOutputWindowData`.

| Check | Result |
|---|---|
| Simple_Triangle back buffer displays | **PASS**: the output shows a #333333 background, the #00FF00 triangle and the magenta FF triangle |
| Background picks as 0.2, 0.2, 0.2, 1 | **PASS**: PickPixel (5,5) = (0.2, 0.2, 0.2, 1); centre (0,1,0,1) |
| GetMinMax / GetHistogram | **PASS**: min/max (0,0,0,1)..(1,1,1,1); histogram over 0–1, RGB, 256 buckets: [0]=32160, [51]=308220, [255]=19620 (sum 360000 = 400×300×3) |
| Flip, red only, range 0.1–0.3 (output window) | **PASS**: flipped; red only shows grey, as single channels do; with range 0.1–0.3, 0.2 shows as #7F7F7F |
| Save as PNG (`SaveTexture`) | **PASS**: 5862 bytes |
| Stepping back: the back buffer changes with each draw | **PASS**: at (60,270), magenta from event 19 on and grey before; at (200,150), green from event 11 on and grey at events 6 and 1 |
| Returning to the last event looks the same | **PASS**: the output at the last event is byte-identical before and after stepping back |
| Resource_Contents: every texture, mip, cube face and volume slice | **PASS**: values below |
| Edge_Cases pick, automatic depth stencil | **PASS**: (50,50) 0.25 / 90, (150,50) 0.75 / 165, (300,200) 1.0 / 0 |
| … MSAA, every sample | **PASS**: below the diagonal 0.5 / 51, above it 1.0 / 0. On the diagonal at (32,32), samples 0–1 are 1.0 and samples 2–3 are 0.5. |
| … D16 | **PASS**: 0.49999 |
| … INTZ | **PASS**: 0.375 / 15 |
| Min/max of the automatic depth stencil, 0.25 to 1.0 | **PASS**: 0.25..1.0; depth histogram [63]=10000, [191]=10000, [255]=100000 |
| Mesh viewer, VS Input (event 11) | **PASS** from Python. Inputs: POSITION R32G32B32_FLOAT @0, COLOR R32G32B32A32_FLOAT @12, TEXCOORD R32G32_FLOAT @28, stream 0 stride 36. Positions (-0.5,-0.5,0) (0,0.5,0) (0.5,-0.5,0), colour green, UVs (0,0) (0,1) (1,0). The headless mesh output (VSIn, arcball) draws the triangle with axes. |
| Python PickPixel/GetMinMax/GetHistogram agree with the UI | **NOT RUN**: Python values above; the UI comparison is in the checklist |
| UI zoom, channel toggles, range sliders, auto-fit, histogram, Save Texture, mesh table and 3D view | **NOT RUN** (UI checklist) |

**Resource_Contents values** (PickPixel and GetMinMax at the first and last event, plus an output of every subresource):

| Resource | Shows |
|---|---|
| Managed 8×8 | Levels 11, 22, 33, 44. Level 0's centre is green at the first event and blue after its UnlockRect. |
| Default 16×16 | 203040, with A0B0C0 in the top-left of both levels and 5A5A5A at (15,15) |
| RT texture | Green top-left, then blue after the ColorFill; FF0000 elsewhere |
| MSAA RT | 00FFFF in every sample |
| Cube | Faces 10..60 |
| Volume | Slices 100..400 |
| Sysmem surface | 5A5A5A with a green, then blue, centre |
| Recycled textures | B9B966 (old) and BABA67 (new) |

**Other Edge_Cases textures:**
- **Depth targets as displayed at (0,0):** automatic #405A00 (depth in red, stencil in green), D16 #7F0000, INTZ #600F00.
- **MSAA RT:** resolved, yellow lower-left, with partial coverage on the diagonal.
- **State block texture:** zero; the demo never fills it.
- **Span texture:** blue.
- **ReleaseDC surface:** 0x654321 at the first event, 0x123456 after ReleaseDC and 0x808080 elsewhere.

All of these match what the demo writes.

### 3c, pipeline state

| Check | Result |
|---|---|
| `d3d9_pipestate_check.py` on all 8 captures | **PASS**: every run exits 0 with no exceptions; pipelineType and localRenderer are D3D9. Wrapping_Check and Ex have no draws, so only the header prints. |
| `GetD3D9PipelineState()` is a `D3D9State`; generic PipeState agrees | **PASS**: the D3D11/D3D12/GL/Vulkan getters return None, and `IsD3D(D3D9)` is True. The generic PipeState values (topology, vertex inputs, shaders, outputs, depth target, viewport, read-only resources) match the D3D9State values in the rows below. The stock script prints both side by side but doesn't assert, so this was checked by reading the output. |
| Simple_Triangle: TriangleList; vertex elements match the declaration | **PASS**: event 11 is TriangleList; Vertex Declaration 18 has stream 0 +0 POSITION0 FLOAT3, +12 COLOR0 FLOAT4, +28 TEXCOORD0 FLOAT2 (DEFAULT); stream 0 is Vertex Buffer 21, offset 0, stride 36, freq 1; `GetVertexInputs` agrees. Event 19 (DrawPrimitiveUP, FF) shows "declaration - fvf 0x44": POSITIONT0 FLOAT4 + COLOR0 D3DCOLOR. |
| Shader rows show their shader model; constants 256/16/16 for the VS and 224/16/16 for an SM3 PS | **PASS** from Python: VS 19 vs_3_0, PS 20 ps_3_0, both with reflection. Constants 256/16/16 (4096/256/64 bytes) and 224/16/16 (3584/256/64). Constant block 0 is "VS float constants (c#)". |
| Frame_Start_State: c200 and c180 | **PASS**: VS c10 (1,2,3,4), c200 (13,14,15,16), i3 (5,6,7,8), b2 1; PS c20 (9,10,11,12), c180 (17,18,19,20) |
| … render states | **PASS**: fog colour (0.67,0.80,0.94) = 0xABCDEF, fog start 0.25, ambient (0.0627,0.125,0.188) = 0x102030, stencil ref 90. The raw renderStates (102) match: FOGCOLOR 11259375, FOGSTART 0x3E800000, AMBIENT 0x102030, STENCILREF 90. |
| … sampler and texture stage states | **PASS**: pixelSamplers[3].maxAnisotropy 7, vertexSamplers[1].addressU Mirror, textureStages[5].texCoordIndex 3 |
| … transforms | **PASS**: VIEW translation (1,2,3); WORLDMATRIX(5) scale 2 |
| … scissor, material | **PASS**: scissor rect 1,2,3,4, disabled; material diffuse (0.1,0.2,0.3,0.4), power 5 |
| … lights | **PASS**: index 7 POINT, enabled, diffuse (1,0.5,0.25,1), pos (10,20,30), range 100, att0 1; index 42 DIRECTIONAL, dir (0,-1,0), not enabled |
| … clip planes | **PASS**: clipPlanes[1] (1,2,3,4), not enabled |
| … D3DTSS_CONSTANT on stage 0 | **PASS (default only)**: the field is there and reads (0,0,0,0). The demo never sets D3DTSS_CONSTANT, so only the default could be checked. |
| Output merger: targets and depth | **PASS**: see the Edge_Cases details below |
| Output merger: blend, alpha test, two-sided stencil | **Not testable**: no demo enables blending, alpha test or two-sided stencil. The defaults show correctly: One/Zero/Add, write mask 0xF, no independent blending. |
| Bound textures | **PASS** from Python: the span "Texture 31" is on sampler 0 and in `GetReadOnlyResources(Pixel)`. The inputs strip in the UI wasn't checked. |
| Resource_Contents | **PASS**: indexed draw event 16 shows Index Buffer 22, stride 2. FF draws list only stage 0's texture as a read-only resource (8×8 B8G8R8A8 with 4 mips, 16×16 with 2 mips, the 32×32 RT texture, recycled Texture 21388). The cube and volume on samplers 1–2 aren't listed because stage 1 is disabled, as STATUS says. |
| Reset_Capture | **PASS**: after the reset there are no shaders and no VB (the device state was reset); rt0 is Backbuffer 23 with Auto Depth Stencil 22 (the new ones) |
| `pyrenderdoc.CurD3D9PipelineState()` in the UI's Python shell | **PASS** (`qrenderdoc <rdc> --ui-python`): it's a D3D9State, `CurD3D11PipelineState()` is None, and it matches the controller at the loaded event 22 (topology, rt0, all 102 render states identical) |
| The D3D9 panel's IA, VS, RS, PS, OM and Render States pages; double-click stream 0; shader rows open the disassembly | **NOT RUN** (UI checklist) |
| Export to HTML | **NOT RUN**: it needs the file dialog (UI checklist) |

**Edge_Cases output merger details:**

| Event | State |
|---|---|
| 22, 31 | Stencil enabled ALWAYS/REPLACE, ref 90 / 165, write mask 0xFF; depth enabled, func Always |
| 44 | MSAA "Render Target 19" + "Depth Stencil 20", 64×64 viewport, stencil ref 51 |
| 52, 54 | "Render Target Texture 21" with D16 "Depth Stencil 23", then INTZ "Depth Texture 24" (which shows as D24S8); viewport 32×32; depth and stencil off; texture factor 0xFF010203 |
| 81 | Alpha ref 0x10 (frame 62 is even); texture factor 0xFF040506; stencil back to its defaults, because the pixel state block's Apply restores them every frame, as on the device |
| 1 | Alpha ref 0x20 and texture factor 0xFF040506, carried over from frame 61; VS c20 (1,2,3,4) from the recorded state block; c200 (200..203) is the sentinel |

### 3d, shaders

| Check | Result |
|---|---|
| `renderdoccmd.exe test unit "[d3d9]"` | **PASS**: "All tests passed (5331 assertions in 2 test cases)", exit 0 |
| Simple_Triangle VS inputs POSITION, COLOR, TEXCOORD | **PASS**: VS 19 vs_3_0 inputs POSITION0 (reg 0, used 0x7), COLOR0 (reg 1), TEXCOORD0 (reg 2, used 0x3); outputs POSITION0, COLOR0, TEXCOORD0 |
| PS output is COLOR0 | **PASS**: PS 20 ps_3_0 input COLOR0, output COLOR0 (sv ColorOutput) |
| The only disassembly target is "D3D9 Bytecode" | **PASS**: `GetDisassemblyTargets(True/False)` returns `['D3D9 Bytecode']`; `GetTargetShaderEncodings()` returns `[]`, as STATUS says; entry point `main`, encoding D3D9Bytecode |
| Demo shaders vs fxc | **PASS**: the demo compiles with SKIP_OPTIMIZATION \| DEBUG, so it was compared against `fxc /Od /Zi /Fc`. The instructions are identical. fxc also prints its `/Zi` debug-info comments after instructions (`// IN<0,1,2>`, `// ::main<0,1,2,3>`), which RenderDoc doesn't show because there's no source-level debug info (known gap). |
| Edge_Cases depth shaders, extra ps_1_x/ps_2_x shaders, previously unverified forms vs fxc | **PASS, with 3 formatting differences** in 4 of 23 shaders (see "fxc differences") |
| Named uniforms show the application's values | **PASS** from Python (values below) |
| `def` constants show their literals | **PASS**: c7 (1,0,0.5,0), c11 (0.25,0.5,2,3.14159); asm.ps_3_0 i0 (2,0,1,0) and b1 1; asm.vs_3_0 i0 (3,0,1,0) and c200 (1,0.5,0.25,0) |
| Shader viewer disassembly and constants by name in the UI | **NOT RUN** (UI checklist) |

**How the extra shaders were tested.** The demos have only one SM3 VS/PS pair, and it has no uniforms. Edge_Cases' depth shaders are RenderDoc's own, from `d3d9_depth.cpp`; Edge_Cases itself is fixed function. To cover more, a scratch app outside the repo creates 23 shaders on a D3D9 HAL device, draws with each one in the captured frame, and captures that frame with the in-app API (`TriggerCapture`). The shaders are:

| Group | Shaders | Built with |
|---|---|---|
| Demo | default VS and PS | D3DCompile |
| RenderDoc's depth shaders (StencilWritePS includes workaround 3) | DepthVS, DepthTestPS, StencilTestPS, CombinePS, DepthWritePS, StencilWritePS | D3DCompile |
| HLSL | uniforms vs_3_0 and ps_3_0, vs_2_0, ps_2_0, ps_2_a | D3DCompile |
| Legacy HLSL | vs_1_1, ps_1_1, ps_1_4 | D3DXCompileShader with `D3DXSHADER_USE_LEGACY_D3DX9_31_DLL` |
| Hand-written assembly, covering the forms STATUS lists as unverified | vs_1_1, ps_1_1, ps_1_3, ps_1_4, ps_2_x, ps_3_0, vs_3_0 | D3DXAssembleShader |

RenderDoc found all 23 in the capture, with `rawBytes` byte-identical to what was created, and the HLSL bytecode is identical to what real fxc produces.

Each shader was compared against:
- D3DDisassemble, which gives the same output as `fxc /Fc`;
- D3DXDisassembleShader;
- real fxc output for the HLSL ones.

Comments and blank lines were compared separately from the instructions.

**Reflection** (scratch capture). PASS: the names and types from the constant table match the HLSL.

- **uniforms.vs_3_0:**
  - `wvp` 4×4, `rm` 3×4 row major, `arr` float4[3], `s` struct {a float4, b float2};
  - `loopCount` int4 in the integer block, `useArr` bool in the boolean block;
  - the literals c7 and c11 by register.
- **uniforms.ps_3_0:** `tint`, `parr[2]`, `invert` (in the bool block); samplers `tex` (2D) and `cube` (TextureCube), bound at 0 and 1.
- **legacy ps_1_1:** samplers tex0 and tex1.
- **Inputs:**
  - ps_3_0 VPOS (sv Position) and VFACE (sv IsFrontFace);
  - ps_1_1 COLOR0 + TEXCOORD0/1.
- **Outputs:** vs_1_1/vs_2_0 outputs POSITION, FOG, PSIZE, COLOR# and TEXCOORD#, from oPos, oFog, oPts, oD# and oT#.
- **Shaders with no constant table:** a relatively addressed constant becomes an array that reaches up to the next used register. For example, asm.vs_1_1 has c10 ×85, up to c95; asm.vs_3_0 has c4 ×4, and c8 up to c200.

**Constants** (from `GetCBufferVariableContents`, called with the descriptor's offset and size).

The app set:
- VS c[r] = (10r .. 10r+3), i0 (2,0,1,0), b0 TRUE;
- PS c[r] = (100+10r ..), b0 TRUE.

RenderDoc shows:

| Variable | Value |
|---|---|
| `wvp` | rows (0,10,20,30), (1,11,21,31), …; registers are columns |
| `rm` | rows = c4, c5, c6 |
| `arr` | c8..c10 |
| `s.a` | c12 |
| `s.b` | c13.xy = (130,131) |
| `loopCount` | (2,0,1,0) |
| `useArr` | 1 |
| `tint` | (100..103) |
| `invert` | 1 |
| legacy `k` | (100..103) |
| `palette` | c4..c7 |

All of these are the application's values.

**Logs:** no "Couldn't fully parse shader", "debug shader" or other warnings in either run.

## fxc differences

These are instruction differences between RenderDoc's disassembly and fxc (`/Fc`, or D3DDisassemble, which gives the same output). 19 of the 23 shaders are identical. (I said 18 of 23 when I reported phase 3d. That counted asm.vs_3_0, which differs only from D3DX's output, not from fxc's.)

1. **Swizzles with a repeated trailing component.** fxc prints all four components; RenderDoc drops the repeats.

   | Shader | fxc | RenderDoc |
   |---|---|---|
   | combine.ps_3_0 (RenderDoc's own CombinePS) | `texldl r0, r0.xyww, s0` | `texldl r0, r0.xyw, s0` |
   | asm.vs_1_1 | `mov oT1.xy, v2.yxxx` | `mov oT1.xy, v2.yx` |

   Across all 23 shaders, fxc never printed a 2- or 3-letter source swizzle. It prints either all four letters or a single replicated one (`.x`). The 4-letter swizzles it printed were `.xyww .yxxx .xxxy .xyzx .yyyx .xyxy .yxyx .zwzw .wzyx .yxzw`. The mismatch is STATUS's rule that "`.xyzz` is `.xyz`".

2. **ps_1_x co-issue.** fxc prints the `+` two columns to the left, followed by a space; RenderDoc prints `+` directly before the opcode.

   | Shader | fxc | RenderDoc |
   |---|---|---|
   | asm11.ps_1_1 | `  + mov r0.w, t0_bias.w` | `    +mov r0.w, t0_bias.w` |
   | asm14.ps_1_4 | `  + mov_d4 r0.w, r4.w` | `    +mov_d4 r0.w, r4.w` |

3. **ps_1_x complement.**

   | Shader | fxc | RenderDoc |
   |---|---|---|
   | asm11.ps_1_1 | `sub_d2 r1.w, t1.w, 1-v0.w` | `sub_d2 r1.w, t1.w, 1 - v0.w` |

**Previously unverified forms, all identical to fxc:**
- **Modifiers and their order:** `mul_x2_sat`, `mul_sat_pp`, `mov_sat_pp r1, -v1_abs`, `add_sat_pp`, `mul_x4`, `add_d2`, `+mov_d4`.
- **Source modifiers:** `r0_bx2`, `t0_bias.w`, `r1_dz`.
- **Predicates:** `(p0) mov`, `(!p0.x) add_sat_pp`.
- **Declarations:** `dcl_texcoord1_centroid v1`, `dcl vFace`, `dcl vPos.xy`, `defi`, `defb b1, true`.
- **Relative addressing:**
  - `v0[aL]` (ps_3_0);
  - `o1[aL]` and `c8[aL]` (vs_3_0);
  - `c4[a0.x]`;
  - vs_1_1 `c[a0.x + 10]`, shown as `c10[a0.x]`.
- **Texture instructions:** `texm3x3pad`/`texm3x3tex`, `texcrd`, `phase`, `texldd`, `texldl`.
- **Flow control:** `rep i0`, `if b0`/`else`, `if_lt`, `loop aL, i0`.
- **Formatting:**
  - ps_1_x `.rgb` is shown as `.xyz`;
  - `def` floats are printed with 9 significant digits (3.14159012).

**Comments** (the constant table) differ only in formatting:
- RenderDoc pads the Registers table's Name column to the longest name, while fxc pads it to at least 12 (`------------`).
- fxc puts a `//   ` line before and after each struct in Parameters.
- RenderDoc ends with `// N instructions`; for some shaders fxc ends with `// approximately N instruction slots used` instead.

**Not a RenderDoc difference:** D3DXDisassembleShader prints asm.vs_3_0's `mov o1[aL], r0` as `mov o1[x0.x], aL, r0`. fxc/D3DDisassemble prints `o1[aL]`, the same as RenderDoc, so this is a D3DX quirk.

## Observations (not clearly bugs)

- **`parr[0]` shows a literal instead of the app's value.**
  - uniforms.ps_3_0's constant table puts `parr` at c1 with size 2. fxc also put `def c1, 1, 0.001, 0, 0` there, because `parr[0]` is never read.
  - So `parr[0]` shows the literal (1, 0.001, 0, 0) rather than the app's c1.
  - That is what the shader reads, as STATUS describes, but in the UI it will look like a wrong value for `parr[0]`.
- **vs_1_1/vs_2_0 output signatures reuse regIndex across register types:** POSITION 0, FOG 1, PSIZE 2, COLOR0 0, COLOR1 1, TEXCOORD0 0, TEXCOORD1 1. They come from different register files.
- **`PipeState.GetSamplers(stage, False)` is always empty,** for every stage and event, even with a texture on sampler 0. Sampler states are only in D3D9State (STATUS says the descriptors are texture + sampler together), so a generic script that asks for samplers gets nothing.
- **A null read-only resource on some FF draws:** a draw with no texture on stage 0 (for example Simple_Triangle event 19) lists "-" (null) at index 0, because stage 0's colour op isn't DISABLE.
- **INTZ shows as D24S8:** the INTZ depth target's format is reported as D24S8.
- **`GetVBuffers()` / `GetIBuffer()` byteSize is `0xFFFFFFFFFFFFFFFF`:** this is PipeState's "rest of the buffer" convention, which D3D11 uses too (`pipestate.inl:624/635`). Not a bug. `GetVBuffers` lists all 16 streams.
- **The palette isn't part of D3D9State,** so it can't be checked from the pipeline state.
- **`GetCBufferVariableContents` with `length` 0 returns zeros:** `ReplayController` skips `GetBufferData` (`replay_controller.cpp:1747`), so every variable except the literals reads 0. My first 3d run made this mistake. The UI passes the descriptor's byteSize, so it isn't affected.

## Logs and crashes

The logs were checked for errors and for warnings mentioning "Couldn't write the contents", "Couldn't decode", "Couldn't copy", "debug shader" or "Couldn't fully parse shader".

- **Capture logs:** none of these, for any demo.
- **Replay logs with the workarounds:** none of these, across 3a, 3b, 3c and 3d. RenderDoc deletes its log on a clean exit, so the scripts read `rd.GetLogFile()` before shutting down.
- **Before the workarounds:** the bug 1 and bug 2 lines quoted above.

**Unreproduced crash (D3D9Ex):**
- **What happened:** the first `pipe_extra.py` run on `D3D9Ex_Wrapping_Check` exited with 0xC0000005 after 7 s, with no output. That places it during the load, `SetFrameEvent(1)` or the first `GetD3D9PipelineState`.
- **Windows Application log:** qrenderdoc.exe, faulting module unknown, fault offset 0x0 (a call through a null pointer). Report id `f156ae8b-0aac-461e-8abc-2452fd0ea49e`.
- **Reproduction attempts:** 1 run under cdb, 4 normal `pipe_extra.py` runs and 2 stock-script runs were all clean. No stack is available.
- **Possibly related:** the next process, on Edge_Cases, logged this once and never in any other run:
  ```
  d3d12_replay.cpp(109/110): Assertion failed: GetModuleHandleA("d3d12.dll") == 0   (and d3d12core.dll)
  ```

## UI checklist (NOT RUN, for you)

Use a build of the commit that adds this report, since it includes the workarounds. Without them, most captures won't load. Recapture with that build too: workaround 1 changes the capture format, so captures made at `b8b377051` won't load either.

**3a**
- [ ] Every capture opens as D3D9 (title bar and status bar).
- [ ] The Event Browser shows the regions, markers, Clears, draws, copies and presents listed in the Edge_Cases output below.
- [ ] Resource Inspector lists the resources and opens them.

**3b** (Simple_Triangle back buffer; Edge_Cases depth targets)
- [ ] Hovering and picking matches the Python values: background 0.2, 0.2, 0.2, 1; Edge_Cases depth/stencil as in the 3b table.
- [ ] Zoom, flip, channel toggles, range sliders and auto-fit work.
- [ ] The histogram matches the Python histogram.
- [ ] Save Texture as PNG works.
- [ ] Thumbnail strip.
- [ ] Mesh viewer, VS Input: the table lists the 3 vertices and the 3D view shows the triangle.

**3c**
- [ ] The D3D9 panel has IA, VS, RS, PS, OM and Render States pages.
- [ ] The triangle diagram.
- [ ] Simple_Triangle: double-clicking stream 0 opens the vertices in the buffer viewer.
- [ ] The shader rows show vs_3_0 / ps_3_0, and clicking one opens the disassembly.
- [ ] The constants rows show 256/16/16 and 224/16/16, and open the buffer viewer. On Frame_Start_State, check c200 = (13,14,15,16) and c180 = (17,18,19,20).
- [ ] Frame_Start_State: the render, sampler and texture stage states, transforms, lights, material and clip planes show the 3c values.
- [ ] Textures tables and the inputs strip: Edge_Cases event 81 shows the span Texture 31; double-click opens it.
- [ ] The fixed-function tables aren't in italics.
- [ ] The texture viewer's follow outputs.
- [ ] Export to HTML.

**3d**
- [ ] Simple_Triangle: clicking the VS/PS rows opens the disassembly, matching fxc apart from the three differences above.
- [ ] The shader viewer's inputs/outputs and constants panes.
- [ ] Constants are listed by name with the app's values.

  The scratch capture with the 23 shaders is the better test for named constants: `<scratch>\p3\sh\cap\shtest_frame4.rdc` (see "Files").

## Full `d3d9_replay_check.py` output: `D3D9_Capture_Edge_Cases` (all four workarounds)

```
Loaded in 0.42s

Events:
    2 Depth
      3 IDirect3DDevice9::Clear()
     22 IDirect3DDevice9::DrawPrimitiveUP() (4 indices, 1 instances)
     31 IDirect3DDevice9::DrawPrimitiveUP() (4 indices, 1 instances)
     32 Multisampled
     35 IDirect3DDevice9::Clear()
     44 IDirect3DDevice9::DrawPrimitiveUP() (3 indices, 1 instances)
     52 IDirect3DDevice9::DrawPrimitiveUP() (4 indices, 1 instances)
     54 IDirect3DDevice9::DrawPrimitiveUP() (4 indices, 1 instances)
     57 D3DPERF_EndEvent()
   58 State blocks
     66 D3DPERF_EndEvent()
   81 IDirect3DDevice9::DrawPrimitiveUP() (4 indices, 1 instances)
   84 IDirect3DSurface9::ReleaseDC()
   85 IDirect3DDevice9::GetFrontBufferData()
   86 IDirect3DDevice9::Present()

Textures:
  Auto Depth Stencil 16           400x300  mips  1 slices 1 samples 1 D24S8 TextureCategory.DepthTarget
  Backbuffer 17                   400x300  mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.SwapBuffer|ColorTarget
  Render Target 19                 64x64   mips  1 slices 1 samples 4 B8G8R8A8_UNORM TextureCategory.ColorTarget
  Depth Stencil 20                 64x64   mips  1 slices 1 samples 4 D24S8 TextureCategory.DepthTarget
  Render Target Texture 21         32x32   mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.ColorTarget|ShaderRead
  Depth Stencil 23                 32x32   mips  1 slices 1 samples 1 D16 TextureCategory.DepthTarget
  Depth Texture 24                 32x32   mips  1 slices 1 samples 1 D24S8 TextureCategory.DepthTarget|ShaderRead
  Texture 26                        4x4    mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.ShaderRead
  Texture 31                        4x4    mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.ShaderRead
  Offscreen Surface 32             32x32   mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.NoFlags
  Offscreen Surface 33           3840x2160 mips  1 slices 1 samples 1 B8G8R8A8_UNORM TextureCategory.NoFlags

At the first event (1):
  saved ...\p3\cap\D3D9_Capture_Edge_Cases\D3D9_Capture_Edge_Cases_frame62_first.png
  Auto Depth Stencil 16 (D24S8, sample 0): 0xFFFFFF00 x100000, 0x4000005A x10000, 0xBFFFFFA5 x10000
  Depth Stencil 20 (D24S8, sample 0): 0xFFFFFF00 x2080, 0x80000033 x2016
  Depth Stencil 20 (D24S8, sample 1): 0xFFFFFF00 x2080, 0x80000033 x2016
  Depth Stencil 20 (D24S8, sample 2): 0x80000033 x2080, 0xFFFFFF00 x2016
  Depth Stencil 20 (D24S8, sample 3): 0x80000033 x2080, 0xFFFFFF00 x2016
  Depth Stencil 23 (D16, sample 0): 0x7FFF x1024
  Depth Texture 24 (D24S8, sample 0): 0x5FFFFF0F x1024

At the last event (86):
  saved ...\p3\cap\D3D9_Capture_Edge_Cases\D3D9_Capture_Edge_Cases_frame62_last.png

Stepping through 16 actions
  done in 0.12s

[exit code 0]
```

(`...` is `<scratch>`; see "Files".)

At HEAD with only workaround 1, the same run lists no Clear events. Its depth lines are the same, except that the stencil bytes are 7A (automatic), 77 (MSAA) and 13 (INTZ); see bug 3.

## Files

The workarounds are in the same commit as this report. Everything else below is on the test machine only and is not in the repo. `<scratch>` is `C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad` on that machine.

- **Captures and per-capture outputs:** `<scratch>\p3\cap\<demo>\`
  - `*_replay.txt`: 3a;
  - `*_tex.txt` and PNGs: 3b;
  - `*_pipe.txt`, `*_pipex.txt` and `*_pipedump.txt`: 3c;
  - `*_sh.txt`: 3d (Simple_Triangle only).
- **Scripts and notes:** `<scratch>\p3\`
  - scripts: `tex_check.py`, `pipe_extra.py`, `ui_check.py`, `sh_check.py`;
  - `notes_3a.md`: the notes for every phase.
- **3d scratch app:** `<scratch>\p3\sh\`
  - `shtest.cpp` and the shader sources in `src\`;
  - `out\<name>.bin`, `.rd.txt` (RenderDoc), `.d3dc.txt` (D3DDisassemble) and `.d3dx.txt` (D3DX);
  - `fxc\`: real fxc output;
  - `compare_out.txt` and `sh_check_out.txt`.
