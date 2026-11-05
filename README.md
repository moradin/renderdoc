<p align="center"><img src="https://user-images.githubusercontent.com/661798/36482670-f81601c0-170b-11e8-8adb-2365b346ac27.png" /></p>

[![MIT licensed](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE.md)

RenderDoc with D3D9 support
==============

This is a fork of [RenderDoc](https://github.com/baldurk/renderdoc) that adds Direct3D 9 capture and
replay on Windows, for both D3D9 and D3D9Ex. Upstream RenderDoc doesn't support D3D9, and none of
this work is upstream. Apart from the D3D9 work and the few changes listed under
[Changes outside the D3D9 driver](#changes-outside-the-d3d9-driver), the code is upstream's v1.x
branch as of 24 September 2026.

**Report D3D9 problems [on this fork](https://github.com/moradin/renderdoc/issues), not upstream.**
Upstream's builds, documentation and contacts below are for upstream RenderDoc, which can't open
D3D9 captures. To use D3D9 you have to build this fork yourself (see [Building](#building-the-fork)).

The detailed status, with how each part works and how it was tested, is in
[renderdoc/driver/d3d9/STATUS.md](renderdoc/driver/d3d9/STATUS.md). The Windows test reports are in
[renderdoc/driver/d3d9/test_reports](renderdoc/driver/d3d9/test_reports).

### What works

- **Capture**:
  - Every D3D9 and D3D9Ex object is wrapped, and every call is recorded.
  - The device's full state at the start of the frame is saved.
  - Resource contents are saved, including data written with Lock and default pool textures. Those
    textures can't be read back, so the fork keeps a copy of what's uploaded to them.
  - Depth stencils that can't be locked are read back with the depth and stencil tests.
  - State blocks, Reset and ResetEx, device loss during a capture, D3DPERF markers, GDI drawing and
    GetFrontBufferData are handled.
- **Replay**, in the UI and from Python:
  - the Event Browser;
  - the Texture Viewer, for every D3D9 format, with pick pixel, min/max, the histogram and saving;
  - the overlays, except quad overdraw;
  - a D3D9 Pipeline State panel, and `D3D9State` in Python through `GetD3D9PipelineState()`;
  - shader model 1 to 3 disassembly, which matches fxc's;
  - shader reflection from the constant table, and the constant viewers;
  - the Mesh Viewer's VS In and VS Out, for vertex shaders 1.1 to 3.0 and fixed function vertex
    processing. RenderDoc works out the VS Out data on the CPU.
- Replay, and reading back depth stencils during capture, need a shader model 3 GPU.

### What's missing

Not implemented:

- shader debugging;
- pixel history;
- shader editing and replacement, including "Edit Generated Stub";
- custom display shaders in the Texture Viewer;
- GPU performance counters;
- resource usage: the resource context menu's usage list is empty for D3D9;
- debug messages from the D3D9 debug runtime;
- the quad overdraw overlays, since D3D9 has no UAVs.

Known limitations of what works:

- **Depth stencils**:
  - D24FS8 and `D3DMULTISAMPLE_NONMASKABLE` depth stencils can't be read back at capture.
  - D32 is kept to 24 bits.
  - Writing D24 back at replay lands one level off for a depth of 0.375.
- **Texture Viewer**:
  - Multisampled colour targets are only shown resolved.
  - P8 and A8P8 textures show their palette indices, not the colours.
  - ATI2's channel order hasn't been checked against a known texture.
  - Block compressed (DXT) textures can only be saved as DDS.
- **Mesh Viewer, VS Out**:
  - `texldl` in vertex shaders always point samples.
  - Fixed function processing doesn't handle N-patches, user clip planes, displacement mapping or
    more than 8 lights.
  - A vs_3_0 shader that packs two semantics into one output register shows only one of them.
- **Overlays**:
  - Pixels that the pixel shader discards or gives a new depth show as plain pass or fail.
  - Alpha test is ignored.
- **Replay**:
  - An UpdateTexture inside the frame can copy more than it did at capture, because the source's
    dirty region isn't saved.
- **Memory at capture**: the copies of default pool texture uploads take as much memory as the
  textures themselves.

Not tested yet:

- **32-bit programs.** All testing used 64-bit builds of the test programs.
- **Other GPUs.** All testing was on one NVIDIA GPU (RTX 4070 Ti).
- **Pixel shader constant blocks** read through `GetCBufferVariableContents` with a length of 0.
  The vertex shader ones are tested.
- **Remote replay.** It probably won't display textures.
- **D3DPERF markers in D3D11 captures.** Upstream moved its D3DPERF hooks into the D3D11 driver, and
  they now share `d3d9.dll` with this fork's device hooks.

### Changes outside the D3D9 driver

- **D3D9 pipeline state**: a new `D3D9State` type, passed through the replay controller and the
  remote replay proxy, with Python bindings, documentation and its own UI panel.
- **Mesh Viewer**: the GS/DS Output table and preview tab are hidden for D3D9 captures.
- **Rich text cells** (in the Event Browser, the Pipeline State panels and the Resource Inspector,
  for every API):
  - resource link icons are sized to the font;
  - a cell holding just a resource link has its underline at the font's underline position, and
    its hover area is the full height of the row.
- **D3DPERF hooks**: upstream's hooks in the D3D11 driver also pass the markers on to D3D9 devices.
- **AMD shader ISA**: no GCN targets are offered for D3D9 shaders, since AMD's plugins can't
  compile them.
- **Core**: `InitialiseReplay` waits for the GPU enumeration thread it started on a previous call.
  Before this fix, calling it twice in one process (as Python scripts inside qrenderdoc do) could
  run two enumerations at once and crash while a capture loaded. Upstream has this bug too.

### Building the fork

Build [renderdoc.sln](renderdoc.sln) as upstream's [Compiling.md](docs/CONTRIBUTING/Compiling.md)
describes. D3D9 support is part of `renderdoc.dll`, so nothing extra is needed. It has been tested
in the x64 Development build.

To test it:

- The test programs are the `D3D9_*` and `D3D9Ex_*` tests in
  [util/test/demos](util/test/demos/d3d9).
- The scripts that check their captures are `util/test/d3d9_*.py`. STATUS.md describes each one.
- The shader unit tests run with `renderdoccmd test unit "[d3d9]"` on Windows, or
  `util/test/d3d9_shader_unit_tests.sh` on Linux.

Upstream RenderDoc
==============

RenderDoc is a frame-capture based graphics debugger, currently available for Vulkan, D3D11, D3D12, OpenGL, and OpenGL ES development on Windows, Linux, Android, and Nintendo Switch&trade;. It is completely open-source under the MIT license.

RenderDoc is intended for debugging your own programs only. Any discussion of capturing programs that you did not create will not be allowed in any official public RenderDoc setting, including the issue tracker, discord, or via email. For example this includes capturing commercial games that you did not create, or capturing Google Maps or Google Earth. Note: Capturing projects you created that use a third party engine like Unreal or Unity, or open source and free projects is completely fine and supported.

If you have any questions, suggestions or problems or you can [create an issue](https://github.com/baldurk/renderdoc/issues/new/choose) here on github, [email me directly](mailto:baldurk@baldurk.org) or come into [IRC](https://webchat.oftc.net/?channels=renderdoc) or [Discord](https://discord.gg/ahq6yRB) to discuss it.

To install on windows run the appropriate installer for your OS ([64-bit](https://renderdoc.org/stable/latest/RenderDoc_latest_64.msi) | [32-bit](https://renderdoc.org/stable/latest/RenderDoc_latest_32.msi)) or download the portable zip from the [builds page](https://renderdoc.org/builds). The 64-bit windows build fully supports capturing from 32-bit programs. On linux only 64-bit x86 is supported - there is a precompiled [binary tarball](https://renderdoc.org/stable/latest/renderdoc_latest.tar.gz) available, or your distribution may package it. If not you can [build from source](docs/CONTRIBUTING/Compiling.md).

* **Downloads**: Stable and nightly builds: https://renderdoc.org/builds ( [Symbol server](https://renderdoc.org/symbols) )
* **Documentation**: [HTML online](https://renderdoc.org/docs), [CHM in builds](https://renderdoc.org/docs/renderdoc.chm), [Videos](https://www.youtube.com/user/baldurkarlsson)
* **Contact**: [baldurk@baldurk.org](mailto:baldurk@baldurk.org), [#renderdoc on OFTC IRC](https://webchat.oftc.net/?channels=renderdoc), [Discord server](https://discord.gg/ahq6yRB)
* **Code of Conduct**: [Contributor Covenant](docs/CODE_OF_CONDUCT.md)
* **Information for contributors**: [All contribution information](docs/CONTRIBUTING.md), [Compilation instructions](docs/CONTRIBUTING/Compiling.md)
* **Community extensions**: [Extensions repository](https://github.com/baldurk/renderdoc-contrib)

Screenshots
--------------

| [ ![Texture view](https://renderdoc.org/fp/ts_screen1.jpg?2) ](https://renderdoc.org/fp/screen1.jpg) | [ ![Pixel history & shader debug](https://renderdoc.org/fp/ts_screen2.jpg?2) ](https://renderdoc.org/fp/screen2.png) |
| --- | --- |
| [ ![Mesh viewer](https://renderdoc.org/fp/ts_screen3.jpg?2) ](https://renderdoc.org/fp/screen3.png) | [ ![Pipeline viewer & constants](https://renderdoc.org/fp/ts_screen4.jpg?2) ](https://renderdoc.org/fp/screen4.png) |

API Support
--------------

|                          | Windows                  | Linux                    | Android                   |
| ------------------------ | ------------------------ | ------------------------ | ------------------------  |
| Vulkan                   | :heavy_check_mark:       | :heavy_check_mark:       | :heavy_check_mark:        |
| OpenGL ES 2.0 - 3.2      | :heavy_check_mark:       | :heavy_check_mark:       | :heavy_check_mark:        |
| OpenGL 3.2 - 4.6 Core    | :heavy_check_mark:       | :heavy_check_mark:       |  N/A                      |
| D3D11 & D3D12            | :heavy_check_mark:       |  N/A                     |  N/A                      |
| OpenGL 1.0 - 2.0 Compat  | :heavy_multiplication_x: | :heavy_multiplication_x: |  N/A                      |
| D3D9 (this fork only)    | :heavy_check_mark:       |  N/A                     |  N/A                      |
| D3D10                    | :heavy_multiplication_x: |  N/A                     |  N/A                      |
| Metal                    |  N/A                     |  N/A                     |  N/A                      |

* Nintendo Switch&trade; support is distributed separately for authorized developers as part of the NintendoSDK. For more information, consult the Nintendo Developer Portal.

Downloads
--------------

There are [binary releases](https://renderdoc.org/builds) available, built from the release targets. If you just want to use the program and you ended up here, this is what you want :).

It's recommended that if you're new you start with the stable builds. Nightly builds are available every day from the [v1.x branch here](https://renderdoc.org/builds#nightly) if you need it, but correspondingly may be less stable.

Documentation
--------------

The text documentation is available [online for the latest stable version](https://renderdoc.org/docs/), as well as in [renderdoc.chm](https://renderdoc.org/docs/renderdoc.chm) in any build. It's built from [restructured text with sphinx](docs).

As mentioned above there are some [youtube videos](https://www.youtube.com/user/baldurkarlsson) showing the use of some basic features and an introduction/overview.

There is also a great presentation by [@Icetigris](https://twitter.com/Icetigris) which goes into some details of how RenderDoc can be used in real world situations: [slides are up here](https://docs.google.com/presentation/d/1LQUMIld4SGoQVthnhT1scoA3k4Sg0as14G4NeSiSgFU/edit#slide=id.p).

License
--------------

RenderDoc is released under the MIT license, see [LICENSE.md](LICENSE.md) for full text as well as 3rd party library acknowledgements.

Compiling
---------

Building RenderDoc is fairly straight forward on most platforms. See [Compiling.md](docs/CONTRIBUTING/Compiling.md) for more details.

Contributing & Development
--------------

I've added some notes on how to contribute, as well as where to get started looking through the code in [Developing-Change.md](docs/CONTRIBUTING/Developing-Change.md). All contribution information is available under [CONTRIBUTING.md](docs/CONTRIBUTING.md).

