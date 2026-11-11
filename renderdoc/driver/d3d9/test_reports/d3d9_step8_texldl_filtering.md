# D3D9 step 8: texldl filtering

Tested on the working tree on top of de78c37ab, which was committed as "D3D9: filter texldl as
the GPU does". Machine: Windows 11, RTX 4070 Ti. Files are in `d3d9_step8_texldl_filtering/`.

## What changed

The mesh viewer's VS Out runs vertex shaders on the CPU. Its `texldl` always point sampled the
mip level nearest the LOD, whatever the sampler's filters were, so a vertex shader that reads a
heightmap or a skinning texture with linear filtering got different values than the GPU's.

Now `VSTexture::SampleLevel` (`d3d9_vs_emulate.cpp`) follows `D3DSAMP_MAGFILTER`, `MINFILTER`
and `MIPFILTER`, and the replay (`d3d9_postvs.cpp`) also reads `D3DSAMP_SRGBTEXTURE`. How it
filters is what the GPU was measured to do, below, not the documentation's description. Two
point sampling cases changed with it: cube maps picked the x face when two axes were as long
(the GPU picks z, then y), and `MIPFILTER` NONE used `MAXMIPLEVEL`'s level (the GPU reads level
0).

## What the GPU does

Native first: a probe (`probe/d3d9_texldl_probe.cpp`, not committed as a demo) draws one point
per sample coordinate with a vs_3_0 shader that does `texldl` and passes the result through to a
float render target. It ran 105 configurations on a hardware vertex processing device and on a
software one, 263,794 samples each:
- every filter combination (point, linear, anisotropic for magnification and minification, none,
  point, linear mip filters) on an 8x4 A32B32G32R32F texture with 4 levels;
- each address mode with linear filters, with and without a mip filter, and a border colour;
- `MAXMIPLEVEL` 1, 2, 3 and 7, LOD biases of 0.5, -0.7 and 1.25, and which filter applies at LODs
  around 0, with and without `MAXMIPLEVEL`;
- 2-texel textures with 0 and 1, to read the weights straight off, 1/1024 of a texel apart;
- A16B16G16R16F, R32F and A8R8G8B8, and A8R8G8B8 read as sRGB;
- a 4x4 cube with 3 levels, at directions across face edges and corners, and a 4x4x4 volume with
  3 levels.

The coordinates go across texture edges and the LODs around each level. The raw output is 32 MB,
so it isn't kept; `probe/probe.log` is the run's log, and `model.py` the model below in Python,
with its results per configuration in `model_hw.txt` and `model_sw.txt`.

On the hardware device every format was filterable as a vertex texture (`CheckDeviceFormat` with
`D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER`) and `VertexTextureFilterCaps` was
`0x03030700`: point, linear and anisotropic minification, point and linear magnification and
mip filters. What it did:
- **The LOD** is w plus `MIPMAPLODBIAS`, keeping 8 bits of fraction, rounded down. With LOD
  0.001953125 (half a 256th) the blend between levels 0 and 1 had weight 0; with 0.00390625, 1/256.
- **Which filter:** the LOD is clamped to `MAXMIPLEVEL` and the last level; `MAGFILTER` applies at
  0 or less, `MINFILTER` above. With `MAXMIPLEVEL` 1 every LOD, -1 included, was minification.
  Anisotropic filtered as linear.
- **Which level:** `MIPFILTER` NONE reads level 0, even with `MAXMIPLEVEL` 1 to 7 (though it still
  picks the filter above). POINT takes the nearest level, LINEAR blends the two either side.
- **Linear filtering** blends the texels either side of each axis's coordinate (texel centres at
  half texels), each addressed on its own: `BORDER` blends the border colour in, `WRAP` the far
  edge, `MIRRORONCE` and `MIRROR` the mirrored texel. The weights keep 8 bits, rounded to the
  nearest: across a texel the weight went up in 257 steps of 1/256, 1/512 at most from the exact
  one.
- **Cube maps:** the face is the longest axis's, z, then y, then x when two are as long:
  (1, -1, -1) read -Z and (1, -1, 0.3) read -Y. Each face clamps at its edges; nothing filtered
  across faces.
- **sRGB:** with `SRGBTEXTURE` on an A8R8G8B8 texture, the colour channels are converted before
  filtering.

The model in `model.py` puts all of that together. The GPU doesn't filter exactly as it says: its
weights vary by a 256th or two depending on the other coordinates (a volume texel's x weight was
128/256 or 129/256 for the same x, depending on y and z), and no simple rounding of the combined
weights matched better. So it's checked to 1/128 of each channel's range over the texture: all
105 configurations, every sample within that, the worst 0.0072 (`model_hw.txt`).

Software vertex processing (`VertexTextureFilterCaps` `0x1b031b00`) filters differently: weights
of 5 bits (steps of 1/32), the LOD not rounded down to 256ths (so the filter changes at different
LODs), and different faces at cube ties and edges. 63 of its 105 configurations differ from the
model by more than 1/128 (`model_sw.txt`). RenderDoc emulates the hardware path only.

## The tests

`D3D9_Mesh_Output` has a new draw, "vs_3_0 texldl filtering": an assembled vs_3_0 shader over a
5x5 grid (25 vertices, 96 rows) that samples four vertex textures and writes each result to a
TEXCOORD output. Each sampler's coordinate is a different linear function of the vertex's uv, so
the samples cross texels, mip levels, LOD 0 and cube faces:

| Sampler | Texture | Filters (mag, min, mip) | Other states |
|---|---|---|---|
| s0 | 8x4 A32B32G32R32F, 4 levels | linear, linear, linear | wrap u, mirror v, LOD bias 0.25 |
| s1 | 4x4 A32B32G32R32F cube, 3 levels | linear, linear, linear | |
| s2 | 4x4x4 A32B32G32R32F volume, 3 levels | linear, point, none | border, clamp, mirror once |
| s3 | 8x8 A8R8G8B8, 4 levels | point, linear, point | sRGB, clamp, `MAXMIPLEVEL` 1 |

The texels are hashes in 0 to 1.

- **The reference:** C++ (`RefSample` in the demo) works the samples out with the measured model,
  written separately from the driver's. The demo also reads them back from the GPU (`hw` rows),
  as for packed outputs: each vertex drawn as a point into a float target, through a pixel shader
  that writes its input. Natively the two agree within 0.0037 (`native_reference_texldl.txt`).
- **`d3d9_mesh_check.py`:** a draw with `hwfloat 0.01` in the reference compares VS Out with the
  GPU within 0.01, and with the C++ reference as floats (1e-4), which is a failure too if it
  differs.
- **`D3D9_Mesh_Output.py`** also fails if the reference has no texldl filtering draw: the demo
  leaves it out, with a warning, on a GPU without float cube and volume vertex textures or sRGB
  reads.
- **Unit tests:** a new `[vsemu]` test of `SampleLevel`: bilinear weights and their rounding,
  each address mode per texel, the border colour on one axis, magnification and minification
  around LOD 0 and with `MAXMIPLEVEL`, the LOD's rounding, linear and point mip filters, the bias,
  a cube face's edge, and a trilinear volume with a border. The existing `texldl` test sets a
  point mip filter where it tests levels, checks NONE reads level 0, and checks the cube face at
  ties.

## Results

### Automated tests

All 17 passed, x64 and x86 (`run_summary.txt`, `run_x64.txt`, `run_x86.txt`):

```
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:17.199742 ##
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:17.947228 ##
```

The new draw, in both:

```
.. PASS vs_3_0 texldl filtering (VS In 96 vertices, VS Out 96 rows (25 unique), unproject True, 96 rows checked against the GPU within 0.01)
```

The change is replay only; the x86 run captured the 32-bit demos, which include the new draw and
its GPU readback, and the existing "vs_3_0 loops and texldl" draw (point sampling) passed as
before.

### Other checks

- **Unit tests:** `[d3d9]` passed (6147 assertions in 15 test cases, `unit.txt`).
- **Builds:** `renderdoc.sln` and `demos.sln` built with 0 warnings and 0 errors, for both x64 and
  x86.
- **Natively:** the texldl filtering draw's C++ reference and the GPU agree within 0.0037 (per
  output: s0 0.0025, s1 0.0022, s2 0.0017, s3 0.0037), against a tolerance of 0.01. The
  reference written under RenderDoc is byte-identical to the native one (the test checks this).

### The old driver

With the driver from de78c37ab and the new demo and test (`run_old_driver.txt`), the new draw
fails, and so the test does (720 problems: 96 rows, four outputs each, compared with the GPU and
with C++):

```
FAIL vs_3_0 texldl filtering (...): 720 problems
    instance 0 row 0: (GPU) TEXCOORD.0 ours ['0.341009', '0.670039', '0.0315251', '0.501457'] reference ['0.40704', '0.630692', '0.214945', '0.355232']
    instance 0 row 0: (C++) TEXCOORD.0 ours ['0.341009', '0.670039', '0.0315251', '0.501457'] reference ['0.40704', '0.630692', '0.214945', '0.355232']
    instance 0 row 0: (GPU) TEXCOORD.1 ours ['0.100664', '0.74963', '0.608927', '0.0817578'] reference ['0.798197', '0.418827', '0.697783', '0.262336']
    instance 0 row 0: (GPU) TEXCOORD.3 ours ['0.647059', '0.568627', '0.0431373', '0.352941'] reference ['0.376953', '0.283203', '0.00335693', '0.352941']
```

The old values are single texels: TEXCOORD.3 is an unconverted A8R8G8B8 texel (0.647059 is
165/255), where the GPU gave an sRGB-converted, filtered one. The other 16 draws passed as
before.

## UI

I captured the demo and opened it in qrenderdoc at the texldl filtering draw (event 2891):
- **`ui_1_vs_out.png`:** the Mesh Viewer's VS Output, scrolled across in three parts. Row 0
  (vertex 0) has TEXCOORD0 (0.40704, 0.63069, 0.21495, 0.35523) and TEXCOORD1 (0.7982, 0.41883,
  0.69778, 0.26234), the GPU's values to 5 digits, where the old driver gave (0.341, 0.670,
  0.032, 0.501) and (0.101, 0.750, 0.609, 0.082). TEXCOORD2 is the border colour (0.2, 0.4, 0.8,
  1) where its u is outside the volume, and TEXCOORD3 (0.37626, 0.28315, 0.00335, 0.35294) is
  the C++ reference's (the GPU's is 0.37695, 0.28320, 0.00336, 0.35294).
- **`ui_2_vs_samplers.png`:** the Pipeline State's vertex shader: the four vertex textures and
  their sampler states as the demo set them (s2 Min Point, Mag Linear, Mip None with the border
  colour; s3 Min Linear, Mag Point, Mip Point, Max Mip Level 1, sRGB). That display was already
  there; nothing changed in it.

The debug message count was 0.

## Found on the way

The first version of the demo's volume sampler had `MAXMIPLEVEL` 1 without a mip filter, and its
four vertices at LOD exactly 0 differed from the GPU by up to 0.34: the GPU had minified. The
probe had only tried `MAXMIPLEVEL` with the same filter for both, so it was extended with the
mixed filters around LOD 0 (`vol_LPN_maxmip1` and others), which showed that `MAXMIPLEVEL` picks
the filter even without a mip filter. The model, the driver and the demo's reference follow that
now. The demo's volume sampler went back to `MAXMIPLEVEL` 0, so that it magnifies at LOD 0 or less,
and the unit test covers `MAXMIPLEVEL` with no mip filter.

## Not covered

- **Other GPUs:** filtering was measured on an RTX 4070 Ti only. Early SM3 GPUs could only point
  sample vertex textures (and only from float formats), and others may round differently.
- **Software vertex processing** filters differently (above), and isn't emulated.
- **sRGB** was measured on A8R8G8B8 only. The driver converts formats with up to 8 bits per
  colour channel (the 8-bit RGB formats, the 5- and 4-bit ones, L8, A8L8, A4L4 and DXT); none of
  the others was tried, and float formats aren't converted.
- **1D textures, DXT and other formats as vertex textures** weren't probed; they go through the
  same filtering.
- **Real games:** no game with filtered vertex textures was tried.
