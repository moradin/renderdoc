# D3D9 step 8: SetLOD, and replaying on the capture's kind of device

Tested on the working tree on top of 5609dcb0b, which was committed as "D3D9: replay plain D3D9
captures on a plain D3D9 device, so SetLOD works". Machine: Windows 11, RTX 4070 Ti (driver
32.0.16.1088). Files are in `d3d9_step8_setlod/`.

## What changed

`IDirect3DBaseTexture9::SetLOD` only works on D3DPOOL_MANAGED textures. Replay was always on a
D3D9Ex device, which has no managed pool, so managed textures went in the default pool and
replay's SetLOD did nothing: every draw sampling a texture with a LOD set read from level 0
instead. A SetLOD before the frame wasn't captured at all, and the descriptors showed mip 0 as the
first. STATUS.md listed only the last of these.

Emulating SetLOD on a default pool texture would have needed a copy of each texture starting at
its LOD's level, swapped in around the draws (see "What the GPU does" for why a sampler state
can't do it). Instead, captures replay on the kind of device they were made with:
- **A plain D3D9 device** for plain D3D9 captures (`D3D9InitParams::Ex` is false), where managed
  textures and buffers stay managed, and SetLOD works on the GPU as it did in the program.
- **A D3D9Ex device** for D3D9Ex captures, which need one for their Ex calls and can't have
  managed objects anyway, so they never use SetLOD.
- **`D3D9_ReplayOnExDevice`**, a new config setting (off by default), replays plain D3D9 captures
  on D3D9Ex as before, in case the plain device causes trouble.

A plain D3D9 device can be lost, which is probably why replay used D3D9Ex. Recovering is left
out: `FatalErrorCheck` checks `TestCooperativeLevel` and reports the device lost, and the capture
has to be opened again. See "Device loss" for what happened when that was tried.

Also:
- Captures from version 0x4 hold each texture's LOD as the frame started, in its initial contents.
  SetLOD reads the initial contents before changing it (as locks do), and replay sets it when it
  applies them, which also undoes the frame's SetLOD calls when replaying from the start.
- The descriptors start at the LOD's level, with that many fewer mips.
- The texture viewer now samples managed textures directly (it decoded them on the CPU before),
  so it sets the LOD to 0 while it does and back afterwards (`ScopedBaseLOD`), as do custom
  display shaders.
- VS Out emulates `texldl` with the levels from the LOD's on.

## What the GPU does

A probe (`probe/d3d9_lod_probe.cpp`, not committed as a demo; `probe/probe.log`,
`probe/table_2d.txt`, `probe/table_vertex.txt`) samples a 64x64 managed texture with 7 levels,
each level a solid red of 0x10 + 0x20 * level with a green checker (so magnification, which uses
MAGFILTER LINEAR here, shows as a half green), with SetLOD 0, 1, 2, 3, 6 and 9, MAXMIPLEVEL 0, 1,
2 and 4, MIPFILTER NONE, POINT and LINEAR, and a bias of 0 and 1, at explicit LODs (`tex2Dlod`)
and with derivatives (`tex2Dgrad`). The same for a cube, a volume and a vertex texture. Then the
same again on a default pool copy with MAXMIPLEVEL raised to the LOD instead.

From the GPU:

```
managed SetLOD(3) returned 0, GetLOD 3
managed SetLOD(9) returned 3, GetLOD 6
managed SetLOD(0) returned 6, GetLOD 0
default SetLOD(2) returned 0, GetLOD 0
```

and from the tables (level read, per case, at the 11 explicit LODs then the 10 derivative ones):
- MIPFILTER NONE reads level L whatever MAXMIPLEVEL is (`L1 M2 F0`: 1 everywhere).
- An explicit LOD counts from L: `L1 M0 F1 b0` reads 1, 1, 2, 2, 3, 3, 4, 4, 5, 6, 6 at LODs -1,
  0, 0.5, 1, ... 8, which is L = 0's row moved up a level.
- MAXMIPLEVEL counts from L: `L1 M2 F1 b0` doesn't go below level 3.
- With derivatives the LOD is λ - L, and magnification (the half green, `m`) lasts up to λ = L.
- Cube, volume and vertex textures gave the same.

MAXMIPLEVEL raised to L on a default pool texture isn't the same, since it clamps the LOD rather
than moving level 0:

```
2D: 1588 of 3024 samples differ between SetLOD and MAXMIPLEVEL
cube: 848 of 1584 samples differ between SetLOD and MAXMIPLEVEL
volume: 701 of 1584 samples differ between SetLOD and MAXMIPLEVEL
vertex: 872 of 1584 samples differ between SetLOD and MAXMIPLEVEL
```

## The test

A new demo and test, `D3D9_Texture_LOD`. The demo has a managed 2D (64x64, 7 levels), cube (32, 6
levels), volume (16, 5 levels) and vertex texture (64x64, 7 levels), each level a solid colour
that says the texture and level. Before the first frame it sets their LODs to 2, 1, 3 and 3. In
each frame it samples each one at LODs 0 to 7 (one row of a 16x4 target per texture, the vertex
one through points whose colour the vertex shader samples) with MAXMIPLEVEL 0, then sets the LODs
to 4, 3, 1 and 5 and samples them again into a second target with MAXMIPLEVEL 1, then sets them
back. Natively it checks every sample against the level SetLOD and MAXMIPLEVEL say (the level
`min(L + max(lod, M), levels - 1)`), and logs at frame 10 if all passed.

The test:
- checks both runs logged that;
- at replay, reads each target at its pass's last draw and checks every row: the second pass
  first, so that going back to the first replays over the frame's SetLOD, which the start of the
  frame has to undo;
- checks each draw's descriptor for the texture starts at the LOD's level with that many fewer
  mips;
- checks VS Out of the vertex texture draws, colour by colour;
- displays the 2D texture's mips 0, 3 and 6 in a headless texture output at the second pass,
  where its LOD is 4, and checks the colour shown;
- replays every action;
- opens the capture again with `D3D9_ReplayOnExDevice` set, and checks that both passes read as
  if every LOD were 0 and the descriptors start at mip 0.

## Results

### Builds and tests

From `run_summary.txt` (the build logs and full test output are next to it):

```
renderdoc_x64 exit 0 warnings 0 errors 0
demos_x64 exit 0 warnings 0 errors 0
renderdoc_x86 exit 0 warnings 0 errors 0
demos_x86 exit 0 warnings 0 errors 0
All tests passed (6147 assertions in 15 test cases)

## Tests complete summary: 18 passed out of 18 run from 209 total in 0:01:26.828390 ##
## Tests complete summary: 18 passed out of 18 run from 209 total in 0:01:29.269134 ##
```

The two summaries are x64 (`run_x64.txt`) and x86 (`run_x86.txt`). Every D3D9 test made with a
plain D3D9 device now replays on a plain D3D9 device, and passed unchanged.
`D3D9Ex_Wrapping_Check`'s capture is from a D3D9Ex device, so it replays on D3D9Ex as before.

`D3D9_Texture_LOD`, x64 (x86 is the same):

```
E:\Projects\renderdoc\util\test\demos\d3d9\d3d9_texture_lod.cpp:444 Log: All 704 samples read the levels SetLOD says
    ** The capture has no debug messages
    ** 'LOD set in the frame': every row read the levels expected
    ** 'LOD set before the frame', after replaying 'LOD set in the frame': every row read the levels expected
    ** 'LOD set before the frame': the descriptors and D3D9 samplers start at the LOD's level
    ** 'LOD set before the frame': VS Out read the vertex texture's levels from the LOD's on
    ** 'LOD set in the frame': the descriptors and D3D9 samplers start at the LOD's level
    ** 'LOD set in the frame': VS Out read the vertex texture's levels from the LOD's on
    ** The texture viewer shows mips 0, 3 and 6 of the 2D texture with LOD 4
    ** Replayed to each of the 20 actions
    ** On a D3D9Ex device, 'LOD set in the frame': every row read the levels expected
    ** On a D3D9Ex device, 'LOD set in the frame': the descriptors and D3D9 samplers start at the LOD's level
    ** On a D3D9Ex device, 'LOD set before the frame': every row read the levels expected
    ** On a D3D9Ex device, 'LOD set before the frame': the descriptors and D3D9 samplers start at the LOD's level
    ** With D3D9_ReplayOnExDevice set, the capture replays with SetLOD ignored
```

The 704 samples are the GPU's, natively and under RenderDoc; the rows the test reads at replay
are the GPU's too, on the replay device. The descriptors, D3D9 samplers and VS Out are
RenderDoc's. The texture viewer's colours are its output, drawn by the GPU.

### Negative checks

To check the test catches what it should, each fix was taken out in turn and the test run again
(`negative/`):
- Without `ScopedBaseLOD` in the texture viewer: `The texture viewer shows the 2D texture's mip 0
  as (144, 64, 1), expected (16, 64, 1)`, which is level 4's colour, the LOD's.
- Without setting the LOD again when the initial contents are applied: `'LOD set before the
  frame', after replaying 'LOD set in the frame': the 2D row reads ['0xff904001', '0xffb04001',
  '0xffd04001', ...`, which is levels 4, 5 and 6: the frame's SetLOD(4) was left in place.

Replaying the test's capture with a build from before this change wasn't done; the negative
checks and the `D3D9_ReplayOnExDevice` run (which is how replay worked before) cover what it
would show.

### Device loss

A temporary demo (`device_loss/d3d9_fullscreen_probe.cpp`, not committed as a demo) creates a
D3D9 device in exclusive fullscreen at 1280x720 (the desktop is 3840x2160) and presents for 4
seconds. `device_loss/device_loss.py` opens a `D3D9_Texture_LOD` capture, starts the probe, and
keeps replaying every action while it runs.

On the plain D3D9 device (`device_loss/plain_device.txt`):

```
before: 0
fatal error after 0.63s (round 6): 28 Encountered a GPU device lost error: The D3D9 replay device was lost (D3DERR_DEVICELOST), open the capture again
225663 replay rounds while the probe ran, probe exit 0
after: 28 Encountered a GPU device lost error: The D3D9 replay device was lost (D3DERR_DEVICELOST), open the capture again
```

The replay went on without crashing: after the error the controller replays against a dummy
driver, which is why the rounds got so fast. With `D3D9_ReplayOnExDevice` set
(`device_loss/ex_device.txt`):

```
D3D9_ReplayOnExDevice set
before: 0
102 replay rounds while the probe ran, probe exit 0
after: 0 Success
```

So the plain device can be lost by another program going fullscreen, and the setting avoids it.

### UI

In qrenderdoc on the x64 test's capture (`ui/`, each window captured on its own):
- `01_pipeline_ps_event110.png`: the first draw of "LOD set in the frame". The Pixel Shader
  stage's Textures list Texture 19 as "7 (LOD 4)", Cube Texture 20 as "6 (LOD 3)" and Volume
  Texture 21 as "5 (LOD 1)", with Max Mip Level 1.
- `02_pipeline_vs_event124.png`: the vertex texture draw, Texture 22 "7 (LOD 5)" on the Vertex
  Shader stage.
- `03_texviewer_mip0.png` and `04_texviewer_mip4.png`: Texture 19 at event 110, where its LOD is
  4. Mip 0 shows dark green (16, 64, 1), level 0's colour, and mip 4 orange (144, 64, 1).
- `05_mesh_vsout_event124.png`: VS Out of the vertex texture draw. Every vertex's COLOR is
  0.81569, 0.25098, 0.01569, which is (208, 64, 4), level 6 of texture 3: LOD 5 and MAXMIPLEVEL
  1 on a 7 level texture read level 6 at every LOD, as the GPU did.
- `06_config_editor.png`: Tools, Settings, Core, Config Editor lists the setting as D3D9,
  ReplayOnExDevice, off.

The device-lost message wasn't checked in the UI; it's the same `GetFatalErrorStatus` message
the Python run above shows.

## Not covered

- Recovering from a lost replay device, which isn't done.
- Derivatives: the test samples at explicit LODs. The probe covered them natively, and on the
  plain device the GPU does the sampling, so there's nothing of RenderDoc's in between.
- The texture viewer's LOD reset through custom display shaders: it's the same code as the plain
  display, which the test covers.
- Other GPUs, and games.
