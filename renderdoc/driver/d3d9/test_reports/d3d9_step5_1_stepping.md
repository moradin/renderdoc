# D3D9: the vertex shader interpreter can step

Step 5.1 of the shader debugging plan (STATUS.md, "Step 5 plan"). Tested on the working tree on
top of 6c2db4ee9, committed as "D3D9: let the vertex shader interpreter run one instruction at a
time". Machine: Windows 11, RTX 4070 Ti. Files are in `d3d9_step5_1_stepping/`.

## Why

Shader debugging (5.2 on) needs to run a vertex shader one instruction at a time. After each
instruction it needs to know which registers changed and what they held before. It also needs
each instruction's line in the disassembly, so the shader viewer can highlight it. Before this
change the interpreter could only run a vertex to the end (`VSEmulator::Execute`, used by VS
Out), and the disassembler didn't say where each instruction was. Nothing visible changes here.

## The change

- **`VSThread`** (`d3d9_vs_emulate.h`) is one vertex's run.
  - `Start` sets it up, `Step` runs one instruction, and `Run` runs to the end.
  - `Registers()` has the temporaries, `a0`, `p0`, the loop stack (with `aL`), the call stack and
    the outputs. Each call frame now has its `call` instruction and label, for 5.3's call stack.
  - `NextInstruction()` is the instruction that runs next.
- **`Step(&changes)`** appends a `VSRegisterChange` for each register the instruction wrote, with
  what it held before.
  - It covers temporaries, outputs, `a0` and `p0` (as integers), and `aL`. `aL` is recorded when
    a `loop` starts, steps or ends, not for `rep`.
  - A predicated write whose components are all off records nothing.
  - A failed instruction ends the thread with the error, changes nothing, and leaves
    `NextInstruction()` on it.
- **`Execute`** is now a thread's `Start` and `Run`. The old `ExecState` became `VSThread`, with
  its register state in a `VSRegisters` struct the debugger can read.
  - The instruction loop is a template on whether it records changes and whether it stops after
    one instruction. So the run to the end has nothing for stepping in its loop, and keeps the
    instruction pointer in a local as before.
  - `Write` is a template on recording too.
  - A new thread isn't reset twice, and `Execute` only sets `error` on failure, as before.
- **`Disassemble`** (`d3d9_bytecode.cpp`) and **`ShaderInfo::GetDisassembly`**
  (`d3d9_shader_reflect.cpp`) can return the line, from 1, each instruction is on. That's what a
  trace's `instInfo[].lineInfo.disassemblyLine` needs. `GetDisassembly`'s lines count the
  constant table comment above the instructions.

## The tests

Unit tests (`renderdoccmd test unit "[d3d9]"`):
- **Every interpreter test steps too.** The `[vsemu]` tests' `Runner` steps each shader as well
  as running it.
  - A shadow copy of the registers is updated only from the changes each step reports. After
    every step it has to equal the thread's registers, and each change's old value has to be
    what the shadow held. So a register changed without being reported, or reported with the
    wrong old value, fails.
  - The stepped run has to end with the same outputs, error and success as `Execute`, and a
    failed step must report no changes and stay on its instruction.
  - That covers every opcode, flow control (loops, `rep`, `break`, calls, predicates), relative
    addressing and `texldl` that the existing tests use.
- **A new "stepping" test case** checks five things:
  - the exact changes and old values of a short shader, including a predicated write that
    records nothing, `mova` and `setp`;
  - `aL`'s changes through a `loop` with a `rep` inside it;
  - the call stack's frames inside two nested calls;
  - a failure (calls nested too deeply) that leaves the registers and instruction as they were;
  - a thread on an emulator that failed to initialise.
- **Line numbers:** every disassembly the `[shader]` tests make is now made a second time with
  line numbers, and checked:
  - the text is the same;
  - there's one line per instruction, in order, after the version line;
  - each line has that instruction's opcode.

  The reflection test checks a vs_3_0 shader with a constant table: each instruction's line in
  `GetDisassembly` is the same text as in the plain disassembly, the first more than 20 lines
  further down.
- **A hidden speed test** (`[d3d9bench]`, not run by default) times `RunPostVS` on 200,000
  vertices of a 16-instruction vs_2_0 shader (`m4x4`, `m3x3`, `nrm`, a `loop` of 4 lights with
  relative addressing on `aL`).

Results (`run/`):

| Check | Result |
|---|---|
| `renderdoc.sln` Development x64 and x86 | 0 warnings, 0 errors (`build_renderdoc_*.txt`) |
| `renderdoccmd test unit "[d3d9]"`, x64 and x86 | "All tests passed (13187 assertions in 16 test cases)" (`unit_x64.txt`, `unit_x86.txt`). Before: 7659 assertions in 15 test cases |
| `run_tests_local.py -t "^D3D9\|D3D11_D3DPERF"`, x64 | "18 passed out of 18 run" (`tests_x64.txt`). VS Out in `D3D9_Mesh_Output` still matches its GPU and `ProcessVertices` references |

The x86 automated tests weren't run, as this is replay-side code; the x86 unit tests were.

## Speed

The plan asked that mesh output on a big draw be no slower. Two measurements (`benchmark/`):

- **A/B in one binary.** The old `Execute` was copied into the file under another name for the
  measurement only (`old_execute.cpp.txt`), with a test that runs both on the same 200,000
  vertices (`ab_test.cpp.txt`), best of 7.
- **`RunPostVS` before and after**, built and run back to back (`run/bench_old_vs_new_development.txt`).

| Build | Old | New | |
|---|---|---|---|
| Release x64, A/B, loop of 4 | 176.7-181.2 ms | 168.1-170.9 ms | about 5% faster |
| Release x64, A/B, loop of 64 | 1843.7-1844.3 ms | 1754.4-1761.5 ms | about 5% faster |
| Development x64, A/B, loop of 4 | 780.4-787.0 ms | 813.4-826.0 ms | about 4% slower |
| Development x64, `RunPostVS` | 977.8-983.0 ms | 1041.7-1043.4 ms | about 6% slower |

Release is what users run, and it's faster. Development is slower, because it compiles with
`/Od /JMC` and nothing is inlined, so every extra call counts: `Start`, `Run` and the loop's own
function. The first version called `Step` once per instruction and was 15% slower in
Development. `ab_development_x64.txt` lists each version tried and what it gained.

Release has no unit tests (`ENABLE_UNIT_TESTS` is `RDOC_DEVEL`), so `common/globalconfig.h` was
changed to turn them on for the Release A/B build only. It was reverted, Release rebuilt, and it's
not part of the commit. The A/B code isn't committed either. The `[d3d9bench]` test is.

## Not covered

- Nothing uses stepping yet: `DebugVertex` is 5.2. Until then, the unit tests are the only
  check.
- The line map isn't checked in qrenderdoc yet, since no trace uses it. 5.2 and 5.3's UI
  screenshots will show it.
- Development builds' mesh output is about 6% slower, as measured above.
- The unit tests ran on Windows only, not on Linux with AddressSanitizer as phase 3e's did.
