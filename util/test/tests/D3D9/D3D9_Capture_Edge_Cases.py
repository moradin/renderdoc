import renderdoc as rd
import rdtest

PARITY = (0xFF00FF00, 0xFF0000FF)


# Checks what the capture read at the start of the frame, as replay wrote it back: depth stencils
# read with the depth and stencil tests, a texture locked across Present, and a surface drawn to with
# GDI. The demo checks itself that those reads left its state alone, which check_capture() finds in
# its log. The layout is in STATUS.md, "Checking D3D9_Capture_Edge_Cases captures".
class D3D9_Capture_Edge_Cases(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Capture_Edge_Cases'
    internal = False

    def depth(self, name: str, sample: int = 0):
        replay = self.check_script('d3d9_replay_check')
        tex = self.texture_by_name(name)
        values = replay.depth_values(self.controller, tex, sample)
        if values is None:
            raise rdtest.TestFailureException(f"Couldn't read {name} ({tex.format.Name()}) sample {sample}")
        return [values[y * tex.width:(y + 1) * tex.width] for y in range(tex.height)], tex

    def check_d3d9_capture(self):
        self.check_api()

        for marker in ['Depth', 'Multisampled', 'State blocks']:
            if self.find_action(marker) is None:
                raise rdtest.TestFailureException(f"No '{marker}' marker")
        rdtest.log.success("The Depth, Multisampled and State blocks markers are there")

        self.set_event(0, True)

        # the automatic depth stencil (D24S8), as depth << 8 | stencil: two rects drawn with
        # different depths and stencil references over a clear to 1.0 and 0
        rows, tex = self.depth('Auto Depth Stencil 16')

        def auto(x, y):
            if y < 100 and x < 100:
                return 0x4000005A
            if y < 100 and x < 200:
                return 0xBFFFFFA5
            return 0xFFFFFF00

        self.check_texels('The automatic depth stencil', rows, auto)

        # the multisampled one: a triangle over the lower left half in every sample. Pixels on the
        # diagonal are covered by some samples.
        tex = self.texture_by_name('Depth Stencil 20')
        if tex.msSamp < 2:
            raise rdtest.TestFailureException(f"The multisampled depth stencil has {tex.msSamp} samples")
        for sample in range(tex.msSamp):
            rows, _ = self.depth('Depth Stencil 20', sample)
            self.check_texels(f'The multisampled depth stencil sample {sample}', rows,
                              lambda x, y: 0x80000033 if y > x else 0xFFFFFF00 if y < x else (0x80000033, 0xFFFFFF00))

        # D16, cleared to 0.5 before the first frame. 0x7FFF is 0.5's level.
        rows, _ = self.depth('Depth Stencil 23')
        self.check_texels('The D16 depth stencil', rows, lambda x, y: 0x7FFF)

        # INTZ, cleared to 0.375 and stencil 0x0F. 0x600000 is 0.375's level.
        if self.get_resource_by_name('Depth Texture 24') is not None:
            rows, _ = self.depth('Depth Texture 24')
            self.check_texels('The INTZ depth texture', rows, lambda x, y: 0x6000000F)
        else:
            rdtest.log.print("No INTZ texture, the GPU doesn't support it")

        self.check_depth_levels()

        # the texture the previous frame locked before Present and unlocked in this one
        rows = self.read_dwords(self.texture_by_name('Texture 31'), rd.Subresource(0, 0, 0))
        span = self.check_texels('The texture locked across Present', rows, lambda x, y: PARITY)
        if len(span) != 1:
            raise rdtest.TestFailureException(f"The texture locked across Present has both colours: {span}")

        # the system memory surface the previous frame drew to with GDI. It's X8R8G8B8, and GDI may
        # leave the top byte 0, so that isn't compared.
        rows = self.read_dwords(self.texture_by_name('Offscreen Surface 32'), rd.Subresource(0, 0, 0))
        gdi = self.check_texels('The surface drawn with GDI', rows,
                                lambda x, y: (0x00123456, 0x00654321) if x < 16 and y < 16 else 0x00808080,
                                mask=0x00FFFFFF)
        if len(gdi) != 1:
            raise rdtest.TestFailureException(f"The surface drawn with GDI has both colours: {gdi}")

        self.step_all_actions()

    def check_depth_levels(self):
        # A D24S8 depth stencil with 4096 depths cleared a pixel at a time before the frame, and a
        # draw testing the same depths against it with ZFUNC EQUAL, green where it passes (natively
        # all of them, which the demo checks). Replay only passes them all if it wrote each depth
        # back to the level it had.
        if self.find_action('Depth levels') is None:
            rdtest.log.print("No 'Depth levels' marker, the GPU doesn't support R32F textures")
            return

        draw = self.find_marker_draw('Depth levels')

        # what the capture read: the first 8 depths' levels, which are certain (d * (2^24 - 1)
        # isn't near half way between two of them), for 0.375, 0.75000006, 0, 1, 0.25, 0.5, 0.75
        # and 0.33333334. 0.375's and 0.75000006's are the ones the old formula never wrote back.
        self.set_event(0, True)
        tex = self.get_texture(draw.depthOut)
        replay = self.check_script('d3d9_replay_check')
        values = replay.depth_values(self.controller, tex, 0)
        if values is None:
            raise rdtest.TestFailureException(f"Couldn't read the depth levels' depth stencil {draw.depthOut}")
        expected = [0x60000000, 0xC0000000, 0x00000000, 0xFFFFFF00,
                    0x40000000, 0x80000000, 0xBFFFFF00, 0x55555500]
        got = values[0:8]
        if got != expected:
            raise rdtest.TestFailureException(
                "The depth levels' first 8 values are {}, expected {}".format(
                    ' '.join(f'0x{v:08X}' for v in got), ' '.join(f'0x{v:08X}' for v in expected)))
        rdtest.log.success("The depth levels' first 8 values are each depth's level")

        self.set_event(draw.eventId, True)
        target = self.get_texture(draw.outputs[0])
        rows = self.read_dwords(target, rd.Subresource(0, 0, 0))
        self.check_texels('The depth levels tested with ZFUNC EQUAL', rows, lambda x, y: 0xFF00FF00)
