import renderdoc as rd
import rdtest

BASE = 0xFF203040
# the end of each frame fills the sources with one of these, alternating, and the frame's locks
# write one of these
FRAME_COLOURS = (0xFF00FF00, 0xFF0000FF)
LOCK_COLOURS = (0xFFFF00FF, 0xFFFFFF00)


def small_rects(count):
    return [(i, (i * 3) % 8, i + 1, (i * 3) % 8 + 1) for i in range(count)]


# What each UpdateTexture in the frame should copy, in order, as the demo sets it up
# (util/test/demos/d3d9/d3d9_dirty_regions.cpp): for each subresource (mip, slice), the boxes
# (x0, y0, x1, y1) copied. Volume slices are subresources here. 2D and volume textures keep up to
# 6 boxes, the 7th makes all of it dirty; a cube face keeps the box around what was added to it.
WHOLE8 = [(0, 0, 8, 8)]
COPIES = [
    ('the 2D texture, with two rects dirty since the last frame',
     {(0, 0): [(0, 0, 8, 8), (10, 12, 13, 15)], (1, 0): [(0, 0, 4, 4), (5, 6, 7, 8)]}),
    ('the cube texture, with two rects on face 1 (copied as the box around them) and all of face 2',
     {(0, f): {1: [(0, 0, 7, 7)], 2: WHOLE8}.get(f, []) for f in range(6)}),
    ('the volume texture, with two boxes dirty',
     {(0, z): [(0, 0, 4, 4)] if z < 2 else ([(6, 6, 8, 8)] if z == 3 else []) for z in range(4)}),
    ('the texture with nothing dirty', {(0, 0): []}),
    ('the texture with 6 dirty rects', {(0, 0): small_rects(6)}),
    ('the texture with 7 dirty rects, which make all of it dirty', {(0, 0): WHOLE8}),
    ("the 2D texture, with the frame's lock dirty (not the D3DLOCK_NO_DIRTY_UPDATE one or level 1's)",
     {(0, 0): [(8, 8, 12, 12)], (1, 0): [(4, 4, 6, 6)]}),
]

LOG_LINE = 'UpdateTexture copied the dirty regions in all'


def inside(boxes, x, y):
    return any(b[0] <= x < b[2] and b[1] <= y < b[3] for b in boxes)


class D3D9_Dirty_Regions(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Dirty_Regions'
    internal = False

    # the demo logs its GPU checks' result at frame 10
    native_frames = 12

    def get_capture(self):
        # natively, the demo checks on the GPU that UpdateTexture copies what the dirty regions say
        self.run_native()
        return super().get_capture()

    def require_log_line(self, path: str, what: str):
        with open(path, errors='replace') as f:
            if not any(LOG_LINE in l for l in f):
                raise rdtest.TestFailureException(f"{what} didn't log '{LOG_LINE}'")
        rdtest.log.success(f"{what} logged '{LOG_LINE}'")

    def check_d3d9_capture(self):
        self.check_api()

        self.require_log_line(rdtest.get_tmp_path('native.log'), 'The native run')
        self.require_log_line(self.demo_log_path(), 'The run under RenderDoc')

        sdfile = self.controller.GetStructuredFile()
        copies = []

        def walk(actions):
            for a in actions:
                if (a.flags & rd.ActionFlags.Copy) and 'UpdateTexture' in a.GetName(sdfile):
                    copies.append(a)
                walk(a.children)

        walk(self.controller.GetRootActions())

        if len(copies) != len(COPIES):
            raise rdtest.TestFailureException(f"The frame has {len(copies)} UpdateTexture calls, expected {len(COPIES)}")

        self.check_initial_contents(copies)

        for action, (what, regions) in zip(copies, COPIES):
            self.check_copy(action, what, regions)

        self.step_all_actions()

    def check_initial_contents(self, copies):
        """
        The default pool textures as the frame starts, which captures store from our shadow copies
        of what UpdateTexture copied into them: what the previous frame copied, on the base colour.
        """
        self.set_event(0, True)
        found = set()

        # the 2D texture: the previous frame's two carried rects, and its locked rect. The lock only
        # wrote mip 0, so mip 1 copied the colour the source had there.
        dst = self.get_texture(copies[0].copyDestination)
        carried, locked = COPIES[0][1], COPIES[-1][1]
        for sub in carried:
            def want(x, y):
                if inside(carried[sub], x, y):
                    return FRAME_COLOURS
                if inside(locked[sub], x, y):
                    return LOCK_COLOURS if sub[0] == 0 else FRAME_COLOURS
                return BASE
            found |= self.check_texels(f"The 2D texture's initial contents, mip {sub[0]}",
                                       self.read_dwords(dst, rd.Subresource(*sub, 0)), want)

        for action, (what, regions) in zip(copies[1:-1], COPIES[1:-1]):
            dst = self.get_texture(action.copyDestination)
            for sub, boxes in regions.items():
                found |= self.check_texels(f"The initial contents of {what}: mip {sub[0]} slice {sub[1]}",
                                           self.read_dwords(dst, rd.Subresource(*sub, 0)),
                                           lambda x, y: FRAME_COLOURS if inside(boxes, x, y) else BASE)

        # one frame colour and one lock colour, from the same frame
        if len(found) != 2 or len(found & set(FRAME_COLOURS)) != 1:
            raise rdtest.TestFailureException(f"The previous frame's colours are mixed: {[hex(c) for c in found]}")
        rdtest.log.success("Every region has the previous frame's colour")

    def check_copy(self, action, what: str, regions):
        src = self.get_texture(action.copySource)
        dst = self.get_texture(action.copyDestination)

        # the contents before the copy, and after it
        self.controller.SetFrameEvent(action.eventId - 1, True)
        before = {sub: self.read_dwords(dst, rd.Subresource(*sub, 0)) for sub in regions}
        self.controller.SetFrameEvent(action.eventId, True)
        after = {sub: self.read_dwords(dst, rd.Subresource(*sub, 0)) for sub in regions}
        source = {sub: self.read_dwords(src, rd.Subresource(*sub, 0)) for sub in regions}

        # outside the regions the source has to differ from the destination, or copying too much
        # wouldn't show
        differ = 0
        for sub, boxes in regions.items():
            for y, row in enumerate(source[sub]):
                for x, v in enumerate(row):
                    if not inside(boxes, x, y) and v != before[sub][y][x]:
                        differ += 1
        whole = all(inside(boxes, x, y) for sub, boxes in regions.items()
                    for y, row in enumerate(source[sub]) for x in range(len(row)))
        if differ == 0 and not whole:
            raise rdtest.TestFailureException(f"UpdateTexture of {what}: the source is the same as the destination "
                                              f"outside the dirty region, so the check means nothing")

        for sub, boxes in regions.items():
            def expected(x, y):
                return source[sub][y][x] if inside(boxes, x, y) else before[sub][y][x]

            self.check_texels(f"UpdateTexture (event {action.eventId}) of {what}: mip {sub[0]} slice {sub[1]}",
                              after[sub], expected)
