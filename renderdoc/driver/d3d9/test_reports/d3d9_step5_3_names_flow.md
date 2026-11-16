# D3D9: constant names, call stacks and flags in the vertex shader debugger

Step 5.3 of the shader debugging plan (STATUS.md, "Step 5 plan"). Tested on the working tree on
top of 62fb29974, committed as "qrenderdoc: show constant values as values, not as block
bindings" and "D3D9: name constants, show call stacks and flag samples in the VS debugger", tested
together. Machine: Windows 11, RTX 4070 Ti. Files are in `d3d9_step5_3_names_flow/`.

## Why

5.2's debugger named every constant by register. An HLSL shader's constant table says what each
register is, and that's what a user looks for: `world`, `lights[1].dir`, not `c4` or `c12`. The
viewer's Callstack panel and its "Run to Sample" and "Run to NaN/Inf" commands were also empty or
did nothing for D3D9, since the trace had no call stacks or flags.

## The change

- **Constants named from the constant table** (`D3D9Emu::VSDebugger::Begin`, `ConstantNamer` in
  `d3d9_vs_debug.cpp`):
  - each scalar, vector and matrix in each constant's type gets a source variable, with struct
    members and array elements spelled out (`lights[1].dir`, `o.i[1].q`), on the registers the
    compiler allocated the constant (the table's `registerCount`, which `ShaderInfo` now passes to
    the debugger with the reflection);
  - matrices take a register per row (row_major) or column (column_major), in the float and
    integer sets; in the boolean set every component is a register;
  - a constant the compiler allocated only part of is cut to that part, since the registers after
    it can belong to something else: a column_major float4x4 given two registers is shown as the
    float4x2 the shader reads, and elements or members with no register aren't shown.
  - Those registers are listed again under "Registers" (`Registers.c4`). The viewer looks up a
    register clicked, hovered or watched in the disassembly (`c4`) among top-level entries'
    children as well, so the disassembly's names still resolve.
  - The shader's `def`, `defi` and `defb` registers are under "Literals" (`Literals.c16`).
  - Any other register keeps its disassembly name (`c12`), as in 5.2.
  - `ConstantSetFor` and `ConstantRegistersPerElement` are now public in `d3d9_shader_reflect.h`,
    so the debugger lays out the registers as the reflection does.
- **Call stacks:** each state's `callstack` is "main", then the label (`l1`) of each subroutine
  call the next instruction is in, from the interpreter's call frames.
- **Flags:** `SampleLoadGather` on the state after a `texldl`, and `GeneratedNanOrInf` on the state
  after an instruction that wrote a NaN or infinity to a component of its write mask (as D3D11
  does, so an instruction carrying an infinity on is flagged too).
- **qrenderdoc (`ShaderViewer.cpp`), found in the screenshots:** the viewer read the value of every
  constant a source variable points at as a binding. A TRUE boolean register is (1, 0, 0, 0),
  which reads as "constant block 0", so every true `b#` showed as "VS float constants (c#)"
  (`03a_calls_before_constant_fix.png`); in 5.2 too, though no screenshot showed one. An integer
  register like a one-pass loop's (1, 0, 1, 0) would as well, and a value with w = 1 lost its
  component as a "direct access". Now only a reference to a whole constant block, or an element
  of an array of them, is looked up as a binding. Upstream has the same code, so integer or
  boolean constant buffer values in other APIs could show it too; their UI wasn't rechecked.

## Tests

### Unit tests (`[vsdebug]`)

New sections in "D3D9 vertex shader debugging":
- **Compiled constant tables:** the vs_3_0 tables in `d3d9_ctab_test_data.h` (`floats_vs3`,
  `intbool_vs3`, `partial_vs3`, from D3DCompile, with what D3DX9_43's `GetConstantDesc` says), each
  put in a shader that also has a `def c95` and reads `c90`. For every scalar, vector and matrix
  D3DX lists (every struct member and array element), the trace has a source variable of that
  name exactly when D3DX gives it registers, on exactly those registers, with rows x columns
  components and the set's type, and each register is also under "Registers". `c90` is named by
  register and `c95` is under "Literals". By hand: `m44` (column_major, c13) row 1 column 2 is
  c15.y, `rm44` (row_major, c17) is c18.z, `m23` (float2x3) is c34.y, `sarr[1].c` row 1 column 0
  is c6.y, `bones[1]` (float4x3[4], column_major) is 4x3 on c3-c5, `rbones[1]` on c16-c19, `bb`
  (bool2) is b0 and b1, `count` is i0.x and an integer, `mpart` (a float4x4 given one register)
  is 4x1 on c3, and `tpart.b` and `part[2]`, given no register, aren't there.
- **Call stacks and flags:** a vs_3_0 shader with `rep i0` around `call l1`, which calls `l2`, a
  `texldl`, `rcp r2.x, c0.x` with c0.x = 0, and `mul r2.y, c0.x, c0.x`. Every state's call stack
  matches the subroutine its next instruction is in (`main`, `main/l1`, `main/l1/l2`), the calls
  run 4 times, only the state after the `texldl` is a sample and only the one after the `rcp` is a
  NaN/Inf (0 * 0 isn't), and r2.x ends infinite.
- The failing-call test checks the last state's call stack is the 16 calls that ran.
- 5.2's vs_3_0 test now expects `def c10` and `defi i0` under "Literals".

### The zoo

`D3D9_Shader_Debug_Zoo` has two new draws (the grid is 4x2 now):

| Draw | Shader | What it exercises |
|---|---|---|
| vs_2_x calls | assembly | `rep i1` around `call l1`, which calls `l3`, then `callnz l2, b0` |
| vs_3_0 names | HLSL | a constant table with a column_major `wvp`, a row_major float4x3 `world`, `Light lights[2]` (float4, float3 and float members), a float4x4 `texMat` of which the shader reads two columns, a float `zero` and `bool flags[2]`; `min(1 / zero, 4)` makes an infinity. The compiler turned its loop into `rep i0` |

The test adds, for every draw (`check_flow`):
- each state's call stack starts with `main` and ends with the subroutine its next instruction is
  in, read from the disassembly's `label` lines, and the last state's is `main`;
- the call stacks each draw's first vertex goes through: vs_2_x `main`, `main/l0`; vs_2_x calls
  `main`, `main/l1`, `main/l1/l3`, `main/l2`;
- a state is flagged as a sample exactly when the instruction before it was a `texldl` (1 step in
  vs_3_0, none elsewhere), and as a NaN/Inf exactly when that instruction wrote a NaN or infinity
  to a component its disassembly's destination mask names (2 steps in vs_3_0 names: `rcp r0.z,
  c18.x` and `mul r0.z, r0.z, c16.x`; none elsewhere).

And for the names draw (`check_names`), the names, shapes and registers: `wvp` 4x4 from c0.x to
c3.w, `world` 4x3 from c4.x to c7.z, `lights[0].colour` on c8, `lights[0].dir` on c9.xyz,
`lights[1].range` on c13.x, `texMat` 4x2 from c14.x to c15.w, `zero` on c18.x, `flags[1]` a bool
on b1; each register also under "Registers"; values through the names are the application's
(`world`'s translation x 0.05, `lights[0].dir.y` 0.8, `lights[1].range` 1.5, `texMat` row 3 column
1 0.2); no `lights[2]...`, `lights`, `Light` or `flags`; the compiler's literals (c16, c17, i0) are
under "Literals" and not by register as well. The vs_1_1, vs_2_x and vs_3_0 checks from 5.2 now
accept c0-c3 under "Registers" when the table names them. The disassembly and source variables of
the names draw are in `run/names_draw_disassembly_and_sourcevars.txt`.

## Results

From the final run (`run/`, `verify_summary.txt`):

| Check | Result |
|---|---|
| `renderdoc.sln` and `demos.sln`, Development x64 and x86 | 0 warnings, 0 errors (`build_*.txt`) |
| `renderdoccmd test unit "[d3d9]"`, x64 and x86 | "All tests passed (14831 assertions in 17 test cases)" (`unit_x64.txt`, `unit_x86.txt`). After 5.2: 13694 in 17 |
| `run_tests_local.py -t "^D3D9\|D3D11_D3DPERF"`, x64 | "19 passed out of 19 run" (`tests_x64.txt`) |
| `[d3d9bench]`, Development x64 | `RunPostVS` on 200,000 vertices: 1035.4, 1034.7, 1046.4 ms, and in a rerun 1156.0, 1099.4, 1140.6 ms (`bench_development_x64*.txt`). 5.2: 1009.7-1012.2 ms. The interpreter and mesh output code (`d3d9_vs_emulate.*`) aren't changed by this, so the spread is the machine: other programs were running this time, and two runs minutes apart differ by 10% |

`D3D9_Shader_Debug_Zoo` (`tests_x64.txt`): every vertex of the seven shader draws ends with VS
Out's values (21 vertices; the new draws take 30 and 71 steps), and:

```
vs_1_1: call stacks [['main']], 0 sample(s), 0 NaN or infinity
vs_2_0: call stacks [['main']], 0 sample(s), 0 NaN or infinity
vs_2_x: call stacks [['main'], ['main', 'l0']], 0 sample(s), 0 NaN or infinity
vs_3_0: call stacks [['main']], 1 sample(s), 0 NaN or infinity
vs_3_0 instanced: call stacks [['main']], 0 sample(s), 0 NaN or infinity
vs_2_x calls: call stacks [['main'], ['main', 'l1'], ['main', 'l1', 'l3'], ['main', 'l2']], 0 sample(s), 0 NaN or infinity
vs_3_0 names: call stacks [['main']], 2 NaN or infinity
The names draw's constants are named: ['flags[1]', 'lights[0].colour', 'lights[0].dir', 'lights[1].range', 'texMat', 'world', 'wvp', 'zero'], literals ['Literals.c16', 'Literals.c17', 'Literals.i0']
```

The first test run failed on the names draw: the driver flagged `mul r0.z, r0.z, c16.x` (infinity
times 1), the test then only counted components that became a NaN or infinity. D3D11 flags any
instruction whose result is one, which is more useful for "Run to NaN/Inf", so the test now reads
the destination mask from the disassembly instead.

## The UI

Screenshots of qrenderdoc on a capture of the zoo (`screenshots/`), each `PrintWindow` of
qrenderdoc's own window:

1. `01_names_start.png`: vertex 1 of the names draw, at its first instruction. The constants list
   has `lights`, `texMat`, `world`, `wvp`, `zero` and `Literals` (the boolean names and
   "Registers" are further down the list); the Callstack panel says `main`.
2. `02_names_watch_inf.png`: at step 65, after `rcp r0.z, c18.x`, on the `mul` that carries the
   infinity on. Watches resolve `lights[1].dir` (float3 (0, 0.6, 0.8) in c[12].xyz), `world`
   (float4x3), `texMat` (float4x2), `c4` (through "Registers"), `c16` (through "Literals"), `i0`,
   `flags[1]` and `b1` (1), and r0 is (0.70, 0.70, INF, 1.00). The watch window calls both
   matrices "(row_major)": qrenderdoc labels every matrix it evaluates that way, for any API.
3. `03_calls_callstack.png`: vertex 0 of the vs_2_x calls draw at step 13, on `add r0.x, r0.x,
   v1.x` in `l3`. The Callstack panel shows `l3`, `l1`, `main`, and `b0` is `true`.
   `03a_calls_before_constant_fix.png` is the same before the qrenderdoc fix: `b0` shows
   "VS float constants (c#)".

How they were taken: the first attempt drove the mouse, and another application came to the front
between bringing qrenderdoc forward and clicking, so the clicks landed on it (an empty area) and
that screenshot showed it; it was deleted. After that nothing sent mouse or keyboard input:
a script inside qrenderdoc (`--ui-python`) opened the debugger with `DebugVertex` and
`IShaderViewer`, moved it with `SetCurrentStep`, and added watches with `AddWatch`, all on the UI
thread, and the screenshots are `PrintWindow` captures of qrenderdoc alone. A first version of
that script called the viewer from its own thread, and qrenderdoc exited after the watches; on
the UI thread the same steps run without trouble. So "Debug this Vertex" from the menu (shown in
5.2), the "Run to Sample" and "Run to NaN/Inf" menu commands, and hovering weren't used this time:
the flags they stop on are checked by the tests, and the watches use the same lookup as hovering.

## Not covered

- The menu commands and hovering, as above. Expanding the constants list's groups needs a click,
  so the screenshots show them collapsed; the test checks their contents.
- Other APIs' shader viewers after the qrenderdoc change (only values inside a constant block are
  shown differently).
- A `def` on a register the constant table also names shows under the table's name only.
- The unit tests ran on Windows only, not on Linux with AddressSanitizer.
- The x86 automated tests weren't run, as this is replay-side code (the x86 builds and unit tests
  were).
