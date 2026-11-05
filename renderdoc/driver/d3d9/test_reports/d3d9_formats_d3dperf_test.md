# D3D9 texture formats and D3DPERF markers test on 2836c73a1

- **What I built:** 2836c73a1, `renderdoc.sln` and `util\test\demos.sln`, Development x64. Both built with 0 errors and 0 warnings.
- **Machine:** the same as before (Windows 11, RTX 4070 Ti, driver 610.88).
- **Files:** in `d3d9_formats_d3dperf_test\`, as listed under "Files" at the end.

| # | Item | Result |
|---|---|---|
| 1 | D3D9_Texture_Formats natively | **PASS**: 31 formats supported, 17 not, reference written |
| 2 | `d3d9_formats_check.py` | **Script bug**, then **FAIL** for DXT2–5, ATI1 and ATI2 with the script patched. The references are byte-identical. |
| 3 | ATI2 texel in qrenderdoc against the render target | **Red and green swapped**, and most of the ATI2 data is missing from the capture |
| 4 | D3D11_D3DPERF_Markers: native, capture, `d3d11_d3dperf_check.py`, Event Browser | **Script bug**, then **PASS** with the script patched. The return values differ between native and RenderDoc. |
| 5 | `run_tests.py` D3D11_Refcount_Check | **PASS** (see the note on how the runner was started) |

## Summary of findings

1. **ATI2 channel order.** D3D9 samples ATI2 with red taken from the **second** 8-byte half of each block and green from the **first**, plus blue = 1 and alpha = 1. RenderDoc decodes it as BC5_UNORM (red from the first half), so its red and green are swapped. Block 0 is the only block whose data survives (see 2), and it shows the swap:
   - **RenderDoc decode at texel 0,0:** 0.48627, 0.65882.
   - **GPU (native and replay render target):** 0.657359, 0.484031, 1, 1.
   - **Over all 256 texels:** I rebuilt the demo's data with its generator and decoded it to the D3D spec (see 2). Swapped order matches the native GPU within 0.024. BC5 order is off by up to 1.0.
2. **ATI1 and ATI2 lose data at capture.** The cause is that the driver reports Pitch for ATI1 and ATI2 as one byte per texel column.
   - **The driver's Pitch:** `LockRect` reports Pitch = 16 for a 16×16 ATI1 or ATI2 texture, and 64 for 64×64. `pitch.cpp` measures this natively. DXT1 and DXT5 report the real row-of-blocks size (32 and 64 for 16×16).
   - **How the GPU reads the data:** the blocks are tightly packed, one row of blocks right after another (32 bytes per row for ATI1 and 64 for ATI2 at 16×16). It does not use the reported Pitch.
   - **The demo:** `FillRow` writes each row of blocks at `row * lr.Pitch`, so with Pitch = 16 the rows overlap. Simulating those writes and reading the result tightly packed reproduces the native reference: ATI1 within 0.0054, ATI2 within 0.024 with swapped channels.
   - **The capture:** `GetTextureData` on the captured texture has only the first 16 bytes (= Pitch) of each row of blocks, copied from offset `row * 16` of the app's memory. The rest of each row is zero. `bc_analyse_output.txt` has the hex dump.
   - **Effect:** the replay render target differs from the native one by up to 1.0 for ATI1 and ATI2. The texture viewer shows data only in the first column of blocks.
   - **Fix:** both the demo and the capture code need the tightly-packed row-of-blocks size for ATI1 and ATI2, not the Pitch that `LockRect` reports.
3. **DXT2–5 CPU decoding.** RenderDoc's CPU decode of BC2 and BC3 (PickPixel, and the texture viewer's values) switches the colour block to BC1's 3-colour mode (½ mix + transparent black) when c0 ≤ c1. The GPU always decodes the colour block of DXT2–5 in 4-colour mode, as the D3D spec says for BC2 and BC3.
   - **Evidence:** with forced 4-colour mode, my decode of the captured bytes matches the native GPU within 0.016 on r, g and b for all four. With BC1-style mode switching it matches RenderDoc's PickPixel within 0.0033.
   - **Not affected:** the captured bytes are identical to the demo's data, and the replay render target matches native exactly. Only the CPU decode is wrong. DXT1's 3-colour mode is decoded correctly.
4. **Check script bugs.** I ran both scripts as committed, then with a local patch for these bugs only. The patched copies aren't committed.
   - **`d3d9_formats_check.py`:** lines 119 and 176 call `controller.GetTexture(id)`, which `ReplayController` doesn't have. I patched it to `next(t for t in controller.GetTextures() if t.resourceId == id)`.
   - **`d3d11_d3dperf_check.py`:** line 43 uses `rd.ActionFlags.PushPopMarker`, which doesn't exist. `PushMarker` does, and I patched it to that.
5. **Tolerance.** NVIDIA's BC4/BC5 decode differs from the spec's interpolation by up to 0.024 in 6-value-mode blocks (r0 ≤ r1). Example: endpoints 20 and 252, spec 0.4424, GPU 0.4183. That is above the script's 0.02 block tolerance. It only matters once ATI1 and ATI2 are fixed and compared against the GPU.

## 1. Native D3D9_Texture_Formats

The demo log, from `demos_x64.exe D3D9_Texture_Formats --log ...`:

```
d3d9_test.cpp:224 Log: Running D3D9 test on NVIDIA GeForce RTX 4070 Ti
d3d9_texture_formats.cpp:299 Log: Not supported: R8G8B8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: R3G3B2 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: A8R3G3B2 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: X4R4G4B4 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: A8B8G8R8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: X8B8G8R8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: A8P8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: P8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: A4L4 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: L6V5U5 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: A2W10V10U10 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: CxV8U8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: Q16W16V16U16 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: UYVY (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: YUY2 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: R8G8_B8G8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:299 Log: Not supported: G8R8_G8B8 (CheckDeviceFormat)
d3d9_texture_formats.cpp:300 Log: 31 formats supported, 17 not
d3d9_texture_formats.cpp:182 Log: Wrote the reference to E:\Projects\renderdoc\util\test\D3D9_Texture_Formats_reference.txt
```

- **Errors:** there are no TEST_WARN or TEST_ERROR lines, and no format failed at CreateTexture or LockRect.
- **Screenshot:** `native_D3D9_Texture_Formats.png`. The empty cell in the top row is A8, which samples as (0, 0, 0, a) and so shows black.

## 2. Capture and `d3d9_formats_check.py`

- **The capture:** `D3D9_Texture_Formats_frame14382.rdc`, made with ExecuteAndInject and TriggerCapture as before. The demo log under RenderDoc has the same lines, ending in "Wrote the reference to ...\D3D9_Texture_Formats_reference_rdoc.txt".
- **References:** `D3D9_Texture_Formats_reference_rdoc.txt` is **byte-identical** to the native `D3D9_Texture_Formats_reference.txt` (same SHA-256, and `fc /b` finds no differences). Both are in the folder.

### The script as committed

```
Reference E:\Projects\renderdoc\util\test\D3D9_Texture_Formats_reference.txt: 31 formats
Traceback (most recent call last):
  ...
  File "E:\Projects\renderdoc\util\test\d3d9_formats_check.py", line 256, in main
    ok = check_format(controller, name, draw, ref)
  File "E:\Projects\renderdoc\util\test\d3d9_formats_check.py", line 119, in check_format
    desc = controller.GetTexture(tex)
AttributeError: 'renderdoc.ReplayController' object has no attribute 'GetTexture'
```

### The full output with `GetTexture` patched to a `GetTextures()` lookup

```
Reference E:\Projects\renderdoc\util\test\D3D9_Texture_Formats_reference.txt: 31 formats
PASS A8R8G8B8       256 texels, max diff 0.000000 (B8G8R8A8_UNORM, compared r,g,b,a)
PASS X8R8G8B8       256 texels, max diff 0.000000 (B8G8R8A8_UNORM, compared r,g,b,a)
PASS R5G6B5         256 texels, max diff 0.002801 (B5G6R5_UNORM, compared r,g,b,a)
PASS X1R5G5B5       256 texels, max diff 0.002657 (B5G5R5A1_UNORM, compared r,g,b,a)
PASS A1R5G5B5       256 texels, max diff 0.002657 (B5G5R5A1_UNORM, compared r,g,b,a)
PASS A4R4G4B4       256 texels, max diff 0.000000 (B4G4R4A4_UNORM, compared r,g,b,a)
PASS A8             256 texels, max diff 0.000000 (A8_UNORM, compared r,g,b,a)
PASS A2B10G10R10    256 texels, max diff 0.000000 (R10G10B10A2_UNORM, compared r,g,b,a)
PASS G16R16         256 texels, max diff 0.000000 (R16G16_UNORM, compared r,g,a)
PASS A2R10G10B10    256 texels, max diff 0.000000 (B10G10R10A2_UNORM, compared r,g,b,a)
PASS A16B16G16R16   256 texels, max diff 0.000000 (R16G16B16A16_UNORM, compared r,g,b,a)
PASS L8             256 texels, max diff 0.000000 (R8_UNORM, compared r,r=g,r=b,a)
PASS A8L8           256 texels, max diff 0.000000 (R8G8_UNORM, compared r,r=g,r=b,g=a)
PASS V8U8           256 texels, max diff 0.000000 (R8G8_SNORM, compared r,g,a)
PASS X8L8V8U8       256 texels, max diff 0.000000 (Undefined, compared r,g,b,a)
PASS Q8W8V8U8       256 texels, max diff 0.000000 (R8G8B8A8_SNORM, compared r,g,b,a)
PASS V16U16         256 texels, max diff 0.000000 (R16G16_SNORM, compared r,g,a)
PASS L16            256 texels, max diff 0.000000 (R16_UNORM, compared r,r=g,r=b,a)
PASS DXT1           256 texels, max diff 0.019608 (BC1_UNORM, compared r,g,b,a)
FAIL DXT2           79 of 256 texels differ by more than 0.02, max diff 0.850980 (BC2_UNORM, compared r,g,b,a)
       first at 0,0: decoded (0.788235, 0.388235, 0.772549, 0.733333), GPU (0.733333, 0.509804, 0.709804, 0.733333)
       decoded r = GPU none
       decoded g = GPU none
       decoded b = GPU none
       decoded a = GPU a
FAIL DXT3           51 of 256 texels differ by more than 0.02, max diff 0.937255 (BC2_UNORM, compared r,g,b,a)
       first at 3,0: decoded (0, 0, 0, 0.266667), GPU (0.647059, 0.196078, 0.45098, 0.266667)
       decoded r = GPU none
       decoded g = GPU none
       decoded b = GPU none
       decoded a = GPU a
FAIL DXT4           53 of 256 texels differ by more than 0.02, max diff 0.968627 (BC3_UNORM, compared r,g,b,a)
       first at 2,0: decoded (0.741176, 0.823529, 0.741176, 0.501961), GPU (0.678431, 0.862745, 0.819608, 0.498039)
       decoded r = GPU none
       decoded g = GPU none
       decoded b = GPU none
       decoded a = GPU a
FAIL DXT5           66 of 256 texels differ by more than 0.02, max diff 0.937255 (BC3_UNORM, compared r,g,b,a)
       first at 14,0: decoded (0, 0, 0, 0.94902), GPU (0.54902, 0.937255, 0.235294, 0.94902)
       decoded r = GPU none
       decoded g = GPU none
       decoded b = GPU none
       decoded a = GPU a
FAIL ATI1           156 of 256 texels differ by more than 0.02, max diff 1.000000 (BC4_UNORM, compared r)
       first at 8,0: decoded (0, 0, 0, 1), GPU (0.168627, 0, 0, 1)
       decoded r = GPU none
       decoded g = GPU g,b
       decoded b = GPU g,b
       decoded a = GPU a
FAIL ATI2           141 of 256 texels differ by more than 0.02, max diff 1.000000 (BC5_UNORM, compared r,g)
       first at 0,0: decoded (0.486275, 0.658824, 0, 1), GPU (0.657359, 0.484031, 1, 1)
       decoded r = GPU none
       decoded g = GPU none
       decoded b = GPU none
       decoded a = GPU b,a
PASS R16F           256 texels, max diff 0.000000 (R16_FLOAT, compared r,a)
PASS G16R16F        256 texels, max diff 0.000000 (R16G16_FLOAT, compared r,g,a)
PASS A16B16G16R16F  256 texels, max diff 0.000000 (R16G16B16A16_FLOAT, compared r,g,b,a)
PASS R32F           256 texels, max diff 0.000000 (R32_FLOAT, compared r,a)
PASS G32R32F        256 texels, max diff 0.000000 (R32G32_FLOAT, compared r,g,a)
PASS A32B32G32R32F  256 texels, max diff 0.000000 (R32G32B32A32_FLOAT, compared r,g,b,a)
FAIL replay render target differs from the native one for: ATI1 (1.000000), ATI2 (1.000000)

25 formats passed, 6 failed (DXT2, DXT3, DXT4, DXT5, ATI1, ATI2), 0 not compared
Not supported by the GPU: R8G8B8 (CheckDeviceFormat), R3G3B2 (CheckDeviceFormat), A8R3G3B2 (CheckDeviceFormat), X4R4G4B4 (CheckDeviceFormat), A8B8G8R8 (CheckDeviceFormat), X8B8G8R8 (CheckDeviceFormat), A8P8 (CheckDeviceFormat), P8 (CheckDeviceFormat), A4L4 (CheckDeviceFormat), L6V5U5 (CheckDeviceFormat), A2W10V10U10 (CheckDeviceFormat), CxV8U8 (CheckDeviceFormat), Q16W16V16U16 (CheckDeviceFormat), UYVY (CheckDeviceFormat), YUY2 (CheckDeviceFormat), R8G8_B8G8 (CheckDeviceFormat), G8R8_G8B8 (CheckDeviceFormat)
Replay render target: FAIL
```

- **ATI2's "decoded x = GPU y" lines:** these say "none" for r and g. The swap alone would give "decoded r = GPU g" and "decoded g = GPU r". They don't, because only block 0 of the captured data is real (finding 2), so decoded and GPU values agree only in block 0.
- **ATI1:** for the same reason, decoded r matches the GPU only in its first two blocks.
- **DXT2–5:** these failures are the 3-colour-mode CPU decode (finding 3).

### Decoding the block formats myself

`bc_analyse.py` (output in `bc_analyse_output.txt`) does the following for each block format:
- regenerates the demo's bytes with its xorshift generator;
- compares them with `GetTextureData` on the captured texture;
- decodes them to the D3D spec;
- compares that decode with the native GPU, RenderDoc's PickPixel and the replay render target, per channel.

The maximum difference per channel (r, g, b, a):

| Format | Captured bytes | Spec decode vs native GPU | Spec decode vs PickPixel | Replay RT vs native |
|---|---|---|---|---|
| DXT1 (BC1 rules) | identical | 0.003, 0.019, 0.003, 0 | 0.003, 0.003, 0.003, 0 | 0 |
| DXT2, 4-colour | identical | 0.003, 0.016, 0.003, 0 | 0.85, 0.77, 0.84, 0 | 0 |
| DXT2, BC1 mode switch | | 0.85, 0.77, 0.84, 0 | 0.003, 0.003, 0.003, 0 | |
| DXT3, 4-colour | identical | 0.003, 0.021, 0.003, 0 | 0.74, 0.94, 0.75, 0 | 0 |
| DXT4, 4-colour | identical | 0.003, 0.013, 0.003, 0.012 | 0.82, 0.79, 0.97, 0.002 | 0 |
| DXT5, 4-colour | identical | 0.003, 0.018, 0.003, 0.013 | 0.70, 0.94, 0.62, 0.002 | 0 |
| ATI1 | **zero after byte 16 of each row of blocks** | see finding 2 | | **1.0**, 0, 0, 0 |
| ATI2 | **zero after byte 16 of each row of blocks** | see finding 2 | | **1.0, 1.0**, 0, 0 |

DXT3, DXT4 and DXT5 under BC1 mode switching match PickPixel within 0.0033, as DXT2 does. The full table is in the output file.

The captured ATI2 bytes against the app's memory after the demo's writes, per 16 bytes:

```
    0 got a349c44941a3fceda6a8ed2a05d116ff  app a349c44941a3fceda6a8ed2a05d116ff
   16 got 00000000000000000000000000000000  app 58bce052c825544514fcbf1bac97f525
   ...
   64 got 58bce052c825544514fcbf1bac97f525  (app bytes 16..31)
  128 got 4011abbabc8b4a5ef04d2d3db989ae2d  (app bytes 32..47)
  192 got 88e6822317d0c83fba7e968c0ccfaf03  (app bytes 48..63)
  everything else 0
```

So each 64-byte row of blocks in the capture holds 16 bytes (= Pitch) from offset `row * Pitch` of the app's memory, then zeros. ATI1 shows the same pattern with 32-byte rows.

The native Pitch probe (`pitch.cpp`, managed pool, level 0):

```
DXT1 16x16: Pitch 32      DXT1 64x64: Pitch 128
DXT5 16x16: Pitch 64      DXT5 64x64: Pitch 256
ATI1 16x16: Pitch 16      ATI1 64x64: Pitch 64
ATI2 16x16: Pitch 16      ATI2 64x64: Pitch 64
```

## 3. ATI2 in qrenderdoc

- **Opening the texture:** I selected the "ATI2 at 2,42" draw (event 134) and opened its input, Texture 43, in the texture viewer. The status bar shows `Texture 43 - 16x16 1 mips - BC5_UNORM`.
- **The texture's own pick at texel 0,0:** **0.48627, 0.65882** (blue 0, alpha 1 from PickPixel). Crop: `zoom_ati2_tex_statusbar.png`. The view (`zoom_ati2_texture.png`) shows colour only in the first column of blocks, and black where the capture has zeros.
- **The render target at the same texel:** I selected the last format draw ("A32B32G32R32F at 122,42", event 158) and opened Render Target Texture 51 (R32G32B32A32_FLOAT). Picking 2,42 (the ATI2 region's x,y) shows **0.65736, 0.48403, 1.00, 1.00**. Crops: `zoom_rt_statusbar.png` and `zoom_rt.png`. The native reference has exactly 0.657359, 0.484031, 1, 1 there.

So the GPU's red is RenderDoc's green and the GPU's green is RenderDoc's red. The GPU also fills blue with 1, where RenderDoc decodes 0.

- **Missing data in the render target:** away from block 0 the replay render target shows the zeros. For example, at 10,42 (texel 8,0) the replay gives 0, 0, 1, 1, where native has 0.583, 0.199, 1, 1. In `zoom_rt.png` the ATI2 cell is blue apart from its first column of blocks.

## 4. D3D11_D3DPERF_Markers

### The "D3DPERF returns:" line

| Run | Line |
|---|---|
| Native (`--frames 10`) | `D3DPERF returns: BeginEvent -1, nested BeginEvent -1, nested EndEvent -1, EndEvent -1` |
| Under RenderDoc (the captured run) | `D3DPERF returns: BeginEvent 0, nested BeginEvent 0, nested EndEvent -1, EndEvent -1` |

- **Under RenderDoc:** the D3D11 hook returns `WrappedID3D11Device::BeginEvent` / `EndEvent`, which comes from the immediate context's `ThreadSafe_BeginEvent` / `ThreadSafe_EndEvent`. The nested BeginEvent returns 0, not 1.
- **Log quirk:** the line wasn't in the log when I killed the demo while it was running, only when it exited by itself with `--frames`.

### The capture and `d3d11_d3dperf_check.py`

The capture is `D3D11_D3DPERF_Markers_frame23507.rdc`, made with ExecuteAndInject and TriggerCapture as before.

The script as committed:

```
pipelineType GraphicsAPI.D3D11
Traceback (most recent call last):
  ...
  File "E:\Projects\renderdoc\util\test\d3d11_d3dperf_check.py", line 79, in main
    got = tree(controller.GetRootActions(), controller.GetStructuredFile(), colours)
  File "E:\Projects\renderdoc\util\test\d3d11_d3dperf_check.py", line 43, in tree
    elif a.flags & (rd.ActionFlags.PushPopMarker | rd.ActionFlags.SetMarker) or a.children:
  File "enum.py", line 324, in __getattr__
AttributeError: PushPopMarker
```

The full output with `PushPopMarker` patched to `PushMarker`:

```
pipelineType GraphicsAPI.D3D11
Markers and draws (clears and Present left out):
  D3DPERF region
    Draw
    D3DPERF marker
    D3DPERF nested region
      Draw
  Draw
  Annotation region
    Draw
Colour of D3DPERF region: (1, 0, 0) (the demo set (1.0, 0.0, 0.0))
Colour of D3DPERF marker: (0, 1, 0) (the demo set (0.0, 1.0, 0.0))
Colour of D3DPERF nested region: (0, 0, 1) (the demo set (0.0, 0.0, 1.0))

D3DPERF markers in the D3D11 capture: PASS
```

### The Event Browser

The Event Browser shows the same nesting. Crop: `zoom_perf_event_browser.png`.
- "D3DPERF region" (9-14) holds Draw(3) at 10, "D3DPERF marker" at 11, and "D3DPERF nested region" (12-13) with Draw(3) at 13.
- Draw(3) at 16 is outside any region.
- "Annotation region" (17-18) holds Draw(3) at 18.
- **Colours:** the region row is red, the marker row green and the nested region row blue. The Annotation region has the default colour, since the demo sets none.
- **Timeline:** shows the red "D3DPERF region" bar.

## 5. D3D11_Refcount_Check with `run_tests.py`

**PASS**:

```
>> Test D3D11_Refcount_Check
    .. Running exe:'E:\Projects\renderdoc\util\test\demos_x64.exe' cmd:'D3D11_Refcount_Check --log ...\demos.log --tmp ...' in dir:'' with env:'[]'
E:\Projects\renderdoc\util\test\demos\main.cpp:690 Log: Running 'D3D11_Refcount_Check'
    .. Queuing capture of frame 5..6 with timeout of default
E:\Projects\renderdoc\util\test\demos\d3d11\d3d11_test.cpp:319 Log: Running D3D11 test on NVIDIA GeForce RTX 4070 Ti
E:\Projects\renderdoc\util\test\demos\d3d11\d3d11_test.cpp:319 Log: Running D3D11 test on NVIDIA GeForce RTX 4070 Ti
    .. Retrieved 1 captures
    .. Loading capture
    .. Checking capture
    ** Picked value at 199,149 in Swapchain Image 237 is as expected
    ** Captured loaded with color as expected
    .. Test D3D11_Refcount_Check ran in 0:00:02.811040
<< Test D3D11_Refcount_Check
// total=191 fail=0 skip=190 time=5

## Tests complete summary: 1 passed out of 1 run from 191 total in 0:00:05.742161 ##
```

I couldn't run the command exactly as given, for two reasons:

- **Python version:** `run_tests.py` needs Python 3.7 or later (`from __future__ import annotations` in rdtest). The build's `pymodules\renderdoc.pyd` is built against the bundled Python 3.6, and the only Python installed here is 3.14.
  - I built `pyrenderdoc_module.vcxproj` alone against 3.14 using upstream's `VSPythonOverridePath`, with a separate obj folder, and copied that `renderdoc.pyd` out.
  - I then restored the 3.6 pymodules, so the build is unchanged.
  - `--pyrenderdoc` pointed at the 3.14 module, and `--renderdoc` at `x64\Development`.
- **Vulkan layer:** the runner then stopped at "Couldn't register vulkan layer properly, might need admin rights".
  - The cause: an installed RenderDoc in Program Files owns the system Vulkan layer registration, and replacing it needs a UAC prompt and a machine-wide change.
  - So I started `run_tests.py` through a small wrapper that makes `rd.NeedVulkanLayerRegistration` return False and then runs `run_tests.py` unchanged. The test itself runs in a `run_tests.py --internal_run_test` subprocess, which doesn't check the layer. A D3D11-only test doesn't use the Vulkan layer.

## Files in `d3d9_formats_d3dperf_test\`

- `D3D9_Texture_Formats_reference.txt` (native) and `D3D9_Texture_Formats_reference_rdoc.txt` (under RenderDoc), byte-identical.
- `native_D3D9_Texture_Formats.png`: the native window.
- `zoom_ati2_texture.png`, `zoom_ati2_tex_statusbar.png`: Texture 43 (ATI2) and its pick at 0,0.
- `zoom_rt.png`, `zoom_rt_statusbar.png`: the render target at event 158 and its pick at 2,42.
- `zoom_perf_event_browser.png`: the D3D11 capture's Event Browser and timeline.
- `bc_analyse.py`, `bc_analyse_output.txt`: the block-format decode comparison, run inside qrenderdoc's Python.
- `pitch.cpp`: the native LockRect Pitch probe.
