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

        # INTZ, cleared to 0.375 and stencil 0x0F. It reads one below 0.375's nominal level (see
        # STATUS.md's known gaps).
        if self.get_resource_by_name('Depth Texture 24') is not None:
            rows, _ = self.depth('Depth Texture 24')
            self.check_texels('The INTZ depth texture', rows, lambda x, y: 0x5FFFFF0F)
        else:
            rdtest.log.print("No INTZ texture, the GPU doesn't support it")

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
