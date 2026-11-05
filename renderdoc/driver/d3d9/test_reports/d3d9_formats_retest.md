# D3D9 texture formats retest on 20fb8d025

Everything passes. There is one small leftover: RenderDoc's ATI2 pick gives blue **0**, but the GPU samples blue as **1**. The check script only compares red and green for ATI2, so it doesn't catch this.

- **What I built:** 20fb8d025, `renderdoc.sln` and `util\test\demos.sln`, Development x64, with 0 errors and 0 warnings.
- **Machine:** the same as before (Windows 11, RTX 4070 Ti).
- **Check scripts:** the committed ones, with no local patches. As in earlier rounds, they ran inside qrenderdoc's embedded Python.
- **Files:** in `d3d9_formats_retest\`.

| # | Item | Result |
|---|---|---|
| 1 | D3D9_Texture_Formats natively, then captured, then `d3d9_formats_check.py` | **PASS**: 31 of 31 formats, and the replay render target passes. The `_rdoc` reference is byte-identical to the new native one. |
| 2 | `bc_analyse.py` on the new capture | Every block format's captured bytes match the generator. PickPixel matches the spec decode within 0.0034. |
| 3 | qrenderdoc: ATI2 texel 0,0 against the render target | **Red and green agree.** ATI1 and ATI2 are filled in every 4×4 block. |
| 4 | `d3d11_d3dperf_check.py` on last round's capture | **PASS** |

## 1. Native run, capture and `d3d9_formats_check.py`

### Native run

- **Demo log:** the same 17 "Not supported:" lines as last round, then "31 formats supported, 17 not" and "Wrote the reference to E:\Projects\renderdoc\util\test\D3D9_Texture_Formats_reference.txt". There is no TEST_WARN or TEST_ERROR.
  - The unsupported formats are R8G8B8, R3G3B2, A8R3G3B2, X4R4G4B4, A8B8G8R8, X8B8G8R8, A8P8, P8, A4L4, L6V5U5, A2W10V10U10, CxV8U8, Q16W16V16U16, UYVY, YUY2, R8G8_B8G8 and G8R8_G8B8.
- **Screenshot:** `native_D3D9_Texture_Formats.png`.
- **What changed in the native reference:** compared with last round's, only the ATI1 and ATI2 sections differ. The other 29 formats are identical.

### Capture

- **How:** ExecuteAndInject and TriggerCapture, as before. The capture is `D3D9_Texture_Formats_frame14903.rdc`.
- **Demo log under RenderDoc:** the same lines, except that the reference goes to `D3D9_Texture_Formats_reference_rdoc.txt`.
- **`D3D9_Texture_Formats_reference_rdoc.txt` against the new native reference:** **byte-identical**. Both are 293,903 bytes, with SHA-256 `A3D7FA67A631210CC43292884730393A05FCA759D8DC49C0CE789C72469E342F`. Last round they differed in ATI1 and ATI2.
- **In the folder:** the native reference as `D3D9_Texture_Formats_reference.txt`.

### `d3d9_formats_check.py`

I ran `d3d9_formats_check.py <capture.rdc> E:\Projects\renderdoc\util\test\D3D9_Texture_Formats_reference.txt`. It exited 0. The full output (also in `d3d9_formats_check_output.txt`):

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
PASS DXT2           256 texels, max diff 0.015686 (BC2_UNORM, compared r,g,b,a)
PASS DXT3           256 texels, max diff 0.019608 (BC2_UNORM, compared r,g,b,a)
PASS DXT4           256 texels, max diff 0.011765 (BC3_UNORM, compared r,g,b,a)
PASS DXT5           256 texels, max diff 0.019608 (BC3_UNORM, compared r,g,b,a)
PASS ATI1           256 texels, max diff 0.007874 (BC4_UNORM, compared r)
PASS ATI2           256 texels, max diff 0.024857 (BC5_UNORM, compared r,g)
PASS R16F           256 texels, max diff 0.000000 (R16_FLOAT, compared r,a)
PASS G16R16F        256 texels, max diff 0.000000 (R16G16_FLOAT, compared r,g,a)
PASS A16B16G16R16F  256 texels, max diff 0.000000 (R16G16B16A16_FLOAT, compared r,g,b,a)
PASS R32F           256 texels, max diff 0.000000 (R32_FLOAT, compared r,a)
PASS G32R32F        256 texels, max diff 0.000000 (R32G32_FLOAT, compared r,g,a)
PASS A32B32G32R32F  256 texels, max diff 0.000000 (R32G32B32A32_FLOAT, compared r,g,b,a)
PASS replay render target matches the native one for all 31 formats (max diff 5e-09)

31 formats passed, 0 failed, 0 not compared
Not supported by the GPU: R8G8B8 (CheckDeviceFormat), R3G3B2 (CheckDeviceFormat), A8R3G3B2 (CheckDeviceFormat), X4R4G4B4 (CheckDeviceFormat), A8B8G8R8 (CheckDeviceFormat), X8B8G8R8 (CheckDeviceFormat), A8P8 (CheckDeviceFormat), P8 (CheckDeviceFormat), A4L4 (CheckDeviceFormat), L6V5U5 (CheckDeviceFormat), A2W10V10U10 (CheckDeviceFormat), CxV8U8 (CheckDeviceFormat), Q16W16V16U16 (CheckDeviceFormat), UYVY (CheckDeviceFormat), YUY2 (CheckDeviceFormat), R8G8_B8G8 (CheckDeviceFormat), G8R8_G8B8 (CheckDeviceFormat)
Replay render target: PASS
```

## 2. `bc_analyse.py`

### Changes to the script

- **The generator:** it already produced one continuous run of 16 × block-size bytes. That is the tightly packed layout the demo now writes, with `pitch = max(Pitch, 4 blocks)`. So the generator itself needed no change. Last round it matched what the demo meant to write, not what the texture held: the demo then wrote each 32- or 64-byte row at the reported Pitch of 16, so the rows overlapped.
- **ATI2:** `GetTextureData` now describes ATI2 as BC5 and swaps each block's 8-byte halves. So the script compares ATI2's bytes with the generator's halves swapped. It still decodes the spec from the unswapped D3D9 bytes.
- **In the folder:** the updated `bc_analyse.py` and its full output, `bc_analyse_output.txt`.

### ATI1, ATI2 and DXT2–5 sections

```
== DXT2 event 114 (BC2_UNORM), 256 bytes from GetTextureData, generator 256 bytes: identical
   spec decode [four] vs native GPU  max diff per channel (0.0027, 0.0158, 0.0027, 0.0000)
   spec decode [four] vs PickPixel   max diff per channel (0.0033, 0.0032, 0.0028, 0.0000)
   spec decode [four] vs replay RT   max diff per channel (0.0027, 0.0158, 0.0027, 0.0000)
   spec decode [bc1] vs native GPU  max diff per channel (0.8510, 0.7725, 0.8392, 0.0000)
   spec decode [bc1] vs PickPixel   max diff per channel (0.8510, 0.7686, 0.8392, 0.0000)
   spec decode [bc1] vs replay RT   max diff per channel (0.8510, 0.7725, 0.8392, 0.0000)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.7333, 0.5098, 0.7098, 0.7333) replay (0.7333, 0.5098, 0.7098, 0.7333) pick (0.7294, 0.4980, 0.7098, 0.7333)
   texel 8,0: native (0.0314, 0.3333, 0.2588, 0.0667) replay (0.0314, 0.3333, 0.2588, 0.0667) pick (0.0314, 0.3333, 0.2588, 0.0667)
   texel 5,9: native (0.9059, 0.6353, 0.6118, 0.1333) replay (0.9059, 0.6353, 0.6118, 0.1333) pick (0.9059, 0.6353, 0.6118, 0.1333)

== DXT3 event 118 (BC2_UNORM), 256 bytes from GetTextureData, generator 256 bytes: identical
   spec decode [four] vs native GPU  max diff per channel (0.0027, 0.0212, 0.0027, 0.0000)
   spec decode [four] vs PickPixel   max diff per channel (0.0028, 0.0031, 0.0030, 0.0000)
   spec decode [four] vs replay RT   max diff per channel (0.0027, 0.0212, 0.0027, 0.0000)
   spec decode [bc1] vs native GPU  max diff per channel (0.7412, 0.9373, 0.7529, 0.0000)
   spec decode [bc1] vs PickPixel   max diff per channel (0.7412, 0.9373, 0.7529, 0.0000)
   spec decode [bc1] vs replay RT   max diff per channel (0.7412, 0.9373, 0.7529, 0.0000)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.5176, 0.3490, 0.3216, 0.6667) replay (0.5176, 0.3490, 0.3216, 0.6667) pick (0.5176, 0.3490, 0.3216, 0.6667)
   texel 8,0: native (0.5804, 0.2941, 0.4196, 0.5333) replay (0.5804, 0.2941, 0.4196, 0.5333) pick (0.5804, 0.3137, 0.4196, 0.5333)
   texel 5,9: native (0.4196, 0.0000, 0.6784, 0.0667) replay (0.4196, 0.0000, 0.6784, 0.0667) pick (0.4196, 0.0000, 0.6784, 0.0667)

== DXT4 event 122 (BC3_UNORM), 256 bytes from GetTextureData, generator 256 bytes: identical
   spec decode [four] vs native GPU  max diff per channel (0.0027, 0.0131, 0.0028, 0.0118)
   spec decode [four] vs PickPixel   max diff per channel (0.0028, 0.0028, 0.0034, 0.0017)
   spec decode [four] vs replay RT   max diff per channel (0.0027, 0.0131, 0.0028, 0.0118)
   spec decode [bc1] vs native GPU  max diff per channel (0.8196, 0.7882, 0.9686, 0.0118)
   spec decode [bc1] vs PickPixel   max diff per channel (0.8196, 0.7922, 0.9686, 0.0017)
   spec decode [bc1] vs replay RT   max diff per channel (0.8196, 0.7882, 0.9686, 0.0118)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.5490, 0.9216, 0.9686, 0.6588) replay (0.5490, 0.9216, 0.9686, 0.6588) pick (0.5490, 0.9216, 0.9686, 0.6588)
   texel 8,0: native (0.3529, 0.1882, 0.0941, 0.1765) replay (0.3529, 0.1882, 0.0941, 0.1765) pick (0.3529, 0.1882, 0.0941, 0.1765)
   texel 5,9: native (0.5608, 0.4980, 0.7216, 0.0000) replay (0.5608, 0.4980, 0.7216, 0.0000) pick (0.5608, 0.4863, 0.7216, 0.0000)

== DXT5 event 126 (BC3_UNORM), 256 bytes from GetTextureData, generator 256 bytes: identical
   spec decode [four] vs native GPU  max diff per channel (0.0027, 0.0184, 0.0029, 0.0125)
   spec decode [four] vs PickPixel   max diff per channel (0.0027, 0.0029, 0.0028, 0.0017)
   spec decode [four] vs replay RT   max diff per channel (0.0027, 0.0184, 0.0029, 0.0125)
   spec decode [bc1] vs native GPU  max diff per channel (0.6980, 0.9373, 0.6235, 0.0125)
   spec decode [bc1] vs PickPixel   max diff per channel (0.6980, 0.9373, 0.6235, 0.0017)
   spec decode [bc1] vs replay RT   max diff per channel (0.6980, 0.9373, 0.6235, 0.0125)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.8392, 0.1569, 0.5176, 0.1804) replay (0.8392, 0.1569, 0.5176, 0.1804) pick (0.8392, 0.1569, 0.5176, 0.1804)
   texel 8,0: native (0.3529, 0.0000, 0.0627, 0.6784) replay (0.3529, 0.0000, 0.0627, 0.6784) pick (0.3529, 0.0000, 0.0627, 0.6824)
   texel 5,9: native (0.6980, 0.1059, 0.4314, 0.7059) replay (0.6980, 0.1059, 0.4314, 0.7059) pick (0.6980, 0.1059, 0.4275, 0.7020)

== ATI1 event 130 (BC4_UNORM), 128 bytes from GetTextureData, generator 128 bytes: identical
   spec decode [-] vs native GPU  max diff per channel (0.0094, 0.0000, 0.0000, 0.0000)
   spec decode [-] vs PickPixel   max diff per channel (0.0017, 0.0000, 0.0000, 0.0000)
   spec decode [-] vs replay RT   max diff per channel (0.0094, 0.0000, 0.0000, 0.0000)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.4638, 0.0000, 0.0000, 1.0000) replay (0.4638, 0.0000, 0.0000, 1.0000) pick (0.4627, 0.0000, 0.0000, 1.0000)
   texel 8,0: native (0.3172, 0.0000, 0.0000, 1.0000) replay (0.3172, 0.0000, 0.0000, 1.0000) pick (0.3176, 0.0000, 0.0000, 1.0000)
   texel 5,9: native (0.1384, 0.0000, 0.0000, 1.0000) replay (0.1384, 0.0000, 0.0000, 1.0000) pick (0.1333, 0.0000, 0.0000, 1.0000)

== ATI2 event 134 (BC5_UNORM), 256 bytes from GetTextureData, generator 256 bytes: identical (to the generator with each block's halves swapped; unswapped: different)
   spec decode [xy] vs native GPU  max diff per channel (1.0000, 1.0000, 0.0000, 0.0000)
   spec decode [xy] vs PickPixel   max diff per channel (1.0000, 1.0000, 1.0000, 0.0000)
   spec decode [xy] vs replay RT   max diff per channel (1.0000, 1.0000, 0.0000, 0.0000)
   spec decode [yx] vs native GPU  max diff per channel (0.0241, 0.0185, 0.0000, 0.0000)
   spec decode [yx] vs PickPixel   max diff per channel (0.0017, 0.0017, 1.0000, 0.0000)
   spec decode [yx] vs replay RT   max diff per channel (0.0241, 0.0185, 0.0000, 0.0000)
   replay RT vs native GPU          max diff per channel (0.0000, 0.0000, 0.0000, 0.0000)
   texel 0,0: native (0.6574, 0.4840, 1.0000, 1.0000) replay (0.6574, 0.4840, 1.0000, 1.0000) pick (0.6588, 0.4863, 0.0000, 1.0000)
   texel 8,0: native (0.2481, 0.2039, 1.0000, 1.0000) replay (0.2481, 0.2039, 1.0000, 1.0000) pick (0.2510, 0.2039, 0.0000, 1.0000)
   texel 5,9: native (0.3016, 1.0000, 1.0000, 1.0000) replay (0.3016, 1.0000, 1.0000, 1.0000) pick (0.3020, 1.0000, 0.0000, 1.0000)
```

### What it shows

- **ATI1 and ATI2 bytes:** the capture now keeps all of them. Last round only 16 bytes of each row of blocks survived.
- **ATI2 channel order:** the D3D9 decode with red from the second half (`[yx]`) matches the native GPU within 0.024 and PickPixel within 0.0017.
- **DXT2–5:** PickPixel now matches the 4-colour decode within 0.0034 on r, g and b. Last round it followed BC1's 3-colour switching.
- **Replay against native:** the replay render target equals the native GPU exactly for all six formats.
- **The remaining spec-vs-GPU gaps are the GPU's own rounding, not RenderDoc:** about 0.02 in DXT green and 0.024 in ATI2's red and green.
- **The ATI2 blue leftover:** PickPixel returns blue **0** for ATI2 because it decodes as BC5. The GPU, and the replay, sample D3D9 ATI2 with blue **1**. The check compares only r and g, so it passes. It shows up as blue 0 in the texture viewer's pick, while the render target (and so the "real" sampled value) has blue 1.

## 3. ATI2 in qrenderdoc

I drove the UI with my usual script and opened each draw's input in the texture viewer.

| What | Event | Pick | Crops |
|---|---|---|---|
| **ATI2**, Texture 43 (`16x16 1 mips - BC5_UNORM`), texel 0,0 | 134 | **0.65882, 0.48627** (PickPixel: blue 0, alpha 1) | `zoom_ati2_tex.png`, `zoom_ati2_tex_statusbar.png` |
| **The render target**, Render Target Texture 51 (R32G32B32A32_FLOAT), at 2,42 (the ATI2 region's x,y), at the last format draw "A32B32G32R32F at 122,42" | 158 | **0.65736, 0.48403, 1.00, 1.00** | `zoom_rt.png`, `zoom_rt_statusbar.png` |

- **Red and green now agree:** within 0.0015 and 0.0022. Last round the texture pick was 0.48627, 0.65882, the other way round. The render target's value is unchanged and equals the native reference (0.657359, 0.484031, 1, 1).
- **Blue differs:** the texture pick gives 0 and the render target gives 1, as noted in section 2.
- **ATI1:** Texture 42 (`BC4_UNORM`, event 130) picks **0.46275** at 0,0. The native value is 0.4638. Crops: `zoom_ati1_tex.png` and `zoom_ati1_tex_statusbar.png`.
- **Every 4×4 block is filled** in both views. Last round only the first column of blocks had colour and the rest was black.
  - ATI2 (`zoom_ati2_tex.png`) shows red and green blocks.
  - ATI1 (`zoom_ati1_tex.png`) shows red blocks. Some are dark or near-black, but those are real data: the native GPU returns the same.

## 4. `d3d11_d3dperf_check.py`

I ran the committed script on last round's `D3D11_D3DPERF_Markers_frame23507.rdc`. It exited 0, and its output is identical to last round's patched run apart from the log path:

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

## Files in `d3d9_formats_retest\`

| File | What it is |
|---|---|
| `native_D3D9_Texture_Formats.png` | The native run |
| `D3D9_Texture_Formats_reference.txt` | The native reference (the `_rdoc` one is byte-identical) |
| `d3d9_formats_check_output.txt` | The check script's full output |
| `bc_analyse.py`, `bc_analyse_output.txt` | The updated analysis script and its full output |
| `d3d11_d3dperf_check_output.txt` | The D3DPERF check's output |
| `zoom_ati2_tex.png`, `zoom_ati2_tex_statusbar.png` | ATI2 in the texture viewer, and its pick at 0,0 |
| `zoom_ati1_tex.png`, `zoom_ati1_tex_statusbar.png` | ATI1 in the texture viewer, and its pick at 0,0 |
| `zoom_rt.png`, `zoom_rt_statusbar.png` | The render target at event 158, and its pick at 2,42 |

## ff8a6dd08: ATI2 blue

I pulled ff8a6dd08 and rebuilt `renderdoc.sln`, Development x64, with exit code 0. I didn't rebuild the demos, since they are unchanged.

| # | Item | Result |
|---|---|---|
| 1 | `d3d9_formats_check.py` on `D3D9_Texture_Formats_frame14903.rdc` with `d3d9_formats_retest\D3D9_Texture_Formats_reference.txt` | **PASS**: 31 of 31 formats, and the replay render target passes. The ATI2 line now reads "compared r,g,b". |
| 2 | qrenderdoc: ATI2 (Texture 43, event 134) at texel 0,0 | **0.658824, 0.486275, 1.000000, 1.000000**: blue is now 1, as expected |

### `d3d9_formats_check.py`

The script is the committed one, with no local patches. It exited 0. The full output (also in `d3d9_formats_check_output_ff8a6dd08.txt`):

```
Reference E:\Projects\renderdoc\renderdoc\driver\d3d9\test_reports\d3d9_formats_retest\D3D9_Texture_Formats_reference.txt: 31 formats
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
PASS DXT2           256 texels, max diff 0.015686 (BC2_UNORM, compared r,g,b,a)
PASS DXT3           256 texels, max diff 0.019608 (BC2_UNORM, compared r,g,b,a)
PASS DXT4           256 texels, max diff 0.011765 (BC3_UNORM, compared r,g,b,a)
PASS DXT5           256 texels, max diff 0.019608 (BC3_UNORM, compared r,g,b,a)
PASS ATI1           256 texels, max diff 0.007874 (BC4_UNORM, compared r)
PASS ATI2           256 texels, max diff 0.024857 (BC5_UNORM, compared r,g,b)
PASS R16F           256 texels, max diff 0.000000 (R16_FLOAT, compared r,a)
PASS G16R16F        256 texels, max diff 0.000000 (R16G16_FLOAT, compared r,g,a)
PASS A16B16G16R16F  256 texels, max diff 0.000000 (R16G16B16A16_FLOAT, compared r,g,b,a)
PASS R32F           256 texels, max diff 0.000000 (R32_FLOAT, compared r,a)
PASS G32R32F        256 texels, max diff 0.000000 (R32G32_FLOAT, compared r,g,a)
PASS A32B32G32R32F  256 texels, max diff 0.000000 (R32G32B32A32_FLOAT, compared r,g,b,a)
PASS replay render target matches the native one for all 31 formats (max diff 5e-09)

31 formats passed, 0 failed, 0 not compared
Not supported by the GPU: R8G8B8 (CheckDeviceFormat), R3G3B2 (CheckDeviceFormat), A8R3G3B2 (CheckDeviceFormat), X4R4G4B4 (CheckDeviceFormat), A8B8G8R8 (CheckDeviceFormat), X8B8G8R8 (CheckDeviceFormat), A8P8 (CheckDeviceFormat), P8 (CheckDeviceFormat), A4L4 (CheckDeviceFormat), L6V5U5 (CheckDeviceFormat), A2W10V10U10 (CheckDeviceFormat), CxV8U8 (CheckDeviceFormat), Q16W16V16U16 (CheckDeviceFormat), UYVY (CheckDeviceFormat), YUY2 (CheckDeviceFormat), R8G8_B8G8 (CheckDeviceFormat), G8R8_G8B8 (CheckDeviceFormat)
Replay render target: PASS

[exit code 0]
log C:\Users\Balazs\AppData\Local\Temp\RenderDoc\RenderDoc_2026.11.05_21.42.23.log
```

Compared with this report's section 1 run, only the ATI2 line changed, apart from the reference path and the log name. It now reads "compared r,g,b" (it was "compared r,g"). Its max diff stays 0.024857, so blue adds no difference: it matches the reference's 1 exactly.

### ATI2 pick in qrenderdoc

I used the same UI driver as in section 3, on the same capture: `event 134`, then I opened the pixel shader's input in the texture viewer and went to 0,0.

- **The texture:** ResourceId 43, "Texture 43", `16x16 1 mips - BC5_UNORM`.
- **PickPixel from the UI's Python, at 0,0:** **0.658824, 0.486275, 1.000000, 1.000000**. Last round it gave blue 0.000000.
- **The texture viewer's status bar:** the right-click pick reads `0, 0: 0.65882, 0.48627`. It only shows two channels for a BC5 texture, so blue doesn't appear there. Crop: `zoom_ati2_tex_statusbar_ff8a6dd08.png`.
- **Against the render target:** the value now agrees with the render target at 2,42 (0.65736, 0.48403, 1.00, 1.00, from section 3) in all four channels. Red and green are within 0.0015 and 0.0022; blue and alpha are equal.

New files in `d3d9_formats_retest\`:

| File | What it is |
|---|---|
| `d3d9_formats_check_output_ff8a6dd08.txt` | The check script's full output on ff8a6dd08 |
| `zoom_ati2_tex_statusbar_ff8a6dd08.png` | ATI2's status bar pick at 0,0 on ff8a6dd08 |
