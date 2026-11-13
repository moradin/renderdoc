# D3D9: the constant table against the compiler

Tested on the working tree on top of 12983db02, which was committed as "D3D9: read constant table
default values as the registers they fill". Machine: Windows 11, RTX 4070 Ti. Files are in
`d3d9_ctab_check/`.

## Why

This is the first of the two shader items before step 5 (shader debugging), which will rely on
the parser. STATUS.md listed as unchecked against fxc:
- some instruction and source modifier forms, `dcl_texcoord1_centroid`, and relative addressing in
  ps_3_0 inputs and vs_3_0 outputs;
- the constant table's default values, which were read "as the type's components packed
  together, as Wine does", integer and boolean defaults included.

The first list was already checked: `d3d9_phase3_report.md` found every one of those forms
identical to fxc, and step 4's assembly round trip covers every shader in the test captures.
STATUS.md hadn't been updated. That leaves the constant table, which this round checks against
Microsoft's own reader of it, D3DX9_43's `ID3DXConstantTable`.

## How

`probe/shaders.py` has 6 HLSL shaders whose uniforms all have initialisers and are all read, so
the compiler allocates them:
- **floats** (vs_3_0 and vs_2_0): `float` to `float4`, `float4x4` and `float3x2` in both orders,
  `float2x3`, arrays of scalars, vectors and matrices, a struct, an array of structs, a struct
  holding an array of structs, `int`, `int3`, `bool` and `bool2` the compiler puts in float
  registers, and one uniform with no initialiser;
- **intbool_ps** (ps_3_0): `int` and `int4` loop counts in integer registers, `bool`s and a
  `bool[3]` in boolean registers, a `float4`, and 2D, cube, volume and an array of 2D samplers;
- **intbool_vs** (vs_3_0 and vs_2_a): an `int` loop count, `bool` and `bool2` branch conditions,
  and arrays of `float4x3` in both orders;
- **sm2_ps** (ps_2_0): `float4`, `float3x3`, `float[2]` and a sampler;
- **legacy** ps_1_1, ps_1_4 and vs_1_1, built with D3DXCompileShader and
  `D3DXSHADER_USE_LEGACY_D3DX9_31_DLL`, since D3DCompile doesn't do ps_1_x;
- **partial** (vs_3_0): a `float4[4]`, a struct, a `float4x4` and a `bool[4]` of which only part
  is read, so the compiler allocates fewer registers than the type has.

That's 10 constant tables. Two probes read them:
- `probe/d3dx_probe.py` compiles them with D3DCompile (d3dcompiler_47) or the legacy compiler,
  then walks each table with `ID3DXConstantTable::GetConstant`, `GetConstantElement` and
  `GetConstantDesc`. That gives every constant, struct member and array element, with its
  register set, register, register count, class, type, rows, columns, elements, members, and the
  offset of its default value in the table. Output: `out/d3dx.json` and the `.bin` shaders.
- `probe/setdefaults_probe.py` creates a D3D9 HAL device, fills every float, integer and boolean
  constant with a sentinel, calls `ID3DXConstantTable::SetDefaults`, and reads the registers
  back. That gives the registers each table's defaults stand for. Output: `out/setdefaults.json`.

The default values were also read straight from the tables at the offsets D3DX gives, which is
how the layout below was worked out.

## What the compiler stores

Default values are stored as the registers' contents, not as the type's components packed
together:
- **Float and integer sets:** 4 DWORDs per register. A vector is padded with zeros, and a matrix
  takes a register per column (or row, for `row_major`), each padded. `float4x4 m44 = {1, 2, ...
  16}` is stored as `1 5 9 13 2 6 10 14 3 7 11 15 4 8 12 16`, and `float2x3 m23` as 3 registers:
  `61 64 0 0 | 62 65 0 0 | 63 66 0 0`. `float arr1[3] = {71, 72, 73}` is
  `71 0 0 0 | 72 0 0 0 | 73 0 0 0`.
- **An `int` in an integer register** is stored as a loop's: `int loops = 5` is `5 0 1 0`
  (count, start, step). An `int4` is stored as written.
- **An `int` or `bool` the compiler puts in float registers** is stored as a float: `bool fb =
  true` is 1.0f, `int3 fi3 = {-1, 2, -3}` is `-1.0 2.0 -3.0 0`.
- **The boolean set:** 1 DWORD per register, 0 or 0xFFFFFFFF. `bool flags[3] = {true, false,
  true}` is `ffffffff 00000000 ffffffff`, and `bool2 bb = {true, false}` takes 2 registers.
- **Every register the type covers is stored**, even past the ones the compiler allocated: the
  partial shader's `float4 part[4]` has a register count of 2 but all 16 DWORDs stored, and
  `bool bpart[4]` all 4. `SetDefaults` only writes the allocated registers.
- **Samplers** have no default.
- **The legacy compiler** stores them the same way.

From `out/setdefaults.json`, the floats shader's registers after `SetDefaults` (c0 to c12 are the
structs, c13 to c16 `m44`, c35 to c37 `arr1`, c42 onwards the scalars and vectors):

```
F 13  1 5 9 13          F 35  71 0 0 0          F 42  1.5 0 0 0
F 14  2 6 10 14         F 36  72 0 0 0          F 43  2 3 0 0
F 15  3 7 11 15         F 37  73 0 0 0          F 46  7 0 0 0       (int fi)
F 16  4 8 12 16                                 F 48  1 0 0 0       (bool fb)
```

and intbool_ps:

```
I 0  00000005 00000000 00000001 00000000        B 0  ffffffff
I 1  00000003 00000000 00000001 00000000        B 1  00000000
                                                B 2  ffffffff
```

`noinit`, which has no initialiser, has no default and keeps the sentinel.

## What RenderDoc did, and the fix

`ParseConstantTable` read as many DWORDs as the type has components, from the default's offset.
That's right for a single scalar or vector (the first DWORDs of its one register), but not for
anything spanning registers it doesn't fill: `arr1` read as `71 0 0`, and `float2x3 m23` as its
first column and part of the second (`61 64 0 0 62 65`).

Only `ShaderConstant::defaultValue` uses the parsed default, for a default of up to 64 bits. The
UI doesn't show it for D3D9 (qrenderdoc only reads it for enums in its buffer formatter, and
DXBC always leaves it 0). So the visible effect was small: a `float k[2]`, a struct of 2 scalars
or a `float1x2` published a wrong default.

Fixed in `d3d9_shader_reflect.cpp`:
- `CTABConstant::defaultValue` now holds the registers' contents as stored: the registers the
  whole type covers (`RegistersPerElement` times the elements), 4 DWORDs each in the float and
  integer sets and 1 in the boolean set, if they're all inside the table.
- The reflection publishes a default only for a scalar or a vector of 1 or 2 components, from
  those first DWORDs, which is the same in every set.

## The test

`d3d9_ctab_test_data.h` holds 7 of the tables, written by `probe/gen_header.py` from the probes'
output, with D3DX's description of every constant, member and element, and the registers
`SetDefaults` wrote. The other 3 (floats vs_2_0, intbool vs_2_a, legacy ps_1_4) only differ from
one of the 7 in their version token and target string. A new section of the `[d3d9]` unit tests,
"Compiled constant tables", puts each table in a shader with no instructions and checks that:
- each constant's name, register set, register, register count, class, type, rows, columns,
  elements and member count are D3DX's;
- each constant, member and allocated array element is in the reflection at D3DX's register,
  with D3DX's rows and columns, and matrices are row major only for `row_major`;
- each sampler is at D3DX's register, with D3DX's count;
- the registers our defaults fill, for the allocated registers, are exactly the ones
  `SetDefaults` wrote, with the same values;
- a scalar's or 2-component vector's published default is what `SetDefaults` wrote, and 0 without
  one;
- in the partial shader, `part`, `tpart`, `mpart` and `bpart` hold the defaults of their
  unallocated registers too.

The existing synthetic table test wrote a packed default for its `float`; it now writes a whole
register, as the compiler does.

Results (`run/`):

| Check | Result |
|---|---|
| `renderdoc.sln` Development x64 and x86 | 0 warnings, 0 errors (`build_renderdoc_*.txt`) |
| `renderdoccmd test unit "[d3d9]"`, all 10 tables in the header | "All tests passed (8672 assertions in 15 test cases)" (`unit_x64_all10_tables.txt`) |
| The same with the 7 committed tables, x64 and x86 | "All tests passed (7659 assertions in 15 test cases)" (`unit_x64.txt`, `unit_x86.txt`) |
| Negative check: the old packed reading put back, x64 | 2 failures: the synthetic test's `defaultValue.size() == 4` ("1 == 4"), and the new section's first table stops at `arr1`, "3 >= 12" (`unit_x64_negative_packed_defaults.txt`) |
| `run_tests_local.py -t "^D3D9\|D3D11_D3DPERF"`, x64 | "18 passed out of 18 run" (`tests_x64.txt`) |

So every part of the constant table the parser reads matches D3DX across the 10 tables, apart
from the default values, which now match too. The x86 automated tests weren't run, as this is
replay-side code; the x86 unit tests were.

## Not covered

- Tables from other compilers than these two (other fxc versions, Cg, hand-made tables in
  games). The D3DX reading is the reference, but a game's table could still be laid out
  differently.
- Arrays of arrays (`float a[2][3]`) and arrays of more than one dimension of structs.
- What D3D9 does with defaults: nothing, the runtime never applies them. Only programs that call
  `SetDefaults` themselves get them in registers, and the captured registers are what replay uses
  either way.
- The UI: nothing in it shows D3D9 default values, so there's no screenshot.
