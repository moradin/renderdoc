# D3D9 step 8: every D24 depth level written back

Tested on the working tree on top of 1fee69a32, which was committed as "D3D9: read and write
depth stencils by each depth's level". Machine: Windows 11, RTX 4070 Ti (driver 32.0.16.1088).
Files are in `d3d9_step8_d24_levels/`.

## What changed

Depth stencils that can't be locked are read at capture with the depth test, a bisection that
finds for each pixel the largest integer l whose depth(l) passes ZFUNC LESSEQUAL, and replay
writes depth(l) back with `oDepth` (`d3d9_depth.cpp`). depth(l) was l * fp32(1 / max). Because
fp32(1 / 16777215) rounds up, that formula never lands on two of D24's levels, so a depth stored
at one of them was written back one level lower and failed a ZFUNC EQUAL test at replay. One of
them is the level a depth of 0.375 is stored at, which is how the gap was found.

depth(l) is now (l + (l >= 2^(n-1) ? 1 : 0)) * 2^-n for n depth bits. Every step of that is
exact in fp32, and on this GPU it lands on each of the 2^24 D24 levels (and 2^16 D16 ones) exactly
once, in order. So every level is written back, and the integer stored is the level's index,
which is also what the texture viewer shows (0.375 now reads 0x600000, the level it's stored at,
where it read 0x5FFFFF).

The capture version goes to 0x3, since the stored integers mean something else now. Replaying a
capture from before 0x3 writes its depths back with the old formula, as before.

## What the GPU does

Native first: a probe (`probe/d3d9_depth_probe.cpp`, not committed as a demo) uses a depth stencil
with one pixel per level (4096x4096 for 24 bits, 256x256 for 16), so a single draw tests every
level. For each formula it checks:
- **shader:** the depth the shader computes, written to an R32F target, against the same formula
  on the CPU;
- **chain:** F(l) written at pixel l with ZFUNC ALWAYS, then F(l + 1) drawn with ZFUNC GREATER.
  Where that fails, F(l) and F(l + 1) land on the same level. With 2^n integers and 2^n levels,
  no failures means each l has its own level, in order, so l is the level's index;
- **draws:** 2^n random fp32 depths in [0, 1] drawn with `oDepth` (round numbers, any fp32 bit
  pattern, and depths within 2 ulps of a level's centre or edge), read back by bisection with
  the formula, then F(l) drawn with ZFUNC EQUAL. A failure is a depth replay can't write back;
- **clears:** the same for 65,536 one-pixel Clears with the first 65,536 of those depths.

The formulas: A, the old l * fp32(1 / max); B, the new (l + [l >= 2^(n-1)]) * 2^-n; and C,
l * 2^-n. `probe/probe.log` has the run. For D24S8 (D24X8 and INTZ were the same, line for line):

| | A (old) | B (new) | C |
|---|---|---|---|
| shader against the CPU | 0 of 16,777,216 differ | 0 differ | 0 differ |
| chain: F(l + 1) not above F(l) | 2 (l = 0x7FFFFF and 0xFFFFFE) | **0** | 1 (l = 0x800000) |
| draws not EQUAL after reading back | 1 (0.375) | **0 of 16,777,216** | 2 (1.0) |
| clears not EQUAL after reading back | 1 (0.375) | **0 of 65,536** | 1 (1.0) |
| 0.375 reads | 0x5FFFFF | 0x600000 | 0x600000 |
| 0.75 reads | 0xBFFFFF | 0xBFFFFF | 0xC00000 |

A drawn at every l and read back with B (whose l is the level) shows what A did: it lands on l
for 10,485,761 levels and on l + 1 for 6,291,455, and never on two levels:

```
level 6291456 (600000) is never reached by A; B's depth for it 0.375 (3ec00000)
level 12582912 (c00000) is never reached by A; B's depth for it 0.75000006 (3f400001)
```

So A's integers are one below the level for 37.5% of D24's levels (this was "about 70% of depths
above 0.4" in STATUS.md), and a depth stored at either of those two levels (by a Clear, or a draw
with a depth A doesn't produce) comes back one level lower. Why: fp32(1 / 16777215) is 2^-24 (1 + 2^-23), a
rounding up of 2^-24 (1 + 2^-24 + ...), so l * it overshoots by up to one level, and at 0x600000
and 0xC00000 it overshoots enough to skip one.

B is exact: l and the 1 are integers below 2^24 + 1, and 2^-n is a power of two. Its idea is
that the GPU converts a depth d to the level nearest d * (2^n - 1): l * 2^-n lands on
l - l * 2^-n, which rounds to l while l is below half of 2^n, and to l - 1 above it, so B adds one
there. That is only a guide: the GPU doesn't round d * (2^n - 1) exactly to the nearest, as
40,376 of the 16,777,216 drawn depths and 160 of the 65,536 cleared ones landed on a neighbouring
level of the one that model predicts. The chain is what shows B reaches every level.

For D16, A and B both pass everything (0 chain failures, 0 failed draws and clears), and give the
same integers, since 1 / 65535 in fp32 is close enough. 0.5 reads 0x7FFF with both (the GPU
rounds that tie down). C misses 1.0 there too. D24X4S4, D32 and D15S1 aren't supported on this
GPU.

## The tests

`D3D9_Capture_Edge_Cases` has a new "Depth levels" marker:
- before the first frame, a 64x64 D24S8 depth stencil is cleared a pixel at a time to 4096
  depths: 0.375, 0.75000006 (0x3F400001, the depth for level 0xC00000), 0, 1, 0.25, 0.5, 0.75 and
  0.33333334, then B's depth for random levels, A's depth for random l (which land between B's),
  and random fp32 bit patterns in [0, 1];
- in the frame, a draw outputs the same depths with `oDepth` and ZFUNC EQUAL, without writing
  depth, green over a red clear. Natively all 4096 pass, which the demo checks and logs;
- the test checks the first 8 values the capture read (0x60000000, 0xC0000000, 0, 0xFFFFFF00,
  0x40000000, 0x80000000, 0xBFFFFF00, 0x55555500; these depths aren't near the edge of a level,
  so their level is certain), and that the draw's target is green everywhere at replay, so every
  depth was written back to its level.

The INTZ texture's expected value, cleared to 0.375, goes from 0x5FFFFF0F to 0x6000000F.

The new resources are made after the existing ones, which keeps the resource names the test
looks up. Formatting the demo with clang-format 15 also rewrapped some lines that hadn't been
formatted before.

## Results

### Automated tests

All 17 passed, x64 and x86 (`run_summary.txt`, `run_x64.txt`, `run_x86.txt`):

```
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:20.337396 ##
## Tests complete summary: 17 passed out of 17 run from 208 total in 0:01:18.989408 ##
```

`D3D9_Capture_Edge_Cases`, in both:

```
Log: All 4096 depths passed ZFUNC EQUAL natively
** The INTZ depth texture is as expected
** The depth levels' first 8 values are each depth's level
** The depth levels tested with ZFUNC EQUAL is as expected
```

The automatic depth stencil (0.25 and 0.75), the multisampled one (0.5) and D16 (0.5) read the
same as before. Both solutions built with 0 warnings and 0 errors for x64 and x86, and the unit
tests passed (`unit.txt`, 6147 assertions in 15 test cases).

### The old driver

With the driver from 1fee69a32 and the new demo and test (`run_old_driver.txt`), the test fails
at INTZ, before it gets to the new draw:

```
!+ FAILURE in D3D9_Capture_Edge_Cases: The INTZ depth texture: 1024 texels are wrong, the first at 0,0 is 0x5FFFFF0F, expected 0x6000000F
```

A capture the old driver made, replayed by the old driver (`levels_check.py`,
`old_capture_old_driver.txt`), leaves exactly 0.375's and 0.75000006's pixels red:

```
first 8 values at the start of the frame: 0x5FFFFF00 0xBFFFFF00 0x00000000 0xFFFFFF00 0x40000000 0x80000000 0xBFFFFF00 0x55555500
not green after the ZFUNC EQUAL draw: 2 of 4096 [0, 1]
```

### Older captures

That capture (version 0x2) replayed by the new driver (`old_capture_new_driver.txt`) gives the
same output, line for line: its integers are written back with the old formula, as before. Had
they been written with the new one, the probe's numbers say 37.5% of the levels would come back
one lower; that wasn't tried at replay. (The values printed are read with the new formula, so
they're the levels the old formula wrote, which is why 0.375 shows 0x5FFFFF.)

## UI

The demo captured with the new build, in qrenderdoc at the "Depth levels" draw (event 68):
- **`ui_1_depth_levels.png`:** the texture viewer on the 64x64 depth stencil (Depth Stencil 34),
  with the draw's render target in the outputs, green all over;
- **`ui_2_pick_0375.png`, `ui_3_pick_075.png`:** the status bar after picking pixels (0, 0) and
  (1, 0): "Depth 0.375, Stencil 0x00" and "Depth 0.75, Stencil 0x00". The texture viewer rounds
  the depth it shows, so the exact levels are checked by the test instead.

The debug message count was 0.

## Not covered

- Other GPUs. The formula leans on how this GPU turns a depth into a level (near enough rounding
  `d * (2^n - 1)` to the nearest), which the D3D9 documentation doesn't pin down.
- D15S1, D24X4S4 and D32, which this GPU doesn't support, so the formula for 15 bits and D32's
  24 is untested. D24FS8 and `D3DMULTISAMPLE_NONMASKABLE` still can't be read at all.
- Multisampled depth stencils go through the same shaders, a sample at a time; the 4x MSAA one in
  `D3D9_Capture_Edge_Cases` passed, but it only holds 0.5 and 1.0.
- Games.
