# D3D9: edited disassembly keeps the original's constant names

Tested on the working tree on top of edac97a4b, which was committed as "D3D9: name an edited
disassembly's constants from the original's table". Machine: Windows 11, RTX 4070 Ti. Files are
in `d3d9_edit_ctab/`.

## Why

This is the second of the two shader items before step 5. Edit Disassembly, the default way to
edit a D3D9 shader, gives `D3DAssemble` the shader's disassembly. The constant table is only
comments there, so the assembled shader had none, and once it replaced the original, the
pipeline state, the constants and the shader viewer named its constants and samplers by register
(`c0`, `s0`) instead of `tint` and `tex`. The editor's header comment said so. On a real shader
with dozens of constants, that's every edit.

## The change

- **`D3D9Replay::ReplaceResource`** (`d3d9_shaderedit.cpp`): when the replacement is a shader
  built by `BuildTargetShader`, has no constant table, and the original has one, the
  replacement's `ShaderInfo` is read again from its bytecode with the original's `CTAB` comment
  inserted after the version token. Its reflection and disassembly then have the original's
  names. The shader the device created isn't changed: a comment does nothing in a shader, so it
  runs the same either way. An edit with a table of its own (HLSL, or bytecode with one) keeps
  it.
- The names go by register, so a register the edit uses for something else keeps the original's
  name. That's the case the user sees anyway: the edited disassembly starts with the original's
  table as a comment.
- **qrenderdoc** (`PipelineStateViewer.cpp`): Edit Disassembly's header comment said "a constant
  table below is lost and the constants are named by register". It now says the edit's constants
  are named from the original's table, by register.
- README, STATUS.md and `docs/how/how_edit_shader.rst` say the same.

## The test

`D3D9_Shader_Editing` (`util/test/tests/D3D9/D3D9_Shader_Editing.py`) checks two more things:
- **The round trip**: each of the capture's 10 shaders is disassembled and assembled. The result
  must have no constant table. After it replaces the original, its constant blocks (each
  variable's name and offset) and samplers (name and register) must be exactly the original's,
  and its instructions unchanged.
- **The 10 edits**: after each replaces the original, an edit with a table must have exactly the
  bytecode it was built with. An edit without one (the ps_3_0 assembly edit) must have every name
  from the original's table.

Fixing the test found an older bug in it. `shader_at` returned the D3D9 pipeline state's
`resourceId`, which the Python bindings give as a reference into the controller's pipeline
state, not a copy. It changed whenever the event did. So the round trip's list of originals was
keyed by IDs that drifted to the last event's shaders, and `ReplaceResource` replaced the wrong
shader. Before this change nothing in the round trip used the ID after the event moved, so it
didn't show. The first run with the new checks failed on 5 of the 10 shaders this way (for
example, vs_1_1 "getting" `rect` and `misc` from another shader's table), and
`run/debug_ids.py` showed the IDs drifting (`run/test_2_reference_ids.txt`, `run/debug_ids_output.txt`: the "==" lines' IDs are all 20 or 41, while each reflection's is its own).
`shader_at` now uses `PipeState.GetShader` and `GetShaderReflection`, which return the ID by
value. Its other use, checking that the pipeline state has the original's ID after a
replacement, now really compares two IDs. It passes.

`GetShader`'s reflection is a reference too. The round trip reads what it needs from the
assembled shader's reflection before replacing, since replacing changes it.

Results (`run/`):

| Check | Result |
|---|---|
| `renderdoc.sln` Development x64 and x86 | 0 warnings, 0 errors (`build_renderdoc_*.txt`) |
| `renderdoccmd test unit "[d3d9]"`, x64 and x86 | "All tests passed (7659 assertions in 15 test cases)" (`unit_x64.txt`, `unit_x86.txt`) |
| `D3D9_Shader_Editing`, x64 | passes (`test_3.txt`): 7 round trips with names, the ps_3_0 assembly edit with `['tint', 'tex']`, and the 7 edits with a table (6 HLSL, 1 bytecode) keeping their own. The other 2 edits are assembly for vs_1_1 and ps_1_1 shaders that have no table |
| Negative check: the `ReplaceResource` change taken out, x64 | fails with 8 problems (`test_negative.txt`): each of the 7 round trips has `c0`, `c1`, `s0`... instead of the original's names, and the assembly edit has none of `['tint', 'tex']` |
| `run_tests_local.py -t "^D3D9\|D3D11_D3DPERF"`, x64 | "18 passed out of 18 run" (`tests_x64.txt`) |

From `test_3.txt`:

```
** Frame Start's vs (vs_3_0): replacing the original, its assembled disassembly's constants are named ['flip', 'misc', 'rect'] from the original's table (as assembled: [])
** HLSL 2_0's ps (ps_2_0): replacing the original, its assembled disassembly's constants are named ['tint', 'tex'] from the original's table (as assembled: [])
** Shared ps_3_0, assembly: the edit has no constant table, replacing the original its constants are named ['tint', 'tex'] from the original's
** vs_3_0, HLSL: the edit keeps its own constant table, constants ['flip', 'misc', 'rect']
```

The 3 shaders without names in the round trip (SM1's vs_1_1 and ps_1_1, and ps_1_4) are
assembled by the demo, so they have no table to give. Their constants have to match the
original's too, and do.

The x86 automated tests weren't run, as this is replay-side code; the x86 unit tests were.

## The UI

In qrenderdoc, on `D3D9_Shader_Editing`'s capture, event 53 (the "HLSL 2_0" draw, Pixel Shader
24, ps_2_0), with Edit then Apply changes in the pipeline state (`ui/`):
- `01_before.png`: the original, its constants `tint` (0.70, 0.60, 0.46, 0.68) and `c1`, a
  literal.
- `02_editor.png`: Edit Disassembly, with the new header comment.
- `03_applied.png`: Apply changes. "Status: Edited Shader Active", and the API Inspector has
  "Pixel Shader 24 (Edited)".
- `04_constants_after.png`: the pipeline state shows "Pixel Shader 24 (Edited) (ps_2_0)". The
  constants window was closed and opened again from the pipeline state, and shows `tint` and
  `c1`. From the UI's Python, the bound reflection is the edit's (ID 1000000000000000201), with
  `tint`, `c1` and the sampler `tex`.
- `05_edited_view.png`: View on the edited shader. Its disassembly starts with the original's
  table (`tint c0`, `tex s0`).

An earlier try was on a build without the change (the negative check's build, not rebuilt). It
showed `tint` in the constants window too, because clicking Go on an open constants window only
raises it. So the window was closed and reopened for `04_constants_after.png`, and the UI's
Python confirmed the names. That try's screenshots were deleted.

## Not covered

- Edits that move constants to other registers: they keep the original's names, by register,
  which the comment at the top of the edited disassembly says.
- Shaders from real games' captures.
- A replacement used for two originals in turn: it keeps the first's table.
