import struct

import renderdoc as rd
import rdtest

SHADOW_A = 0xFF203040
SHADOW_B = 0xFFA0B0C0
SURFACE_E = 0xFF5A5A5A
RT_C = 0xFFFF0000
MSAA = 0xFF00FFFF
# the demo writes one of these every frame, alternating, so the frame starts with the other one
PARITY = (0xFF00FF00, 0xFF0000FF)


def inside(x, y, x0, y0, x1, y1):
    return x0 <= x < x1 and y0 <= y < y1


def colour565(r5, g6, b5):
    return (r5 << 11) | (g6 << 5) | b5


def expand565(c):
    # R5G6B5 to 8 bits per channel, as GPUs expand DXT endpoints: the top bits repeated
    r, g, b = (c >> 11) & 0x1f, (c >> 5) & 0x3f, c & 0x1f
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255]


# Checks every object's contents at the start of the frame, which are the capture's initial
# contents as replay wrote them back. The layout is in STATUS.md, "Checking D3D9_Resource_Contents
# captures", and in the demo.
class D3D9_Resource_Contents(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Resource_Contents'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()
        self.set_event(0, True)

        parities = set()

        # managed texture, 8x8 with 4 levels: each level its own grey, and level 0's (2,2)-(6,6)
        # what the previous frame locked and wrote
        tex = self.texture_by_name('Texture 24')
        for mip in range(4):
            grey = 0xFF000000 | (0x111111 * (mip + 1))
            rows = self.read_dwords(tex, rd.Subresource(mip, 0, 0))
            if mip == 0:
                parities |= self.check_texels('The managed texture level 0', rows,
                                              lambda x, y: PARITY if inside(x, y, 2, 2, 6, 6) else grey)
            else:
                self.check_texels(f'The managed texture level {mip}', rows, lambda x, y: grey)

        # default pool texture, 16x16 with 2 levels, read from our shadow copy: UpdateTexture's
        # SHADOW_B corner, and UpdateSurface's copy of the system memory surface at (12,12)
        tex = self.texture_by_name('Texture 25')
        rows = self.read_dwords(tex, rd.Subresource(0, 0, 0))

        def default0(x, y):
            if inside(x, y, 13, 13, 15, 15):
                return PARITY
            if inside(x, y, 12, 12, 16, 16):
                return SURFACE_E
            return SHADOW_B if inside(x, y, 0, 0, 8, 8) else SHADOW_A

        parities |= self.check_texels('The default pool texture level 0', rows, default0)
        rows = self.read_dwords(tex, rd.Subresource(1, 0, 0))
        self.check_texels('The default pool texture level 1', rows,
                          lambda x, y: SHADOW_B if inside(x, y, 0, 0, 4, 4) else SHADOW_A)

        # system memory surface, 4x4, with the previous frame's lock at (1,1)-(3,3)
        rows = self.read_dwords(self.texture_by_name('Offscreen Surface 30'), rd.Subresource(0, 0, 0))
        parities |= self.check_texels('The system memory surface', rows,
                                      lambda x, y: PARITY if inside(x, y, 1, 1, 3, 3) else SURFACE_E)

        # render target texture, 32x32, with the previous frame's ColorFill in the top left 16x16
        rows = self.read_dwords(self.texture_by_name('Render Target Texture 31'), rd.Subresource(0, 0, 0))
        parities |= self.check_texels('The render target texture', rows,
                                      lambda x, y: PARITY if inside(x, y, 0, 0, 16, 16) else RT_C)

        # multisampled render target, 64x64
        tex = self.texture_by_name('Render Target 33')
        if tex.msSamp < 2:
            raise rdtest.TestFailureException(f"The multisampled render target has {tex.msSamp} samples")
        rows = self.read_dwords(tex, rd.Subresource(0, 0, 0))
        self.check_texels('The multisampled render target', rows, lambda x, y: MSAA)

        # cube texture, 4x4: face i is 0xFF000010 * (i + 1)
        tex = self.texture_by_name('Cube Texture 34')
        for face in range(6):
            rows = self.read_dwords(tex, rd.Subresource(0, face, 0))
            self.check_texels(f'The cube texture face {face}', rows, lambda x, y: 0xFF000000 | (0x10 * (face + 1)))

        # volume texture, 4x4x4: slice i is 0xFF000100 * (i + 1)
        tex = self.texture_by_name('Volume Texture 35')
        for z in range(4):
            rows = self.read_dwords(tex, rd.Subresource(0, z, 0))
            self.check_texels(f'The volume texture slice {z}', rows, lambda x, y: 0xFF000000 | (0x100 * (z + 1)))

        # DXT1 cube and volume textures, 4x4: each face and slice one colour, as the demo makes them.
        # Saving one as a PNG decodes it on the CPU, a face or a depth slice at a time.
        tex = self.texture_by_name('Cube Texture 36')
        for face in range(6):
            want = expand565(colour565(5 * (face + 1), 63 - 8 * face, 3 * face))
            self.check_saved_png(f'The DXT1 cube texture face {face}', tex, face, want)

        tex = self.texture_by_name('Volume Texture 37')
        for z in range(4):
            want = expand565(colour565(0, 10 * (z + 1), 31 - 6 * z))
            self.check_saved_png(f'The DXT1 volume texture slice {z}', tex, z, want)

        # the recycled texture, released in the frame: one colour, from the previous frame
        rows = self.read_dwords(self.texture_by_name('Texture 44'), rd.Subresource(0, 0, 0))
        colour = rows[0][0]
        self.check_texels(f'The recycled texture (0x{colour:08X})', rows, lambda x, y: colour)

        # every parity region has the previous frame's colour
        if len(parities) != 1:
            raise rdtest.TestFailureException(f"The regions the frames alternate have different colours: {parities}")
        rdtest.log.success(f"Every region the frames alternate has the previous frame's 0x{parities.pop():08X}")

        # the managed vertex buffer has the default triangle, the default pool index buffer 0, 1, 2
        vb = self.get_resource_by_name('Vertex Buffer 21')
        data = self.controller.GetBufferData(vb.resourceId, 0, 0)
        positions = [struct.unpack_from('3f', data, i * 36) for i in range(3)]
        want = [(-0.5, -0.5, 0.0), (0.0, 0.5, 0.0), (0.5, -0.5, 0.0)]
        if not all(rdtest.value_compare(p, w) for p, w in zip(positions, want)):
            raise rdtest.TestFailureException(f"The managed vertex buffer's positions are {positions}, expected {want}")
        rdtest.log.success("The managed vertex buffer has the default triangle")

        ib = self.get_resource_by_name('Index Buffer 22')
        indices = struct.unpack('<3H', self.controller.GetBufferData(ib.resourceId, 0, 6))
        if indices != (0, 1, 2):
            raise rdtest.TestFailureException(f"The write-only index buffer holds {indices}, expected (0, 1, 2)")
        rdtest.log.success("The write-only default pool index buffer holds 0, 1, 2")

        # and the frame replays: the triangle drawn from those buffers
        last_action = self.get_last_action()
        self.set_event(last_action.eventId, True)
        self.check_pixel_value(last_action.copyDestination, 200, 200, [0.0, 1.0, 0.0, 1.0])

        self.step_all_actions()

    def check_saved_png(self, what: str, tex: rd.TextureDescription, slice_index: int, want):
        save = rd.TextureSave()
        save.resourceId = tex.resourceId
        save.destType = rd.FileType.PNG
        save.mip = 0
        save.slice.sliceIndex = slice_index
        save.alpha = rd.AlphaMapping.Preserve
        path = rdtest.get_tmp_path(f'saved_{tex.resourceId}_{slice_index}.png'.replace('::', '_'))
        result = self.controller.SaveTexture(save, path)
        if result.code != rd.ResultCode.Succeeded:
            raise rdtest.TestFailureException(f"{what}: saving as PNG failed: {result.Message()}")

        rows = rdtest.png_load_data(path)
        if len(rows) != tex.height:
            raise rdtest.TestFailureException(f"{what}: the PNG has {len(rows)} rows, expected {tex.height}")
        channels = len(rows[0]) // tex.width
        for y, row in enumerate(rows):
            for x in range(tex.width):
                got = list(row[x * channels:(x + 1) * channels]) + [255] * (4 - channels)
                if got != want:
                    raise rdtest.TestFailureException(f"{what}: the PNG's {x},{y} is {got}, expected {want}")
        rdtest.log.success(f"{what} saved as a PNG is {want}")
