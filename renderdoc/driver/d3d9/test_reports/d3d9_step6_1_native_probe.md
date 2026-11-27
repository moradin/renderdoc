# D3D9: what the GPU does with what pixel history needs (step 6.1)

Step 6.1 of the pixel history plan (STATUS.md, "Step 6 plan"). This is a native probe and a report,
with no driver change. It measures what the later chunks rely on.

Run natively, without RenderDoc, on Windows 11 with an RTX 4070 Ti (driver 32.0.16.2124), at
commit 892b7a965 (the plan). Every number here is from the GPU.

Files are in `d3d9_step6_1_native_probe/`:
- `d3d9_pixel_history_probe.cpp` is the probe. It's a demo, `D3D9_Pixel_History_Probe`, that was
  built into `demos_x64.exe` for the run and taken out again, as 5.10's was. It isn't in
  `demos.vcxproj`.
- `probe.txt` is its output, which it writes to the folder named by `PROBE_OUT`.

To run it again, copy the probe into `util/test/demos/d3d9/`, add it to `demos.vcxproj`, build,
and run `demos_x64.exe D3D9_Pixel_History_Probe` with `PROBE_OUT` set.

The decisions are summed up in STATUS.md, "What the GPU does for pixel history".

## A: occlusion queries scissored to one pixel

The draws are triangles covering a 16x16 A32B32G32R32F target. The scissor is the one pixel
(5, 7); depth, stencil, blending and culling are off.

```
  1 covering triangles: 256 unscissored (16x16), 1 scissored to (5, 7), 1 with COLORWRITEENABLE 0
  ...
  5 covering triangles: 1280 unscissored (16x16), 5 scissored to (5, 7), 5 with COLORWRITEENABLE 0
  a triangle whose top edge is at y 6.99, pixel (5, 7) centre at 7.0: 1
  a triangle whose top edge is at y 7.00, pixel (5, 7) centre at 7.0: 1
  a triangle whose top edge is at y 7.01, pixel (5, 7) centre at 7.0: 0
  alpha test failing 0, passing 1; texkill killing 0, keeping 1
  the triangle (clockwise on screen) with CULL_CW 0, CULL_CCW 1; fixed function 1
  4x multisampled: fully covered 4, MULTISAMPLEMASK 0x5 2, MULTISAMPLEANTIALIAS FALSE 4, top edge on the centre 2
```

- **Counting.** A query scissored to one pixel counts that pixel's fragments exactly: one per
  covering triangle, whether colour writes are on or off.
- **Coverage.** It follows the top-left rule: an edge through the pixel centre covers it.
- **What removes a fragment:** the alpha test, `texkill` and culling all remove it from the count.
  A fixed function draw counts like any other.
- **Multisampled targets:** the query counts samples. A fully covered pixel counts 4, one
  `MULTISAMPLEMASK` 0x5 lets through counts 2, and a half covered pixel counts 2.
  `MULTISAMPLEANTIALIAS` FALSE counts all 4, since every sample is written (as in 5.10).

## B: 1x1 StretchRect copies

Each render target format was filled with random bits through UpdateSurface. For the float
formats, NaNs, infinities, denormals and -0 were included. Each pixel was then copied alone with a
1x1 StretchRect into a 1x1 target of the same format and read with GetRenderTargetData. That
value was compared with the whole target read back.

| Format | Result |
|---|---|
| A8R8G8B8, R5G6B5, A2R10G10B10, A2B10G10R10, A16B16G16R16, G16R16, R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F | 0 of 256 pixels differ, with both D3DTEXF_NONE and POINT. NaN payloads, infinities, denormals and -0 are kept |
| X8R8G8B8 | 255 differ, all in the X byte alone, which comes back 0 (`7a9dc883->009dc883`) |
| X1R5G5B5 | 126 differ, all in the X bit alone, which comes back 0 (`be29->3e29`) |
| A8, L8, L16 | Render target textures exist, but a system memory surface of the format can't be created (D3DERR_INVALIDCALL), so GetRenderTargetData can't read them |
| A8B8G8R8, X8B8G8R8, A1R5G5B5, A4R4G4B4, X4R4G4B4, R3G3B2, A8R3G3B2, A8L8 | Not render targets on this GPU |

Other cases:
- **Render target textures:** a mip level (level 1 of 32x32) and a cube face (-Y) of A8R8G8B8
  copy exactly too.
- **Multisampled targets:** a 1x1 StretchRect of a 4x target resolves that pixel. For A8R8G8B8
  and A32B32G32R32F, it gives exactly what the whole target's resolve gives at every pixel, with
  partly covered pixels included.
- **Inside a scene:** StretchRect works between BeginScene and EndScene too.

## C: a draw's primitives drawn alone

There are 40 random triangles, lines or points on a 32x32 float target, each vertex with random
texture coordinates, which the shader writes out. The whole draw was compared with each primitive
drawn alone, in order, over the same cleared target. A difference in any component counts.

```
 cull none:
  DrawPrimitive TRIANGLELIST                    40 primitives cover  565 pixels; each alone (start 3p): 0 pixels differ
  DrawPrimitive TRIANGLELIST                   prefixes of 1, 8, 15... primitives: at most 0 pixels differ
  DrawPrimitive TRIANGLESTRIP                   40 primitives cover 1015 pixels; each alone (start p): 0 pixels differ
  DrawPrimitive TRIANGLEFAN                     40 primitives cover  952 pixels; each alone (UP lists 0, p+1, p+2): 0 pixels differ
  DrawIndexedPrimitive TRIANGLELIST, base 5     40 primitives cover  838 pixels; each alone (start index 3p): 0 pixels differ
  DrawIndexedPrimitive TRIANGLESTRIP, base 5    40 primitives cover  791 pixels; each alone (start index p): 0 pixels differ
  DrawPrimitiveUP TRIANGLELIST                  40 primitives cover  565 pixels; each alone (pointer + 3p): 0 pixels differ
  DrawIndexedPrimitiveUP TRIANGLELIST           40 primitives cover  838 pixels; each alone (indices + 3p): 0 pixels differ
  DrawPrimitive LINELIST                        40 primitives cover  311 pixels; each alone (start 2p): 0 pixels differ
  DrawPrimitive LINESTRIP                       40 primitives cover  327 pixels; each alone (start p): 0 pixels differ
  DrawPrimitive POINTLIST                       40 primitives cover   27 pixels; each alone (start p): 0 pixels differ
  DrawPrimitive POINTLIST, sprites of 5         40 primitives cover  501 pixels; each alone (start p): 0 pixels differ
  flat shading takes vertex: list triangle 0 -> 0; strip triangles 0, 1 -> 0, 1; fan triangles 0, 1 -> 1, 2
 cull CCW:
  DrawPrimitive TRIANGLESTRIP                   40 primitives cover  535 pixels; each alone (start p): 401 pixels differ
  DrawPrimitive TRIANGLESTRIP, odd ones reversed  40 primitives cover  535 pixels; each alone (odd as UP lists p+1, p, p+2): 0 pixels differ
  DrawIndexedPrimitive TRIANGLESTRIP, base 5    40 primitives cover  679 pixels; each alone (start index p): 488 pixels differ
  (the lists, the fan and the UP draws: 0 pixels differ)
```

The instancing lines are under "cull CCW" in `probe.txt`, but culling was off for them. They use
4 instances of 8 triangles, with vs_3_0 and stream frequencies:

```
  instanced, each instance's each primitive     32 primitives cover  662 pixels; each alone (instance-major): 0 pixels differ
  instanced, each instance's each primitive     32 primitives cover  662 pixels; each alone (primitive-major): 66 pixels differ
  instanced, each primitive for every instance   8 primitives cover  662 pixels; each alone (4 instances each): 66 pixels differ
  instanced, each instance whole                 4 primitives cover  662 pixels; each alone (1 instance each): 0 pixels differ
```

- **Parts of a draw are exact.** A part of a draw covers the same pixels with the same
  interpolated values, bit for bit, as in the whole draw. That holds for every topology and draw
  call, prefixes included.
- **Fans.** A fan's triangle drawn as the list (0, p+1, p+2) is exact too, since a fan can't be
  started part way.
- **Strips and culling.** A strip's odd triangle drawn alone, starting at an odd vertex, has the
  opposite winding. With culling on it's culled the wrong way (401 and 488 pixels differ). Drawn
  as (p+1, p, p+2), the winding it has in the strip, it's exact. A prefix always starts at vertex
  0, so prefixes keep every triangle's winding.
- **Instancing.** D3D9 draws instance by instance: all of instance 0's primitives, then instance
  1's. One instance drawn alone (stream 1's offset moved on, a count of 1) is exact, whole or one
  primitive at a time. Drawing one primitive for every instance is in a different order from the
  draw.
- **Flat shading** takes a list's and a strip's first vertex (vertex p for strip triangle p), and
  a fan's second (vertex p+1).

## D: stencil INCR_SAT counting fragments, EQUAL picking fragment N

The stencil is cleared to 0. The stencil func is EQUAL with ref N, and the pass, fail and z-fail
ops are all INCRSAT, for both faces with two-sided stencil. Fragment N is then the only one that
passes. Each triangle writes 10 plus its index; the clear colour's red is 1.

```
  plain, 6 triangles (both windings, red 10 + their index), ref 0-6 write: 10 11 12 13 14 15 1; 0 pixels not as expected (10 + n, then the clear colour's 1)
  300 triangles (red 10 + their index): ref 0 -> 10 ref 200 -> 210 ref 254 -> 264 ref 255 -> 309
  4 triangles (red 10 + index), 1 killed by texkill and 2 by the alpha test: ref 0-4 write: 10 13 1 1 1 (1 is the clear colour)
  3 triangles (red 10 + index), 1 failing the depth test (STENCILZFAIL INCRSAT too): ref 0-3 write: 10 1 12 1 (1 is the clear colour)
  4x, MULTISAMPLEANTIALIAS FALSE, 6 triangles (both windings, red 10 + their index), ref 0-6 write: 10 11 12 13 14 15 1; 0 pixels not as expected (10 + n, then the clear colour's 1)
```

- **Order.** Fragments are counted in primitive order, with both windings, and on a 4x
  multisampled target with `MULTISAMPLEANTIALIAS` FALSE. So ref N picks the Nth fragment, which
  is how D3D11 gets each fragment's shader output.
- **Saturation.** The count stops at 255. Ref 255 then picks every fragment from the 256th on, so
  the last one wins (triangle 299).
- **Killed fragments.** A fragment killed by `texkill` or the alpha test never reaches the stencil
  test, so it isn't counted: with triangles 1 and 2 killed, ref 1 picks triangle 3.
- **Depth failures.** A fragment failing the depth test is counted (STENCILZFAIL INCRSAT) but writes
  nothing: ref 1 picks triangle 1, which leaves the clear colour.

## E: Clear, ColorFill and StretchRect

A 16x16 target is cleared to red; each case then writes green (or blue from StretchRect).

```
  Clear, viewport (4,4) 8x8                                  green: 64 pixels in (4,4)-(11,11)
  Clear, scissor (2,2)-(7,7)                                 green: 25 pixels in (2,2)-(6,6)
  Clear, viewport (4,4) 8x8 and scissor (2,2)-(7,7)          green: 9 pixels in (4,4)-(6,6)
  Clear of rect (0,0)-(4,4), scissor (2,2)-(7,7)             green: 4 pixels in (2,2)-(3,3)
  Clear, COLORWRITEENABLE green only (red stays = yellow)    green: 256 pixels in (0,0)-(15,15)
    pixel (0,0) is (0, 1, 0, 1)
  ColorFill of (0,0)-(4,4), viewport and scissor as above    green: 16 pixels in (0,0)-(3,3)
  StretchRect of all of a blue target, viewport and scissor as above   blue: 256 pixels in (0,0)-(15,15)
  depth Clear to 0 with scissor (2,2)-(7,7); draw at 0.5 LESS green: 231 pixels in (0,0)-(15,15)
```

- **Clear** writes only inside the viewport, the scissor rect when the scissor test is on, and its
  rects, whichever apply. That holds for depth too: 256 - 25 pixels pass a draw after the
  scissored depth clear.
- **Clear ignores `COLORWRITEENABLE`.** With green alone enabled, red was cleared to 0 all the
  same.
- **ColorFill and StretchRect** ignore the viewport and the scissor rect; only their own rects
  count.

## F: depth clipping, D3DRS_CLIPPING and user clip planes

The draws cover the whole 16x16 target, onto an INTZ depth texture cleared to 0.25, with ZFUNC
ALWAYS and depth writes. Each depth is read back by sampling the texture. The vs_2_0 draw passes
its clip space position through.

```
  vs_2_0, CLIPPING TRUE , z  1.5 to  1.5:   0 pixels
  vs_2_0, CLIPPING TRUE , z -0.5 to -0.5:   0 pixels
  vs_2_0, CLIPPING TRUE , z -0.5 to  1.5: 128 pixels, depths in row 7 (x 0, 3, 6...): 0.2500 0.2500 0.2500 0.6250 0.2500 0.2500
  XYZRHW, CLIPPING TRUE , z  1.5:   0 pixels
  XYZRHW, CLIPPING TRUE , z -0.5:   0 pixels
  clip plane x >= 0, CLIPPING TRUE : vs_2_0 128 pixels, XYZRHW 256 pixels
  (CLIPPING FALSE: the same, line for line)
```

- **Depth clipping can't be turned off.** Fragments outside the depth range (0 to w, or 0 to 1
  for pretransformed vertices) aren't drawn, and `D3DRS_CLIPPING` FALSE changes nothing (this is
  hardware vertex processing). A triangle crossing the range is cut where it leaves it.
- **User clip planes** clip vertex shader draws, in clip space, with `D3DRS_CLIPPING` FALSE too.
  They don't clip pretransformed (XYZRHW) vertices.

## G: sRGB writes and blending

An A8R8G8B8 target; the shader writes one value from c0.

```
  SRGBWRITEENABLE FALSE, red stored: 0->00 0.002->00 0.0031308->01 0.04045->0a 0.2->33 0.5->7f 0.735->bb 1->ff
  SRGBWRITEENABLE TRUE , red stored: 0->00 0.002->07 0.0031308->0a 0.04045->39 0.2->7c 0.5->bc 0.735->df 1->ff
  sRGB writes, target 80 80 80 80, 0.1 ONE/ONE: stored (r g b a) cc bc bc 99
  sRGB writes, target 80 80 80 80, 0.0 alpha 0.5 SRCALPHA/INVSRCALPHA: stored (r g b a) 8a 8a 8a 7f
```

- **Writing.** sRGB writes encode the colour with the sRGB curve (0.5 to 0xbc), and not alpha.
- **Blending.** With blending, the target's stored value is used as it is, not decoded to linear.
  It's blended with the shader's output and the result encoded:
  - 0x80 / 255 + 0.1 = 0.602, encoded 0xcc (203.7). Decoding first would give 0x98, and adding the
    encoded output 0xd9.
  - Green, with nothing added, is 0x80 encoded, 0xbc.
  - The SRCALPHA case gives 0x8a, where the formula gives 137.2 (0x89): one step out, in the
    GPU's encoding.

## H: how long it takes

```
  an occlusion query around a one-triangle draw, waited for: 0.016 ms; a 1x1 StretchRect, GetRenderTargetData and lock: 0.025 ms (each the mean of 200)
```

Both are cheap. A pixel history's time will go on replaying the frame up to each event.

## What it means for the next chunks

These are recorded in STATUS.md:

- **Which events touched the pixel, and why fragments failed:** occlusion queries scissored to
  the pixel (6.2, 6.3). On multisampled targets they count samples.
- **Pixel values:** a 1x1 StretchRect and GetRenderTargetData, exact for every render target
  format here, ignoring X bits. Multisampled targets give the resolved value. A8, L8 and L16 need
  a draw that samples them into a float target instead.
- **Clears and copies, on the CPU, with no query** (6.2):
  - a Clear touches the pixel when the pixel is inside the viewport, the scissor rect (when on)
    and the rects;
  - ColorFill and StretchRect by their rects alone.
- **Depth clipping:** `depthClipped` can't be found this way. Fragments outside the depth range
  are never drawn, so a draw whose fragments are all clipped in depth isn't listed. User clip
  planes can be turned off in the queries to find fragments they clip.
- **Fragments within a draw:** counted and picked with stencil as D3D11 does (6.4), at most 255.
  The count is taken twice, as D3D11 does:
  - with the alpha test off and a shader that doesn't discard, every fragment;
  - with the original shader and alpha test, the ones that survive.
- **Primitive IDs** (6.5) come from counting prefixes of the draw, which keep every triangle's
  winding:
  - the fragments that primitives 0 to p-1 make, then those up to p;
  - with instancing, the instances before it whole, then the instance alone.

  The first fragment of primitive p is then picked out of the prefix that ends with p with
  stencil EQUAL. So debugging a chosen fragment needs no primitive drawn on its own.
- **sRGB:** the shader output and the stored values are read as they are, so nothing about sRGB
  blending needs emulating.

## Not covered

- **Other GPUs:** AMD's and Intel's queries, copies and sRGB blending may differ.
- **Clipping with software vertex processing:** whether `D3DRS_CLIPPING` FALSE turns off clipping
  there wasn't tried.
- **Formats:** depth stencil formats as copy sources, and D16 or D24S8 depth through the
  bisection at one pixel. Those were measured in step 8 (`d3d9_step8_d24_levels.md`), not here.
- **Lines and points with culling:** they aren't culled, so only triangles were tried with it.
- **Triangle orders:** a strip's odd triangle drawn as (p, p+2, p+1), the order that would keep
  flat shading's vertex, wasn't tried. The prefix approach doesn't need it.
