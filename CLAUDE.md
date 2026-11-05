# Working on this fork

This fork adds D3D9 and D3D9Ex capture and replay to RenderDoc. Start each session by reading:

- [README.md](README.md): what works, the known limitations, and the changes made outside the D3D9
  driver.
- [renderdoc/driver/d3d9/STATUS.md](renderdoc/driver/d3d9/STATUS.md):
  - how each part works and how it was tested;
  - what still needs testing;
  - the code layout (at the end);
  - the remaining work, in order, under "Remaining work".
- The latest reports in [renderdoc/driver/d3d9/test_reports](renderdoc/driver/d3d9/test_reports),
  for how earlier rounds were run.

Until now the code was written in a Linux cloud session that couldn't build or run anything for
Windows. A local agent on the Windows machine tested it. From here on, one local agent does both
the coding and the testing.

## Git

- **Branch:** work on `claude/renderdoc-directx9-support-tix3lw`.
- **Commits:**
  - Make one commit per piece of work.
  - Use the subject style `D3D9: <what changed>`. Use another prefix for changes elsewhere, such as
    `qrenderdoc:` or `core:`.
  - Explain why in the body.
  - Don't put model names or IDs in commits, code or docs.
- **Test before you commit.** A change is committed with its test results. STATUS.md says how it
  was tested and records the results.
- **Push:** `git push -u origin claude/renderdoc-directx9-support-tix3lw`.
- **Master:** fast-forward it (`git push origin HEAD:master`) once a change has passed its tests on
  this machine. Never rewrite master's history.
- **Pull requests:** don't open any unless asked.

## Building

- **Main solution:** `renderdoc.sln`, Development x64. D3D9 is part of `renderdoc.dll`, and the
  project is `renderdoc/driver/d3d9/renderdoc_d3d9.vcxproj`.
- **Demos:** `util/test/demos.sln`, Development x64. This builds `util/test/demos_x64.exe`.
  - The D3D9 demos are in `util/test/demos/d3d9/`.
  - `D3D11_D3DPERF_Markers` is in `util/test/demos/d3d11/`.
- **Formatting:** format changed C++ files with clang-format 15. Upstream's CI checks this with
  `util/clang_format_all.sh`.
- **Builds must stay clean:** keep both solutions at 0 errors and 0 warnings.
- **Unit tests:** run `x64\Development\renderdoccmd.exe test unit "[d3d9]"` for the shader
  bytecode, reflection and vertex emulation tests. These are Catch tests in `d3d9_*.cpp`.

## Testing

The machine is Windows 11 with an RTX 4070 Ti.

1. **Run the automated tests** after every change, with both solutions built:
   ```
   powershell -File util\test\build_pymodule_py3.ps1
   python util\test\run_tests_local.py -t "^D3D9|D3D11_D3DPERF"
   ```
   - They run each demo (natively first where a test needs the GPU's reference), capture it, and
     check the capture. STATUS.md's "Automated tests" lists what each one checks.
   - `build_pymodule_py3.ps1` builds the python module against the installed Python 3.14, since
     `run_tests.py` needs 3.7 or later and the build's own module is for qrenderdoc's bundled 3.6.
     Rebuild it when renderdoc's public API changes.
   - `run_tests_local.py` stops `run_tests.py` asking to register the Vulkan layer, which the
     installed RenderDoc in Program Files owns. D3D9 and D3D11 tests don't use the layer.
   - Add a test, or a check to an existing one, for each fix and feature.
2. **Check the UI.** For anything that shows in the UI, check it in `qrenderdoc.exe` as well, and
   take screenshots.

By hand:

- The demos run on their own: `demos_x64.exe <TestName> --log <file>`. `D3D9_Texture_Formats` and
  `D3D9_Mesh_Output` write a reference file to `util\test\tmp\<TestName>` (or the `--tmp` folder),
  `*_reference_rdoc.txt` under RenderDoc.
- The check scripts (`util/test/d3d9_*_check.py`, `util/test/d3d11_d3dperf_check.py`) run on a
  capture on their own, with the 3.14 module, or inside qrenderdoc's embedded Python 3.6 through
  a wrapper that sets `sys.argv` and redirects stdout:
  ```
  x64\Development\qrenderdoc.exe --python <wrapper.py>
  ```
  They have to stay Python 3.6 compatible for that.

Debugging crashes:

- Dumps go to `C:\dumps`.
- `test_reports/d3d9_d3d9ex_crash.md` shows how an earlier crash was tracked down.

## Recording results

- **STATUS.md:**
  - Keep it accurate. For each fix, say whether it's been tested, and in which report.
  - The pattern so far: "(Fixed after `<report>.md`.) ... Not retested yet." Once it passes, the
    entry says "Retested in `<report>.md`: passed."
  - Update the section for the area you changed: how it works, its decisions and its limitations.
- **Reports:**
  - For a test round worth keeping, write `renderdoc/driver/d3d9/test_reports/<name>.md`, with
    screenshots, logs and outputs in a folder of the same name.
  - State the commit tested, the results with their actual output, and anything unexpected.
  - Say which numbers came from the GPU and which from RenderDoc.
- **README.md:** keep "What works", the limitations and "Changes outside the D3D9 driver" in sync
  with the code.
- **Upstream code:** keep changes outside `renderdoc/driver/d3d9` small. List each one in the
  README.

## Ground rules

- **Real games:** the user tests anything that needs a real D3D9 game, such as Reset, device loss,
  the overlay in a game, and capture memory and time. Don't capture commercial games.
- **Native first:** compare RenderDoc against what the GPU does natively. Don't trust the spec or a
  decoder library on its own. Earlier rounds found two such cases:
  - ATI2's channel order is the reverse of BC5's.
  - The bundled BC2/BC3 decoders use DXT1's 3-colour mode, which only DXT1 has.
- **Missing checks:** when a test can't check something, say so in the report and in STATUS.md,
  rather than leaving it out.
