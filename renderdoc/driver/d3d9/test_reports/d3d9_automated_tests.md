# D3D9 automated tests: first run

Step 1 of the plan: the D3D9 check scripts are now tests in upstream's framework, so
`run_tests.py` runs each demo, captures it and checks the capture. All 11 tests pass (10 D3D9 ones
and `D3D11_D3DPERF_Markers`), in 53 seconds, and upstream's D3D11 tests are unaffected.

- **What I built:** the commit that adds the tests, on top of 07d4c5f00. `renderdoc.sln` and
  `util\test\demos.sln`, Development x64, with 0 errors and 0 warnings, and the python module
  for Python 3.14 with `util\test\build_pymodule_py3.ps1`.
- **Machine:** Windows 11, RTX 4070 Ti (driver 610.88).
- **How:** `python util\test\run_tests_local.py -t "^D3D9|D3D11_D3DPERF"`.
- **Unit tests:** `renderdoccmd test unit "[d3d9]"`: all 13 test cases pass (6057 assertions).

## What was added

- **`util/test/tests/D3D9/`:** one test per D3D9 demo, and `tests/D3D11/D3D11_D3DPERF_Markers.py`.
  STATUS.md's "Automated tests" lists what each one checks.
- **`util/test/rdtest/shared/D3D9_Test.py`:** their base class. It:
  - fails a test whose demo logged a TEST_ERROR, natively or under RenderDoc;
  - captures the demo itself, letting it run for 2 seconds after the capture, so that checks the
    demos make in the frames after a captured one reach the log;
  - runs the demo natively first where a test needs the GPU's own results;
  - runs the standalone scripts' checks on the open capture, with their output in the test log.
- **The check scripts:** each has a `check_capture(controller, ...)` that the tests call, and
  `main()` still opens a capture for standalone use. They still run inside qrenderdoc's Python 3.6
  (checked with the mesh, D3DPERF, pipeline state, overlay and replay scripts). Two now fail where
  they used to only print: the mesh check counts a draw with no reference as a failure, and the
  D3DPERF check compares the marker colours.
- **The demos:** `D3D9_Texture_Formats` and `D3D9_Mesh_Output` write their references to their
  tmp folder (`--tmp`) instead of next to the executable, so test runs don't write into the source
  tree. `D3D9_Mesh_Output` now writes `_reference_rdoc.txt` under RenderDoc, as the formats demo
  does, rather than overwriting the native reference.
- **`util/test/build_pymodule_py3.ps1` and `util/test/run_tests_local.py`:** run the framework on
  this machine (see STATUS.md).

## Results

| Test | Result | Main numbers |
|---|---|---|
| D3D9_Simple_Triangle | PASS | 3 triangles; the PS constants one at 96, 191, 0; SaveTexture alpha 255-255; length 0 constants matched at every draw; 11 actions replayed; export round trip identical |
| D3D9_Frame_Start_State | PASS | AMBIENT 0x00102030, STENCILREF 0x5A; VS c10, c200 and PS c20, c180 as set, c138 and c148 zero |
| D3D9_Texture_Formats | PASS | 31 formats passed, 0 failed; replay render target passed; the `_rdoc` reference is byte-identical to the native one (293,903 bytes) |
| D3D9_Mesh_Output | PASS | 18 passed, 0 failed, 0 without a reference; the `_rdoc` reference is byte-identical to the native one (51,443 bytes) |
| D3D9_Overlay_Test | PASS | 101 checks passed, 0 failed |
| D3D9_Resource_Contents | PASS | every level, face and slice texel by texel at the start of the frame; every alternating region had the previous frame's 0xFF00FF00; the recycled texture 0xFF040404; the write-only index buffer read 0, 1, 2 |
| D3D9_Capture_Edge_Cases | PASS | automatic depth stencil 0x4000005A / 0xBFFFFFA5 / 0xFFFFFF00; each of 4 samples of the multisampled one; D16 0x7FFF; INTZ 0x5FFFFF0F; locked and GDI surfaces |
| D3D9_Reset_Capture | PASS | 2 captures: the in-app one has only "After the reset" with a blue clear; the triggered one (frame 61) only "Frame" with a red clear |
| D3D9_Wrapping_Check | PASS | demo log clean, 2 actions replayed |
| D3D9Ex_Wrapping_Check | PASS | demo log clean, 2 actions replayed |
| D3D11_D3DPERF_Markers | PASS | markers nest as drawn, with their colours |

The numbers come from RenderDoc's replay, except the two references, which the GPU wrote natively.
No demo logged a warning.

The full output of the final run is `d3d9_automated_tests/run_tests_output.txt`.

### Upstream's D3D11 tests

The framework change (one import in `rdtest/__init__.py`) could affect every test, so I also ran
`-t "^D3D(9|9Ex|11)_"`: 44 tests, 43 passed in 13 minutes. That's the 11 above and 33 of upstream's
D3D11 tests, of which 32 passed. The one that failed is `D3D11_Shader_ISA`:

```
!+ FAILURE in D3D11_Shader_ISA: AMDIL is not an available disassembly target. Are you missing plugins?
```

That's this machine, not the change: AMDIL disassembly of DXBC needs `atidxx64.dll`, which comes
with AMD's display driver (`CheckForSupport` in `amd_isa.cpp`), and this is an NVIDIA machine. It
still failed with the installed RenderDoc's `plugins\amd` copied next to the build (removed again
afterwards). The fork's `amd_isa.cpp` change only drops D3D9's targets. `D3D11_AMD_Shader_Extensions`
was skipped ("Couldn't load AGS dll"), as upstream's runner does without AMD's plugins.

## Found while writing the tests

Neither of these is a D3D9 bug; both were in my first versions of the tests:

- `GetTextureData` on a volume texture returns the whole mip, every depth slice, whatever slice it's
  given. D3D11 does the same. The test reads slice z from that.
- An in-app capture (`StartFrameCapture`/`EndFrameCapture`) ends before the Present, so its last
  action is the draw, which has no copy destination. Its `frameNumber` is 0xFFFFFFFF.

I also checked that the demo log check fails a test: a log with one `Error:` line raised a test
failure naming it.

## Not covered

- The UI: anything in qrenderdoc still needs checking by hand.
- Comparing disassembly with fxc's (the unit tests cover the disassembler).
- `GetFrontBufferData`'s surface and the recorded state block's contents.
- Remote servers: the base class's capture doesn't go through one.
- 32-bit builds (step 2).
