import struct
import time

import renderdoc as rd
import rdtest


# Where the demo's clears and copies write (d3d9_pixel_history.cpp), as (left, top, right, bottom),
# and whether they write the depth stencil too. 'Begin RenderPass' clears the render target, then
# the depth stencil in a second Clear.
CLEAR_RECTS = [(20, 20, 40, 40), (320, 20, 340, 40)]
# the viewport (50, 15, 40x30) and the scissor rect (55, 0, 400, 300)
CLEAR_VIEWPORT_SCISSOR = [(55, 15, 90, 45)]
WRITES = {
    'Clear Rects': (CLEAR_RECTS, False),
    'Clear Viewport Scissor': (CLEAR_VIEWPORT_SCISSOR, True),
    'ColorFill': ([(360, 20, 380, 40)], False),
    'StretchRect': ([(320, 20, 340, 40)], False),
    # 10x10 at (25, 25)
    'UpdateSurface': ([(25, 25, 35, 35)], False),
}


def float_of(bits: int):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def f32(v: float):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def unorm8(c: int):
    """An 8-bit channel as RenderDoc decodes it: c / 255 rounded to float32."""
    return f32(c / 255.0)


class D3D9_Pixel_History(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Pixel_History'
    internal = False
    slow_test = True

    def get_capture(self):
        # the native run records every probe pixel after every event
        self.run_native()
        return super().get_capture()

    def read_reference(self):
        """The native run's records: per batch, the start, then one per event, in order."""
        batches = {}
        with open(self.demo_tmp_path('D3D9_Pixel_History_reference.txt')) as f:
            rec = None
            for line in f:
                parts = line.rstrip('\n').split('\t')
                if parts[0] == 'E':
                    rec = {'marker': parts[2], 'kind': parts[3], 'depth_used': parts[4] == '1',
                           'probes': {}}
                    batches.setdefault(parts[1], []).append(rec)
                else:
                    x, y = int(parts[1]), int(parts[2])
                    rgba = int(parts[4], 16)
                    rec['probes'][(x, y)] = {
                        'covered': int(parts[3]),
                        'col': [unorm8((rgba >> s) & 0xff) for s in (24, 16, 8, 0)],
                        'depth': float_of(int(parts[5], 16)),
                        'stencil': int(parts[6]),
                    }
        if not batches:
            raise rdtest.TestFailureException("The native run recorded nothing")
        return batches

    def batch_actions(self, batch: rd.ActionDescription):
        """The batch's events that aren't markers, in order, with the last marker set before each."""
        out = []
        marker = ['']

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.SetMarker:
                    marker[0] = a.customName
                    continue
                if a.flags & (rd.ActionFlags.PushMarker | rd.ActionFlags.PopMarker):
                    walk(a.children)
                    continue
                out.append((a, marker[0]))

        walk(batch.children)
        return out

    def expected_listed(self, kind: str, marker: str, nth_clear: int, rec, probe, depth_target: bool):
        """Whether the event at rec wrote the probe in the target: the native coverage for draws,
        the demo's rectangles for clears and copies."""
        x, y = probe
        if kind == 'draw':
            if depth_target and not rec['depth_used']:
                return False
            return rec['probes'][probe]['covered'] == 1
        if marker == 'Begin RenderPass':
            # the first clears the render target, the second the depth stencil
            return (nth_clear == 1) == depth_target
        rects, writes_depth = WRITES[marker]
        if depth_target and not writes_depth:
            return False
        return any(l <= x < r and t <= y < b for (l, t, r, b) in rects)

    def check_history(self, batch_name: str, events, target, depth_target: bool, has_depth: bool,
                      probe, until_eid: int):
        """events: (eventId, kind, marker, nth clear of the marker, record before, record after)."""
        x, y = probe
        what = f"{batch_name} {'depth stencil' if depth_target else 'colour'} ({x}, {y})"

        modifs = self.controller.PixelHistory(target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)

        expected = [e for e in events if e[0] <= until_eid and
                    self.expected_listed(e[1], e[2], e[3], e[5], probe, depth_target)]
        got = [m.eventId for m in modifs]
        if got != [e[0] for e in expected]:
            raise rdtest.TestFailureException(f"{what}: listed {[(m.eventId) for m in modifs]}, expected "
                                              f"{[(e[0], e[2]) for e in expected]}")

        depth_diff = 0.0
        for m, (eid, kind, marker, _, before, after) in zip(modifs, expected):
            name = f"{what} event {eid} ({marker})"
            if m.directShaderWrite != (kind == 'copy'):
                raise rdtest.TestFailureException(f"{name}: directShaderWrite is {m.directShaderWrite}")
            if m.primitiveID != 0xFFFFFFFF or m.unboundPS:
                raise rdtest.TestFailureException(f"{name}: primitive {m.primitiveID}, unboundPS {m.unboundPS}")
            if kind == 'draw' and m.shaderOut.IsValid():
                raise rdtest.TestFailureException(f"{name}: a draw's shader output isn't known yet, but it's given")

            for label, val, rec in (('before', m.preMod, before), ('after', m.postMod, after)):
                native = rec['probes'][probe]
                if depth_target:
                    if list(val.col.uintValue) != [0, 0, 0, 0]:
                        raise rdtest.TestFailureException(f"{name}: a depth stencil's colour {label} isn't 0")
                else:
                    col = list(val.col.floatValue)
                    if col != native['col']:
                        raise rdtest.TestFailureException(f"{name}: colour {label} is {col}, natively "
                                                          f"{native['col']}")
                # a colour target's draws have the depth stencil's values, its clears and copies none
                if depth_target or (kind == 'draw' and has_depth):
                    depth_diff = max(depth_diff, abs(val.depth - native['depth']))
                    if abs(val.depth - native['depth']) > 1.0e-7 or val.stencil != native['stencil']:
                        raise rdtest.TestFailureException(
                            f"{name}: depth and stencil {label} are {val.depth} {val.stencil}, natively "
                            f"{native['depth']} {native['stencil']}")
                elif val.depth != -1.0 or val.stencil != -1:
                    raise rdtest.TestFailureException(f"{name}: depth and stencil {label} are {val.depth} "
                                                      f"{val.stencil}, expected none (-1)")

        return len(modifs), depth_diff

    def check_d3d9_capture(self):
        self.check_api()

        if not self.controller.GetAPIProperties().pixelHistory:
            raise rdtest.TestFailureException("D3D9 doesn't say it has pixel history")

        reference = self.read_reference()

        for batch_name, records in reference.items():
            batch = self.find_action("Batch: " + batch_name)
            if batch is None:
                raise rdtest.TestFailureException(f"No batch '{batch_name}'")

            actions = self.batch_actions(batch)
            if records[0]['kind'] != 'start' or len(actions) != len(records) - 1:
                raise rdtest.TestFailureException(f"{batch_name}: {len(actions)} events in the capture, "
                                                  f"{len(records) - 1} natively")

            events = []
            clears = {}
            for i, (a, marker) in enumerate(actions):
                rec = records[i + 1]
                if marker != rec['marker']:
                    raise rdtest.TestFailureException(f"{batch_name}: event {a.eventId} is after '{marker}', "
                                                      f"natively '{rec['marker']}'")
                nth = 0
                if rec['kind'] == 'clear':
                    clears[marker] = clears.get(marker, 0) + 1
                    nth = clears[marker] - 1
                events.append((a.eventId, rec['kind'], marker, nth, records[i], rec))

            # the targets, as the last draw has them bound
            last = actions[-1][0]
            self.set_event(last.eventId, True)
            pipe = self.controller.GetPipelineState()
            colour = pipe.GetOutputTargets()[0].resource
            depth = pipe.GetDepthTarget().resource
            has_depth = depth != rd.ResourceId()

            targets = [(colour, False)]
            if has_depth:
                targets.append((depth, True))

            probes = list(records[0]['probes'].keys())
            start = time.time()
            listed = 0
            depth_diff = 0.0
            for target, depth_target in targets:
                for probe in probes:
                    n, diff = self.check_history(batch_name, events, target, depth_target, has_depth, probe,
                                                 last.eventId)
                    listed += n
                    depth_diff = max(depth_diff, diff)
            elapsed = time.time() - start
            rdtest.log.success(f"{batch_name}: {len(targets) * len(probes)} pixel histories list {listed} "
                               f"events, each with the colour, depth and stencil the GPU had natively before "
                               f"and after it (depth within {depth_diff:.3g}), in {elapsed:.1f}s")

            # only the events up to the current one
            simple = next(e for e in events if e[2] == 'Simple Test')
            self.set_event(simple[0], True)
            for probe in [(110, 100), (170, 140), (60, 30)]:
                self.check_history(batch_name, events, colour, False, has_depth, probe, simple[0])
            rdtest.log.success(f"{batch_name}: at 'Simple Test', only the events up to it are listed")
