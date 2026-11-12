import struct

import renderdoc as rd
import rdtest

# what util/test/demos/d3d9/d3d9_texture_lod.cpp sets up: per texture (one row of the targets) its
# level count, the LOD set before the frame and the one the frame sets before its second pass
TEXTURES = [
    ('2D', 7, 2, 4),
    ('Cube', 6, 1, 3),
    ('Volume', 5, 3, 1),
    ('Vertex', 7, 3, 5),
]
VERTEX_ROW = 3
COLUMNS = 8
PASSES = ['LOD set before the frame', 'LOD set in the frame']
LOG_LINE = 'samples read the levels SetLOD says'


def level_colour(tex: int, level: int):
    return 0xFF004000 | ((0x10 + 0x20 * level) << 16) | (tex + 1)


def expected_level(lod: int, max_mip: int, levels: int, column: int):
    """The level the GPU reads at LOD column, with MIPFILTER POINT: SetLOD's level is read as
    level 0, and MAXMIPLEVEL counts from it (measured on an RTX 4070 Ti, see the demo's log)."""
    base = min(lod, levels - 1)
    return min(base + max(column, max_mip), levels - 1)


class D3D9_Texture_LOD(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Texture_LOD'
    internal = False

    # the demo logs its GPU checks' result at frame 10
    native_frames = 12

    def get_capture(self):
        # natively, the demo checks on the GPU which level each SetLOD makes it read
        self.run_native()
        return super().get_capture()

    def require_log_line(self, path: str, what: str):
        with open(path, errors='replace') as f:
            if not any(LOG_LINE in l for l in f):
                raise rdtest.TestFailureException(f"{what} didn't log '{LOG_LINE}'")
        rdtest.log.success(f"{what} logged '{LOG_LINE}'")

    def pass_draws(self, controller, name: str):
        marker = None

        def find(actions):
            nonlocal marker
            for a in actions:
                if marker is None and a.customName == name:
                    marker = a
                find(a.children)

        find(controller.GetRootActions())
        if marker is None:
            raise rdtest.TestFailureException(f"No marker '{name}'")
        draws = [a for a in marker.children if a.flags & rd.ActionFlags.Drawcall]
        if len(draws) != 4:
            raise rdtest.TestFailureException(f"'{name}' has {len(draws)} draws, expected 4 (is VTF supported?)")
        return draws

    def read_row(self, controller, target: rd.ResourceId, row: int):
        data = controller.GetTextureData(target, rd.Subresource(0, 0, 0))
        tex = [t for t in controller.GetTextures() if t.resourceId == target][0]
        return list(struct.unpack_from(f'<{tex.width}I', data, row * tex.width * 4))

    def check_rows(self, controller, draws, pass_idx: int, lod_applied: bool, what: str):
        """
        The target the pass drew is read at its last draw. lod_applied is False for a replay that
        ignores SetLOD (on a D3D9Ex device), where every texture reads as if its LOD were 0.
        """
        last = draws[-1]
        controller.SetFrameEvent(last.eventId, True)
        target = controller.GetPipelineState().GetOutputTargets()[0].resource

        for t, (name, levels, before, in_frame) in enumerate(TEXTURES):
            lod = (in_frame if pass_idx == 1 else before) if lod_applied else 0
            row = self.read_row(controller, target, t)
            got = row[1:COLUMNS + 1] if t == VERTEX_ROW else row[:COLUMNS]
            want = [level_colour(t, expected_level(lod, pass_idx, levels, c)) for c in range(COLUMNS)]
            if got != want:
                raise rdtest.TestFailureException(
                    f"{what}: the {name} row reads {[hex(v) for v in got]}, expected {[hex(v) for v in want]} "
                    f"(LOD {lod}, MAXMIPLEVEL {pass_idx})")
        rdtest.log.success(f"{what}: every row read the levels expected")

    def check_descriptors(self, controller, draws, pass_idx: int, lod_applied: bool, what: str):
        for t, (name, levels, before, in_frame) in enumerate(TEXTURES):
            lod = (in_frame if pass_idx == 1 else before) if lod_applied else 0
            draw = draws[t]
            controller.SetFrameEvent(draw.eventId, True)
            stage = rd.ShaderStage.Vertex if t == VERTEX_ROW else rd.ShaderStage.Pixel
            reads = controller.GetPipelineState().GetReadOnlyResources(stage, True)
            # the samplers are s0-s2 in order, the vertex texture is the vertex shader's only one
            if len(reads) <= (0 if t == VERTEX_ROW else t):
                raise rdtest.TestFailureException(f"{what}: {name} draw has {len(reads)} textures bound")
            d = reads[0 if t == VERTEX_ROW else t].descriptor
            if (d.firstMip, d.numMips) != (lod, levels - lod):
                raise rdtest.TestFailureException(
                    f"{what}: the {name} texture's descriptor has mips {d.firstMip}+{d.numMips}, "
                    f"expected {lod}+{levels - lod}")
            # the D3D9 pipeline state's sampler, which the D3D9 Pipeline State panel shows
            d3d9 = controller.GetD3D9PipelineState()
            samp = d3d9.vertexSamplers[0] if t == VERTEX_ROW else d3d9.pixelSamplers[t]
            if samp.resourceId != d.resource or samp.textureLOD != lod:
                raise rdtest.TestFailureException(
                    f"{what}: the {name} texture's D3D9 sampler has LOD {samp.textureLOD} "
                    f"(texture {samp.resourceId}), expected {lod} ({d.resource})")
        rdtest.log.success(f"{what}: the descriptors and D3D9 samplers start at the LOD's level")

    def check_mesh_output(self, controller, draws, pass_idx: int, what: str):
        name, levels, before, in_frame = TEXTURES[VERTEX_ROW]
        lod = in_frame if pass_idx == 1 else before
        draw = draws[VERTEX_ROW]
        controller.SetFrameEvent(draw.eventId, True)
        data = self.get_postvs(draw, rd.MeshDataStage.VSOut)
        key = [k for k in data[0].keys() if k.upper().startswith('COLOR')]
        if len(data) != COLUMNS or not key:
            raise rdtest.TestFailureException(f"{what}: VS Out has {len(data)} vertices, keys {list(data[0].keys())}")
        for c, v in enumerate(data):
            col = level_colour(VERTEX_ROW, expected_level(lod, pass_idx, levels, c))
            want = [((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0, 1.0]
            got = v[key[0]]
            if any(abs(a - b) > 1.0 / 512.0 for a, b in zip(got, want)):
                raise rdtest.TestFailureException(f"{what}: vertex {c}'s colour is {got}, expected {want}")
        rdtest.log.success(f"{what}: VS Out read the vertex texture's levels from the LOD's on")

    def check_display(self, controller, draws):
        """
        At the second pass the 2D texture's LOD is 4, which stops the GPU sampling levels 0-3. The
        texture viewer samples it, so it has to undo that while it does.
        """
        controller.SetFrameEvent(draws[0].eventId, True)
        tex = controller.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel, True)[0].descriptor.resource

        out = controller.CreateOutput(rd.CreateHeadlessWindowingData(64, 64), rd.ReplayOutputType.Texture)
        try:
            for mip in (0, 3, 6):
                disp = rd.TextureDisplay()
                disp.resourceId = tex
                disp.subresource = rd.Subresource(mip, 0, 0)
                disp.scale = 1.0
                out.SetTextureDisplay(disp)
                out.Display()
                data = out.ReadbackOutputTexture()
                if len(data) < 64 * 64 * 3:
                    raise rdtest.TestFailureException(f"The output readback is {len(data)} bytes")
                # the top left texel: every mip is at least 1x1 and drawn from the corner
                rgb = tuple(data[0:3])
                col = level_colour(0, mip)
                want = ((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF)
                if any(abs(a - b) > 2 for a, b in zip(rgb, want)):
                    raise rdtest.TestFailureException(
                        f"The texture viewer shows the 2D texture's mip {mip} as {rgb}, expected {want}")
            rdtest.log.success("The texture viewer shows mips 0, 3 and 6 of the 2D texture with LOD 4")
        finally:
            out.Shutdown()

    def check_ex_device(self):
        """
        With D3D9_ReplayOnExDevice set the capture replays on a D3D9Ex device, as before: managed
        textures go in the default pool, where SetLOD does nothing.
        """
        setting = rd.SetConfigSetting('D3D9_ReplayOnExDevice')
        setting.data.basic.b = True
        cap = None
        controller = None
        try:
            cap = rd.OpenCaptureFile()
            result = cap.OpenFile(self.capture_filename, '', None)
            if result != rd.ResultCode.Succeeded:
                raise rdtest.TestFailureException(f"Couldn't open the capture again: {result}")
            result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
            if result != rd.ResultCode.Succeeded:
                raise rdtest.TestFailureException(f"Couldn't replay on a D3D9Ex device: {result}")

            draws = [self.pass_draws(controller, p) for p in PASSES]
            for i in (1, 0):
                self.check_rows(controller, draws[i], i, False, f"On a D3D9Ex device, '{PASSES[i]}'")
                self.check_descriptors(controller, draws[i], i, False, f"On a D3D9Ex device, '{PASSES[i]}'")
        finally:
            setting.data.basic.b = False
            if controller is not None:
                controller.Shutdown()
            if cap is not None:
                cap.Shutdown()
        rdtest.log.success("With D3D9_ReplayOnExDevice set, the capture replays with SetLOD ignored")

    def check_d3d9_capture(self):
        self.check_api()

        self.require_log_line(rdtest.get_tmp_path('native.log'), 'The native run')
        self.require_log_line(self.demo_log_path(), 'The run under RenderDoc')

        # every call succeeded when it was captured
        messages = self.controller.GetDebugMessages()
        if messages:
            raise rdtest.TestFailureException(f"The capture has {len(messages)} debug messages, the first: "
                                              f"{messages[0].description}")
        rdtest.log.success("The capture has no debug messages")

        draws = [self.pass_draws(self.controller, p) for p in PASSES]

        # the second pass first, so that going back to the first replays over the in-frame SetLOD:
        # the frame's start has to set the LODs back
        self.check_rows(self.controller, draws[1], 1, True, f"'{PASSES[1]}'")
        self.check_rows(self.controller, draws[0], 0, True, f"'{PASSES[0]}', after replaying '{PASSES[1]}'")

        for i in (0, 1):
            self.check_descriptors(self.controller, draws[i], i, True, f"'{PASSES[i]}'")
            self.check_mesh_output(self.controller, draws[i], i, f"'{PASSES[i]}'")

        self.check_display(self.controller, draws[1])

        self.step_all_actions()

        self.check_ex_device()
