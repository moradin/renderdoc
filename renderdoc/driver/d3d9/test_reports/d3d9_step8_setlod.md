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
- **`D3D9_ForceReplayOnExDevice`**, a new config setting (off by default), replays plain D3D9
  captures on D3D9Ex as before, in case the plain device causes trouble. It was called
  `D3D9_ReplayOnExDevice` in the first commit, and renamed in the follow-up (see "Follow-up").

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
- opens the capture again with `D3D9_ForceReplayOnExDevice` set, and checks that both passes read as
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
    ** With D3D9_ForceReplayOnExDevice set, the capture replays with SetLOD ignored
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
checks and the `D3D9_ForceReplayOnExDevice` run (which is how replay worked before) cover what it
would show.

### Device loss

A temporary demo (`device_loss/d3d9_fullscreen_probe.cpp`, not committed as a demo) creates a
D3D9 device in exclusive fullscreen at 1280x720 (the desktop is 3840x2160) and presents for 4
seconds. Two scripts run it next to a replay of a `D3D9_Texture_LOD` capture:
- `device_loss/device_loss.py` opens the capture, starts the probe, and keeps replaying every
  action while it runs: a game going fullscreen while a capture is open.
- `device_loss/open_during_fullscreen.py` starts the probe, opens the capture 1.5 seconds later
  while the probe is fullscreen, replays until it exits, then opens the capture again: opening a
  capture while a game holds the display (for example from a second monitor).

The outputs below are from the follow-up build (see "Follow-up"), with the setting's new name and
the new messages. The first commit's run gave the same results, with the loss after 0.63 seconds
and the shorter message.

Replay open, then the program goes fullscreen, on the plain D3D9 device
(`device_loss/plain_device.txt`):

```
before: 0
fatal error after 0.69s (round 6): 28 Encountered a GPU device lost error: The D3D9 replay device was lost (D3DERR_DEVICELOST), most likely because another program took the display in exclusive fullscreen or changed the display mode. Open the capture again once it has left fullscreen. Setting D3D9_ForceReplayOnExDevice replays on a D3D9Ex device, which isn't lost this way but ignores SetLOD
218790 replay rounds while the probe ran, probe exit 0
after: 28 Encountered a GPU device lost error: ...
```

The replay went on without crashing: after the error the controller replays against a dummy
driver, which is why the rounds got so fast. With `D3D9_ForceReplayOnExDevice` set
(`device_loss/ex_device.txt`):

```
D3D9_ForceReplayOnExDevice set
before: 0
97 replay rounds while the probe ran, probe exit 0
after: 0 Success
```

Opening the capture while the program is fullscreen, on the plain D3D9 device
(`device_loss/open_during_plain.txt`):

```
While the program is fullscreen: OpenCapture <Result: 'API initialisation failed while loading the capture: Couldn't create a D3D9 device to replay with (D3DERR_DEVICELOST): another program probably has the display in exclusive fullscreen. Open the capture again once it has left fullscreen, or set D3D9_ForceReplayOnExDevice to replay on a D3D9Ex device, which ignores SetLOD'> after 0.27s
program exit 0
Opened again afterwards: OpenCapture <Result: 'Success'> after 0.32s
replayed every action, read 480000 bytes, status 0
```

Before the follow-up this failed with "Current replaying hardware unsupported or incompatible with
captured hardware: Couldn't create a D3D9 device to replay with: HRESULT<2289436776>", which is
`D3DERR_DEVICELOST` as a number. With the setting (`device_loss/open_during_ex.txt`):

```
D3D9_ForceReplayOnExDevice set
While the program is fullscreen: OpenCapture <Result: 'Success'> after 0.21s
51 replay rounds until the program exited, first error: None
after it exited: 0 Success
Opened again afterwards: OpenCapture <Result: 'Success'> after 0.31s
```

So the plain device can be lost, or not created, while another program holds exclusive
fullscreen, and the setting avoids both. Capturing isn't affected: the D3D9 driver creates no
device of its own in the captured program, only wraps the program's, and RenderDoc has no D3D9
device until a capture is opened, so launching a fullscreen game with no capture open loses
nothing.

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
- `06_config_editor.png` (follow-up build): Tools, Settings, Core, Config Editor lists the setting
  as D3D9, ForceReplayOnExDevice, off.
- `07_device_lost_dialog.png` (follow-up build): with the capture open at event 110, the probe
  went fullscreen and exited; the next event change showed qrenderdoc's "Device Lost error"
  dialog with the D3D9 message (`D3DERR_DEVICENOTRESET` by then, as the probe had left). The
  generic text under it is upstream's, for every API.

Renaming the setting leaves the old name in `%APPDATA%\renderdoc\renderdoc.conf` if the config
was saved with it (Settings' OK saves every setting); the Config Editor then shows it in italics,
as a setting it doesn't know. On this machine it was removed from the file by hand.

## Follow-up: the setting's name and device loss messages

After the first commit (3ecd43cb3):
- the setting was renamed from `D3D9_ReplayOnExDevice` to `D3D9_ForceReplayOnExDevice`;
- creating the replay device while another program has exclusive fullscreen now fails with a
  message saying so, rather than "hardware unsupported" and a raw HRESULT;
- the device lost message says what most likely caused it and what to do;
  `D3DERR_DRIVERINTERNALERROR` keeps a plain "stopped working" message;
- `D3DERR_*` codes in the device creation messages are shown by name.

Builds and the x64 tests for it are in `d3d9_step8_setlod/followup/` (replay only, so no x86 test
run): all 4 builds at 0 warnings, the unit tests passed (6147 assertions), and 18 of
18 tests passed x64, `D3D9_Texture_LOD`'s D3D9Ex check setting the new name:

```
## Tests complete summary: 18 passed out of 18 run from 209 total in 0:01:27.614325 ##
```

## Follow-up: reporting the SetLOD a forced D3D9Ex replay ignores

With `D3D9_ForceReplayOnExDevice` set, replay ignores SetLOD on textures that were managed at
capture, and nothing said so. Now each LOD it ignores is a debug message (severity High, source
Unsupported Configuration, category State Setting, ID 0):
- a texture's nonzero LOD at the frame's start, from its initial contents, at event 0;
- each SetLOD in the frame to a nonzero LOD (clamped to the last level, as the GPU does), at the
  SetLOD's event. SetLOD(0) isn't reported, since it's what replay does anyway.

`ReplayCreate` records the objects it moves from the managed to the default pool, and only those
are reported: a D3D9Ex capture has no managed objects, and on a plain device nothing is ignored.

`D3D9_Texture_LOD`'s D3D9Ex check now reads the messages first and expects exactly the 12 the
demo's LODs give: 2D 2, cube 1, volume 3 and vertex 3 at event 0, the in-frame SetLOD(4), (3),
(1) and (5) at events 66 to 69, and the SetLOD back to 2, 1, 3 and 3 at events 128 to 131. Each
must name the setting and one texture, with its LOD. From the test's log:

```
Event 0: ResourceId::19 had LOD 2 when the frame started, which replay ignores: D3D9_ForceReplayOnExDevice is set, so this plain D3D9 capture replays on a D3D9Ex device, which puts D3DPOOL_MANAGED textures in the default pool, where SetLOD does nothing. Until its next SetLOD, draws sampling it read level 0 instead of level 2 as its most detailed. Turn the setting off to replay it as captured.
Event 66: SetLOD(4) on ResourceId::19 isn't replayed: D3D9_ForceReplayOnExDevice is set, so this plain D3D9 capture replays on a D3D9Ex device, which puts D3DPOOL_MANAGED textures in the default pool, where SetLOD does nothing. Until its next SetLOD, draws sampling it read level 0 instead of level 4 as its most detailed. Turn the setting off to replay it as captured.
** Each of the 12 LODs replay ignores has a debug message
```

The main check, on the plain device, still requires the capture to have no debug messages.

`ui/08_errors_and_warnings.png`: qrenderdoc with the setting on (set from its Python, and off
again before it closed). The status bar says "Capture has 12 issues", and Errors and Warnings
lists the 12 messages with the textures as links. The first try showed a different random number
in the ID column for each message: `DebugMessage::messageID` isn't initialised, so it's now set
to 0, which the test checks.

Builds and tests (`d3d9_step8_setlod/ignored_lod/`, x64 tests, as this is replay only):
all 4 builds at 0 warnings, the unit tests passed (6147 assertions), and:

```
## Tests complete summary: 18 passed out of 18 run from 209 total in 0:01:28.045826 ##
```

In the screenshot, event 66 is selected: the API Inspector shows it is `SetLOD(Texture 19, 4)`,
the call its message is about.

No negative check was run (the test's comparison is of the exact list, so a missing message
fails it).

## Not covered

- Recovering from a lost replay device, which isn't done.
- Derivatives: the test samples at explicit LODs. The probe covered them natively, and on the
  plain device the GPU does the sampling, so there's nothing of RenderDoc's in between.
- The texture viewer's LOD reset through custom display shaders: it's the same code as the plain
  display, which the test covers.
- Other GPUs, and games.
