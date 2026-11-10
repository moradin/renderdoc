# D3D9 step 8: saving DXT textures as PNG

Tested on the working tree on top of c5b9fa83f, which was committed as "D3D9: save block
compressed textures as PNG". Machine: Windows 11, RTX 4070 Ti. Files are in `d3d9_step8_dxt_png/`.

## What changed

Saving a texture as PNG (or JPG, BMP, TGA, HDR, EXR) asks the driver's `GetTextureData` to convert
it to RGBA. D3D9's conversion skipped block compressed formats (DXT1-5, ATI1, ATI2) and the formats
RenderDoc can't describe (X8L8V8U8, L6V5U5, R3G3B2 and others). It logged "isn't supported yet" and
returned the raw data. The replay controller then wrote the compressed bytes as if they were RGBA
pixels, together with whatever memory came after them, which is a bigger buffer than the data. The
save didn't fail: it wrote a garbage PNG.

These formats are now converted from the texels that picking decodes (`GetDecoded` in
`d3d9_pixel.cpp`). Earlier rounds checked those texels against what the GPU samples, and fixed the
DXT2-5 palette and ATI2 channel order there. A volume's mip is converted a depth slice at a time and
returned whole, as for the raw data, so the replay controller can pick the slice. YUV formats take
the same path and are saved as (Cr, Y, Cb, 1), as picked. The per-texel remap (black and white
points, clamping, the output format) is now one helper that both paths use.

Saving as DDS is unchanged: block compressed data is saved as it is.

## The tests

- **`d3d9_formats_check.py`** (run by `D3D9_Texture_Formats`) now also saves each format's 16x16
  texture as a PNG with `SaveTexture`, as File > Save Texture does. It reads the PNG back with a
  small decoder (standard library only, so the script still runs in qrenderdoc's Python 3.6) and
  compares every texel with the native run's reference. The reference is what the GPU sampled from
  each format, natively, with point sampling. The same channels are compared as for picking, with
  the same tolerance plus one 1/255 step, since the PNG is 8-bit and clamped to 0-1.
- **`D3D9_Resource_Contents`** has a new DXT1 cube texture and DXT1 volume texture (4x4, 4x4x4).
  Each face and slice is one block with both endpoints set to the same R5G6B5 colour, which every
  decoder and GPU decodes exactly. The test saves each face and each depth slice as a PNG and checks
  every texel against the colour expanded to 8 bits. This covers the cube face and volume slice
  selection, which the formats demo's 2D textures don't. The two new textures moved the recycled
  texture's name from Texture 42 to Texture 44.

## Results

### Automated tests

All 16 passed, x64 and x86:

```
## Tests complete summary: 16 passed out of 16 run from 207 total in 0:01:14.289918 ##
## Tests complete summary: 16 passed out of 16 run from 207 total in 0:01:17.692215 ##
```

The first run failed two tests, both because of the new checks:
- `X8L8V8U8` failed the PNG save check: it's one of the Undefined formats, which had the same
  problem as the block formats. That's why the change covers them too.
- `D3D9_Resource_Contents` looked for "Texture 42", which the new textures renamed.

### Other checks

- **Unit tests:** `[d3d9]` passed (6057 assertions in 13 test cases).
- **Builds:** `renderdoc.sln` and `demos.sln` built with 0 warnings and 0 errors, for both x64 and
  x86.

### PNG saves against the GPU

From the formats check (`check_formats_new.txt`). The reference is the GPU's, natively; the PNG is
RenderDoc's:

```
PASS X8L8V8U8       PNG save: 256 texels, max diff 0.001945
PASS DXT1           PNG save: 256 texels, max diff 0.019608
PASS DXT2           PNG save: 256 texels, max diff 0.015686
PASS DXT3           PNG save: 256 texels, max diff 0.019608
PASS DXT4           PNG save: 256 texels, max diff 0.011765
PASS DXT5           PNG save: 256 texels, max diff 0.019608
PASS ATI1           PNG save: 256 texels, max diff 0.007874
PASS ATI2           PNG save: 256 texels, max diff 0.024857
...
PNG save: 31 passed, 0 failed
```

The block formats' differences are the decoder's rounding against NVIDIA's (up to 0.024, as the
picking check already allows). The other 23 formats, which were already converted, passed too.

The DXT1 cube faces and volume slices, from `dxt_saves_new.txt`: every texel of each PNG is its
expected colour.

```
Cube Texture 36 slice 0: 4x4 PNG, colours [(41, 255, 0, 255)]
...
Cube Texture 36 slice 5: 4x4 PNG, colours [(247, 93, 123, 255)]
Volume Texture 37 slice 0: 4x4 PNG, colours [(0, 40, 255, 255)]
...
Volume Texture 37 slice 3: 4x4 PNG, colours [(0, 162, 107, 255)]
```

### The same captures with the old code

To show the new checks catch the old behaviour, I put back `d3d9_replay.cpp` from c5b9fa83f,
rebuilt, and ran the checks on the same two captures:
- The formats check failed exactly the 8 formats that the change covers: X8L8V8U8, DXT1, DXT2,
  DXT3, DXT4, DXT5, ATI1 and ATI2 (`check_formats_old.txt`). The other 23 passed.
- Every DXT1 cube face and volume slice came out as 10 to 15 different colours instead of one
  (`dxt_saves_old.txt`).

```
PNG save: 23 passed, 8 failed (X8L8V8U8, DXT1, DXT2, DXT3, DXT4, DXT5, ATI1, ATI2)
FAIL DXT5           PNG save: 256 of 256 texels differ by more than 0.0339, max diff 1.000000
       first at 0,0: PNG [46, 115, 208, 32], GPU (0.839216, 0.156863, 0.517647, 0.180392)
```

With the new code rebuilt, the same captures give 31 passed, 0 failed, and one colour per face
and slice.

## UI

I opened the formats capture in qrenderdoc, went to the DXT5 draw (event 126) and opened its
texture (Texture 41, BC3_UNORM) in the texture viewer:
- **`ui_1_dxt5_texture_viewer.png`:** the DXT5 texture in the texture viewer.
- **`ui_2_save_dialog_png.png`:** Save Texture with PNG selected, the default black and white points
  (0 and 1), and the overwrite prompt. The file already existed from an earlier attempt of mine.
- **`ui_3_saved_DXT5.png`:** the file it saved. Its texel 0,0 is (214, 40, 132, 46), the same as
  the GPU's sample. All 256 texels are within 5/255 of the GPU's samples.

The debug message count was 0.

When I first opened Texture 41 from a script, its tab stayed blank until I switched to another
tab and back. The hover readout showed the right values while it was blank, so it's the view not
being painted rather than the decode. I didn't look into it further.

`saved_png/` has the PNGs from both checks: each block format's texture, and each DXT1 cube face
and volume slice.

## Not covered

- **YUV formats:** YUY2 and UYVY take the same path, but the RTX 4070 Ti doesn't create these
  textures, so they weren't saved. Other Undefined formats (L6V5U5, R3G3B2, A8R3G3B2, the RGBG
  formats) aren't supported by this GPU either; only X8L8V8U8 was tested.
- **Mips:** only mip 0 was saved. Smaller mips use the same decode, which pick pixel uses at every
  mip.
- **Real games:** not tried with a real game's textures.
- **Other GPUs:** only the RTX 4070 Ti.
