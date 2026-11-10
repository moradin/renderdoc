# D3D9 step 8: UpdateTexture dirty regions in captures

Tested on the working tree on top of dbe9df725, which was committed as "D3D9: save UpdateTexture's
dirty regions in captures". Machine: Windows 11, RTX 4070 Ti. Files are in
`d3d9_step8_dirty_regions/`.

## What changed

UpdateTexture copies only the part of a system memory texture the runtime considers dirty, then
clears it. Texture streaming relies on that: an application rewrites part of a staging texture and
uploads only that part. Before this change captures didn't store the source's dirty region. At
replay, writing the initial contents made the whole source dirty, so the frame's first
UpdateTexture from it copied all of it, overwriting parts of the destination that the application
never uploaded.

Now:
- **Capture:** the initial contents of each D3DPOOL_SYSTEMMEM texture hold its dirty region as the
  frame started, one per cube face. Anything in the frame that changes the region (a lock,
  AddDirtyRect or AddDirtyBox, an UpdateTexture from it) reads the initial contents first. The
  capture format is now version 0x2, and version 0x1 captures still open.
- **Replay:** after writing a system memory texture's initial contents, the region is cleared, by
  an UpdateTexture into a scratch default pool texture holding just the smallest level. The captured
  rects are then added back with AddDirtyRect and AddDirtyBox. Locks in the frame that had
  D3DLOCK_NO_DIRTY_UPDATE are written back with it, so they don't add to the region at replay.
- **Tracking:** while testing, the runtime turned out not to keep the region the way we tracked it,
  as one box around everything. See below. The tracking now follows what was measured, which also
  fixes our shadow copies of default pool textures (initial contents of textures UpdateTexture
  fills), which took the box around the rects instead of the rects.

## How the runtime keeps the dirty region

Measured natively with a probe program (`probe/d3d9_dirty_probe.cpp`, output in `probe/probe.log`,
not committed as a demo). Each case fills a system memory texture, copies it, refills it with
D3DLOCK_NO_DIRTY_UPDATE, adds rects, copies it again and counts the texels that changed:

```
mode adddirty n 6: 6 texels copied         (six 1x1 rects)
mode adddirty n 7: 4096 texels copied      (a 7th: all of the 64x64 texture)
mode lock n 6: 6 texels copied             (the same with locks)
mode lock n 7: 4096 texels copied
identical n 6: 1 texels copied             (the same 1x1 rect six times)
identical n 7: 4096 texels copied          (seven times: all of it)
big then 6 inside: 4096 texels copied      (rects inside another count too)
volume (hr 0) n 6 boxes: 6 texels copied
volume (hr 0) n 7 boxes: 1024 texels copied
cube 2+0 rects: per face 27 0 0 0 0 0      (two 1x1 rects on face 0: the 9x3 box around them)
cube 7+0 rects: per face 637 0 0 0 0 0     (seven: still the box around them, not the face)
cube 3+3 rects: per face 85 85 0 0 0 0     (each face has its own)
surface lock of level 0 2x2: 4 texels copied
```

So 2D and volume textures keep up to 6 rects or boxes, each as it was added, and the 7th makes the
whole texture dirty. Each cube face keeps one rect that grows to cover what's added to it. A lock
through the level-0 surface counts like a lock of the texture.

## The tests

**`D3D9_Dirty_Regions`** is a new demo and test. The end of each frame refills its system memory
textures without making them dirty and sets their dirty regions. The next frame copies each into a
default pool texture:
1. a 16x16 2D texture with 2 mips, two rects dirty: (0,0)-(8,8) and (10,12)-(13,15);
2. an 8x8 cube texture, with two rects on face 1, (0,0)-(2,2) and (5,5)-(7,7), so their bounding
   box (0,0)-(7,7) is copied, and all of face 2;
3. an 8x8x4 volume with two boxes;
4. three 8x8 2D textures: no rects, 6 rects, and 7 rects (all of it);
5. the 2D texture again, after a lock of (8,8)-(12,12), a lock of (12,0)-(16,4) with
   D3DLOCK_NO_DIRTY_UPDATE, and a lock of mip 1: only the first is copied.

Before the copies, the frame also locks the 2D texture read-only, which mustn't make it dirty.

- **The demo's own GPU checks:** every frame it draws each default pool texture, every mip, face
  and slice, point sampled, reads it back and compares it with a model on the CPU of what the
  regions say was copied. It logs "UpdateTexture copied the dirty regions in all 11 frames" at
  frame 10, or an error at the first difference. This is how the rules above were confirmed in a
  real frame, natively and under RenderDoc (where frame 5 is captured).
- **The test** runs the demo natively first and requires that line in both logs. In the capture it
  checks:
  - the default pool textures as the frame starts: what the previous frame copied, on the base
    colour, from our shadow copies;
  - at each of the 7 UpdateTexture events, that the destination changed exactly in the expected
    regions (to the source's texels), and nowhere else, at each mip, cube face and volume slice. Outside
    the regions the source has to differ from the destination, so that copying too much shows.

## Results

### Automated tests

All 17 passed (16 before, and `D3D9_Dirty_Regions`), x64 and x86:

```
run_x64.txt:## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:21.011849 ##
run_x86.txt:## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:19.742571 ##
```

These are from the final code. An earlier full run, before the initial contents' size estimate
counted the dirty boxes, also passed 17 of 17 on both. The x86 run captured a 32-bit process (`test_log_x86.txt`). `test_log_x64.txt` has every check of
the new test.

The first full run of the new test failed one check, which was wrong in the test, not the driver:
the 2D texture's mip 1 as the frame starts, in the region the previous frame's lock copied. The lock
only wrote mip 0, so mip 1 there has the source's frame colour, which the demo's model already had.

### Other checks

- **Unit tests:** `[d3d9]` passed (6057 assertions in 13 test cases).
- **Builds:** `renderdoc.sln` and `demos.sln` built with 0 warnings and 0 errors, for both x64 and
  x86.
- **Natively:** `native_demo.log` has "UpdateTexture copied the dirty regions in all 11 frames".

### The old driver

I put back the driver files from dbe9df725, rebuilt, and ran the new test (`run_old_driver.txt`).
It fails at its first check, the shadow copy of the 2D texture as the frame starts:

```
FAILURE in D3D9_Dirty_Regions: The 2D texture's initial contents, mip 0: 106 texels are wrong,
the first at 8,0 is 0xFF0000FF, expected 0xFF203040
```

That's the box around the two rects, which the old tracking copied into the shadow. To see every
copy, a script checks each UpdateTexture without stopping at the first difference. With the old
driver on its own capture (`check_old_driver.txt`), 6 of the 7 copies are wrong:

```
event 3: the 2D texture, with two rects dirty since the last frame: FAIL, 227 texels wrong
event 4: the cube texture, with two rects on face 1 (...) and all of face 2: FAIL, 271 texels wrong
event 5: the volume texture, with two boxes dirty: FAIL, 220 texels wrong
event 6: the texture with nothing dirty: FAIL, 64 texels wrong
event 7: the texture with 6 dirty rects: FAIL, 58 texels wrong
event 8: the texture with 7 dirty rects, which make all of it dirty: PASS, 0 texels wrong
event 13: the 2D texture, with the frame's lock dirty (...): FAIL, 16 texels wrong
```

The 7-rect case copies the whole texture either way. Event 13's 16 texels are the
D3DLOCK_NO_DIRTY_UPDATE lock, which the old replay wrote back as a normal lock.

With the new driver on its own capture (`check_new_driver.txt`), all 7 pass. With the new driver
on the old driver's version 0x1 capture (`check_new_driver_old_capture.txt`), it opens and replays.
The 5 copies that need the starting dirty region fail as before, since that capture doesn't have
it, and event 13 passes.

## UI

I opened the new capture in qrenderdoc and viewed the 2D texture's destination (Texture 23) in the
texture viewer:
- **`ui_1_before_copy.png`:** event 2, before the first UpdateTexture. The two rects the previous
  frame copied are blue, the previous frame's lock is magenta, and the rest is the base colour.
- **`ui_2_after_copy.png`:** event 3, after it. Only the two dirty rects changed, to the source's
  green.
- **`ui_3_after_lock_copy.png`:** event 13, after the copy of the frame's locks. Only (8,8)-(12,12)
  changed, to this frame's yellow. The D3DLOCK_NO_DIRTY_UPDATE lock at the top right didn't
  change.

The debug message count was 0.

## Not covered

- **Other ways to make a texture dirty:** GetDC on level 0, or GetRenderTargetData into a level of
  a system memory texture, may add to the region. They weren't measured and aren't tracked.
- **Formats a default pool texture can't have:** the region can't be cleared at replay (a warning
  is logged), so those textures stay entirely dirty. Not tried.
- **Compressed formats:** the rects are aligned to blocks for the shadow copies, as before. Only
  A8R8G8B8 was tested.
- **Real games:** not tried with a real game's texture streaming.
- **Other GPUs and Windows versions:** the runtime's rules were measured on Windows 11 only.
