#!/usr/bin/env python3
# Checks the texture viewer's overlays on D3D9 captures (phase 3e). Run it with the renderdoc
# python module from a build, on a capture of the D3D9_Overlay_Test demo:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_overlay_check.py capture.rdc
#
# For each of the demo's markers it renders overlays through a headless ReplayOutput, the way the
# texture viewer does, saves each overlay texture as <capture>_<marker>_<overlay>.png next to the
# capture, and prints PASS or FAIL for each check: that the overlay texture exists with the
# target's size, the colours at known pixels (see the layout at the top of
# util/test/demos/d3d9/d3d9_overlay_test.cpp), and that rendering an overlay leaves the target as
# it was. It ends with the number of failures, and returns 1 if there were any. The
# D3D9_Overlay_Test test (util/test/tests/D3D9) calls check_capture().

import math
import os
import struct
import sys
import zlib

import renderdoc as rd

EPS = 2.5 / 255.0

results = {'pass': 0, 'fail': 0}


def check(name, ok, detail=''):
    results['pass' if ok else 'fail'] += 1
    print('  {} {}{}'.format('PASS' if ok else 'FAIL', name, (': ' + detail) if detail else ''))
    return ok


def write_png_rgba(path, width, height, rows):
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b''.join(b'\x00' + row for row in rows)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def find_marker_draw(actions, sdfile, name):
    # the last draw inside the marker region called name
    for a in actions:
        if a.GetName(sdfile) == name and a.children:
            draws = [c for c in flatten(a.children, []) if c.flags & rd.ActionFlags.Drawcall]
            if draws:
                return draws[-1]
    return None


def pick(controller, tex, x, y):
    return list(controller.PickPixel(tex, int(x), int(y), rd.Subresource(0, 0, 0),
                                     rd.CompType.Typeless).floatValue)


def fmt(v):
    return '({})'.format(', '.join('{:.3f}'.format(c) for c in v))


def close(v, expected, eps=EPS):
    return all(abs(a - b) <= eps for a, b in zip(v, expected))


class Checker:
    def __init__(self, controller, out, base):
        self.controller = controller
        self.out = out
        self.base = base

    def render(self, target, overlay, label, background=None):
        tex = rd.TextureDisplay()
        tex.resourceId = target
        tex.overlay = overlay
        if background is not None:
            tex.backgroundColor = rd.FloatVector(*background)
        self.out.SetTextureDisplay(tex)
        self.out.Display()
        return self.out.GetDebugOverlayTexID()

    def overlay(self, target, overlay, label, width, height):
        # renders the overlay and checks its texture: returns its ID, or None
        oid = self.render(target, overlay, label)
        name = '{} {}'.format(label, str(overlay).split('.')[-1])
        if not check(name + ': overlay texture exists', oid != rd.ResourceId.Null()):
            return None

        data = self.controller.GetTextureData(oid, rd.Subresource(0, 0, 0))
        if not check(name + ': overlay is {}x{} A8R8G8B8'.format(width, height),
                     len(data) == width * height * 4, '{} bytes'.format(len(data))):
            return oid

        rows = []
        for y in range(height):
            line = data[y * width * 4:(y + 1) * width * 4]
            row = bytearray(width * 4)
            row[0::4] = line[2::4]
            row[1::4] = line[1::4]
            row[2::4] = line[0::4]
            row[3::4] = line[3::4]
            rows.append(bytes(row))
        path = '{}_{}_{}.png'.format(self.base, label.replace(' ', '_'),
                                     str(overlay).split('.')[-1])
        write_png_rgba(path, width, height, rows)
        print('  saved ' + path)
        return oid

    def expect(self, oid, name, x, y, expected, eps=EPS):
        v = pick(self.controller, oid, x, y)
        return check('{} at ({}, {}) is {}'.format(name, int(x), int(y), fmt(expected)),
                     close(v, expected, eps), 'got ' + fmt(v))


RED = [1.0, 0.0, 0.0, 1.0]
GREEN = [0.0, 1.0, 0.0, 1.0]
NONE = [0.0, 0.0, 0.0, 0.0]

# the triangle size overlay's colour ramp (colorRamp in renderdoc/replay/replay_driver.cpp)
COLOR_RAMP = [
    [0.000000, 0.000000, 0.000000, 0.0], [0.250980, 0.000000, 0.250980, 1.0],
    [0.250980, 0.000000, 0.752941, 1.0], [0.000000, 0.250980, 1.000000, 1.0],
    [0.000000, 0.501961, 1.000000, 1.0], [0.000000, 0.627451, 1.000000, 1.0],
    [0.250980, 0.752941, 1.000000, 1.0], [0.250980, 0.878431, 1.000000, 1.0],
    [0.250980, 1.000000, 1.000000, 1.0], [0.250980, 1.000000, 0.752941, 1.0],
    [0.250980, 1.000000, 0.250980, 1.0], [0.501961, 1.000000, 0.250980, 1.0],
    [0.752941, 1.000000, 0.250980, 1.0], [1.000000, 1.000000, 0.250980, 1.0],
    [1.000000, 0.878431, 0.250980, 1.0], [1.000000, 0.627451, 0.250980, 1.0],
    [1.000000, 0.376471, 0.250980, 1.0], [1.000000, 0.125490, 0.250980, 1.0],
    [1.000000, 0.376471, 0.752941, 1.0], [1.000000, 0.627451, 1.000000, 1.0],
    [1.000000, 0.878431, 1.000000, 1.0], [1.000000, 1.000000, 1.000000, 1.0],
]


def tri_size_colour(area):
    # the texture display's HEATMAP_TRISIZE (renderdoc/data/hlsl/texdisplay.hlsl), which the D3D9
    # overlay's pixel shader does itself
    area = max(area, 0.001)
    return COLOR_RAMP[2 + int(math.floor(20.0 - 20.1 * (1.0 - math.exp(-0.4 * area))))]


def main():
    if len(sys.argv) < 2:
        print('usage: d3d9_overlay_check.py capture.rdc')
        return 1

    path = sys.argv[1]

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])

    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, '', None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t open the file: ' + str(result))
        return 1

    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1

    ok = check_capture(controller, os.path.splitext(path)[0])

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0 if ok else 1


def check_capture(controller, base):
    # Runs every check on an open D3D9_Overlay_Test capture, saving the overlays as
    # <base>_<marker>_<overlay>.png. Returns True if they all passed. The D3D9 tests in
    # util/test/tests/D3D9 call this too.
    results['pass'] = results['fail'] = 0

    sdfile = controller.GetStructuredFile()
    roots = controller.GetRootActions()
    textures = {t.resourceId: t for t in controller.GetTextures()}

    out = controller.CreateOutput(rd.CreateHeadlessWindowingData(100, 100),
                                  rd.ReplayOutputType.Texture)
    c = Checker(controller, out, base)

    def at(name):
        draw = find_marker_draw(roots, sdfile, name)
        print('\n{}:'.format(name))
        if draw is None:
            check(name + ': marker with a draw found', False)
            return None, None
        controller.SetFrameEvent(draw.eventId, True)
        target = controller.GetPipelineState().GetOutputTargets()[0].resource
        tex = textures.get(target)
        if tex is None:
            check(name + ': render target found', False)
            return None, None
        print('  event {}, target {} ({}x{})'.format(draw.eventId, target, tex.width, tex.height))
        return target, tex

    # Depth Test: a vs_3_0 quad, x 0.05-0.3, y 0.1-0.9, failing in the top half
    target, tex = at('Depth Test')
    if target:
        W, H = tex.width, tex.height
        inTop, inBottom, outside = (0.175 * W, 0.3 * H), (0.175 * W, 0.7 * H), (0.5 * W, 0.97 * H)

        # what the draw did, to check overlays leave it alone
        before = [pick(controller, target, *p) for p in (inTop, inBottom, outside)]
        # read back again each time, unlike pick pixel's decoded copy
        beforeData = bytes(controller.GetTextureData(target, rd.Subresource(0, 0, 0)))
        check('the draw failed the depth test in the top half (setup grey)',
              close(before[0][:3], [0.1, 0.1, 0.1], 1.5 / 255.0), fmt(before[0]))
        check('and passed in the bottom (green)', close(before[1][:3], [0.0, 0.5, 0.0], 1.5 / 255.0),
              fmt(before[1]))

        oid = c.overlay(target, rd.DebugOverlay.Drawcall, 'Depth Test', W, H)
        if oid:
            c.expect(oid, 'Drawcall inside', *inTop, [0.8, 0.1, 0.8, 1.0])
            c.expect(oid, 'Drawcall outside', *outside, [0.0, 0.0, 0.0, 0.5])

        oid = c.overlay(target, rd.DebugOverlay.Wireframe, 'Depth Test', W, H)
        if oid:
            # away from the edges and the quad's diagonal
            c.expect(oid, 'Wireframe inside', 0.1 * W, 0.3 * H, [200.0 / 255.0, 1.0, 0.0, 0.0])
            # somewhere on the left edge
            edge = [pick(controller, oid, x, 0.7 * H) for x in range(int(0.05 * W) - 2, int(0.05 * W) + 3)]
            check('Wireframe has the left edge near x={}'.format(int(0.05 * W)),
                  any(close(v, [200.0 / 255.0, 1.0, 0.0, 1.0]) for v in edge),
                  ' '.join(fmt(v) for v in edge))

        oid = c.overlay(target, rd.DebugOverlay.Depth, 'Depth Test', W, H)
        if oid:
            c.expect(oid, 'Depth fails in the top half', *inTop, RED)
            c.expect(oid, 'Depth passes in the bottom half', *inBottom, GREEN)
            c.expect(oid, 'Depth outside the draw', *outside, NONE)

        oid = c.overlay(target, rd.DebugOverlay.Stencil, 'Depth Test', W, H)
        if oid:
            c.expect(oid, 'Stencil with the test off passes', *inTop, GREEN)

        # quad overdraw isn't supported: a transparent overlay, so the viewer doesn't keep the last
        for overlay in (rd.DebugOverlay.QuadOverdrawDraw, rd.DebugOverlay.QuadOverdrawPass):
            oid = c.overlay(target, overlay, 'Depth Test', W, H)
            if oid:
                data = bytes(controller.GetTextureData(oid, rd.Subresource(0, 0, 0)))
                check('{} is transparent'.format(str(overlay).split('.')[-1]),
                      len(data) > 0 and not any(data),
                      '{} non-zero bytes'.format(sum(b != 0 for b in data)))

        # The quad's triangles (0.25 x 0.8 of the window, halved) and the setup draw's (the top
        # half, halved) are big, so they're the ramp's first colour. The draw shows where it
        # passes the depth test, and the pass the setup draw in the top half too.
        quadColour = tri_size_colour(0.1 * W * H)
        setupColour = tri_size_colour(0.25 * W * H)
        oid = c.overlay(target, rd.DebugOverlay.TriangleSizeDraw, 'Depth Test', W, H)
        if oid:
            c.expect(oid, 'TriangleSizeDraw fails the depth test in the top half', *inTop, NONE)
            c.expect(oid, 'TriangleSizeDraw in the bottom half', *inBottom, quadColour)
            c.expect(oid, 'TriangleSizeDraw outside the draw', *outside, NONE)
        oid = c.overlay(target, rd.DebugOverlay.TriangleSizePass, 'Depth Test', W, H)
        if oid:
            c.expect(oid, 'TriangleSizePass has the setup draw in the top half', *inTop, setupColour)
            c.expect(oid, 'TriangleSizePass in the bottom half', *inBottom, quadColour)
            c.expect(oid, 'TriangleSizePass outside the pass', *outside, NONE)

        c.render(target, rd.DebugOverlay.NoOverlay, 'Depth Test')
        afterData = bytes(controller.GetTextureData(target, rd.Subresource(0, 0, 0)))
        check('the overlays left the target as it was', len(beforeData) > 0 and beforeData == afterData,
              '{} of {} bytes differ'.format(sum(a != b for a, b in zip(beforeData, afterData)),
                                             len(beforeData)))

    # Depth Bias: a fixed function quad, x 0.35-0.6, which fails in the top half only because of
    # its depth bias
    target, tex = at('Depth Bias')
    if target:
        W, H = tex.width, tex.height
        inTop, inBottom = (0.475 * W, 0.3 * H), (0.475 * W, 0.7 * H)
        v = pick(controller, target, *inTop)
        check('the draw failed in the top half (setup grey)', close(v[:3], [0.1, 0.1, 0.1], 1.5 / 255.0),
              fmt(v))
        oid = c.overlay(target, rd.DebugOverlay.Depth, 'Depth Bias', W, H)
        if oid:
            c.expect(oid, 'Depth fails in the top half', *inTop, RED)
            c.expect(oid, 'Depth passes in the bottom half', *inBottom, GREEN)

    # Stencil Test: x 0.65-0.95, STENCILFUNC EQUAL 1, which passes in the top half
    target, tex = at('Stencil Test')
    if target:
        W, H = tex.width, tex.height
        inTop, inBottom = (0.8 * W, 0.3 * H), (0.8 * W, 0.7 * H)
        oid = c.overlay(target, rd.DebugOverlay.Stencil, 'Stencil Test', W, H)
        if oid:
            c.expect(oid, 'Stencil passes in the top half', *inTop, GREEN)
            c.expect(oid, 'Stencil fails in the bottom half', *inBottom, RED)
        oid = c.overlay(target, rd.DebugOverlay.Depth, 'Stencil Test', W, H)
        if oid:
            c.expect(oid, 'Depth with ZENABLE off passes', *inBottom, GREEN)

        # the clear before overlays change the target itself
        blue = [0.0, 0.0, 1.0, 1.0]
        setupPixel, background = (0.5 * W, 0.05 * H), (0.02 * W, 0.95 * H)
        oid = c.render(target, rd.DebugOverlay.ClearBeforeDraw, 'Stencil Test', blue)
        check('ClearBeforeDraw returns an overlay texture', oid != rd.ResourceId.Null())
        v = pick(controller, target, *setupPixel)
        check('ClearBeforeDraw cleared what the setup draw drew', close(v[:3], blue[:3]), fmt(v))
        v = pick(controller, target, *inTop)
        check('ClearBeforeDraw kept the draw', close(v[:3], [0.5, 0.0, 0.5], 1.5 / 255.0), fmt(v))

        oid = c.render(target, rd.DebugOverlay.ClearBeforePass, 'Stencil Test', blue)
        check('ClearBeforePass returns an overlay texture', oid != rd.ResourceId.Null())
        v = pick(controller, target, *setupPixel)
        check('ClearBeforePass replayed the setup draw', close(v[:3], [0.1, 0.1, 0.1], 1.5 / 255.0),
              fmt(v))
        v = pick(controller, target, *background)
        check('ClearBeforePass cleared the background', close(v[:3], blue[:3]), fmt(v))

        c.render(target, rd.DebugOverlay.NoOverlay, 'Stencil Test')
        v = pick(controller, target, *background)
        check('turning the clear off puts the background back', close(v[:3], [0.2, 0.2, 0.2], 1.5 / 255.0),
              fmt(v))

    # Backface Cull: a clockwise triangle around (0.25, 0.8), kept, and a counter-clockwise one
    # around (0.75, 0.8), culled
    target, tex = at('Backface Cull')
    if target:
        W, H = tex.width, tex.height
        oid = c.overlay(target, rd.DebugOverlay.BackfaceCull, 'Backface Cull', W, H)
        if oid:
            c.expect(oid, 'the front face', 0.25 * W, 0.8 * H, GREEN)
            c.expect(oid, 'the back face', 0.75 * W, 0.8 * H, RED)
            c.expect(oid, 'outside', 0.5 * W, 0.3 * H, NONE)

    # Viewport Scissor: the viewport is the bottom right quarter, the scissor rect x 0.6-0.9,
    # y 0.6-0.9. The overlay blends (0.2, 0.2, 0.9, 0.4) over the viewport, with a 3 pixel border.
    target, tex = at('Viewport Scissor')
    if target:
        W, H = tex.width, tex.height
        vx, vy = int(0.5 * W), int(0.5 * H)
        sx = int(0.6 * W)
        oid = c.overlay(target, rd.DebugOverlay.ViewportScissor, 'Viewport Scissor', W, H)
        if oid:
            c.expect(oid, 'inside the scissor rect', 0.75 * W, 0.75 * H, [0.08, 0.68, 0.36, 0.76],
                     4.0 / 255.0)
            c.expect(oid, 'scissored out', 0.55 * W, 0.95 * H, [0.68, 0.08, 0.36, 0.76], 4.0 / 255.0)
            c.expect(oid, 'outside the viewport', 0.25 * W, 0.25 * H, NONE)
            c.expect(oid, 'the viewport\'s border', vx + 1, 0.75 * H, [0.1, 0.1, 0.1, 1.0])
            v = pick(controller, oid, sx + 1, 0.75 * H)
            check('the scissor rect\'s border is black or white',
                  close(v, [1.0] * 4) or close(v, [0.0, 0.0, 0.0, 1.0]), fmt(v))

        # the triangle sizes are in pixels of the viewport, and keep the scissor test, as on D3D11
        oid = c.overlay(target, rd.DebugOverlay.TriangleSizeDraw, 'Viewport Scissor', W, H)
        if oid:
            c.expect(oid, 'TriangleSizeDraw inside the scissor rect', 0.75 * W, 0.75 * H,
                     tri_size_colour(0.125 * W * H))
            c.expect(oid, 'TriangleSizeDraw scissored out', 0.55 * W, 0.95 * H, NONE)
            c.expect(oid, 'TriangleSizeDraw outside the viewport', 0.25 * W, 0.75 * H, NONE)

    # Triangle Size: a fixed function (pretransformed) strip of triangles with areas of 10, 24 and
    # 7 pixels, from (int(0.1 * W), int(0.92 * H)): (0,0) (0,4) (5,0), then (5,0) (0,4) (12,4),
    # then (5,0) (12,4) (8.5,0)
    target, tex = at('Triangle Size')
    if target:
        W, H = tex.width, tex.height
        bx, by = int(0.1 * W), int(0.92 * H)
        tris = [((bx + 1, by + 1), 10.0), ((bx + 5, by + 3), 24.0), ((bx + 8, by + 1), 7.0)]
        print('  expected ramp entries: ' + ', '.join(
            '{} for {} pixels'.format(COLOR_RAMP.index(tri_size_colour(a)), a) for _, a in tris))

        oid = c.overlay(target, rd.DebugOverlay.TriangleSizeDraw, 'Triangle Size', W, H)
        if oid:
            for (x, y), area in tris:
                c.expect(oid, 'TriangleSizeDraw of a {} pixel triangle'.format(area), x, y,
                         tri_size_colour(area))
            c.expect(oid, 'TriangleSizeDraw outside the strip', bx + 2, by + 6, NONE)
            c.expect(oid, 'TriangleSizeDraw leaves out the earlier draws', 0.175 * W, 0.7 * H, NONE)

            # every pixel the draw covers, and only those, has one of the ramp's colours
            triData = bytes(controller.GetTextureData(oid, rd.Subresource(0, 0, 0)))
            oid = c.overlay(target, rd.DebugOverlay.Drawcall, 'Triangle Size', W, H)
            if oid:
                drawData = bytes(controller.GetTextureData(oid, rd.Subresource(0, 0, 0)))
                ramp = set(tuple(int(round(v * 255.0)) for v in col) for col in COLOR_RAMP[1:])
                covered = mismatched = notRamp = 0
                for i in range(0, min(len(triData), len(drawData)), 4):
                    # BGRA
                    tri = (triData[i + 2], triData[i + 1], triData[i], triData[i + 3])
                    inDraw = drawData[i + 3] == 255
                    covered += inDraw
                    mismatched += inDraw != (tri[3] != 0)
                    notRamp += tri[3] != 0 and tri not in ramp
                # pretransformed positions go through clip space here, so a pixel centre exactly
                # on an edge can come out either way
                check('TriangleSizeDraw covers what Drawcall does', covered > 0 and mismatched <= 3,
                      '{} pixels covered, {} differ'.format(covered, mismatched))
                check('TriangleSizeDraw only has ramp colours', notRamp == 0,
                      '{} pixels aren\'t'.format(notRamp))

        oid = c.overlay(target, rd.DebugOverlay.TriangleSizePass, 'Triangle Size', W, H)
        if oid:
            for (x, y), area in tris:
                c.expect(oid, 'TriangleSizePass of a {} pixel triangle'.format(area), x, y,
                         tri_size_colour(area))
            c.expect(oid, 'TriangleSizePass has the Depth Test draw', 0.175 * W, 0.7 * H,
                     tri_size_colour(0.1 * W * H))
            c.expect(oid, 'TriangleSizePass outside the pass', bx + 2, by + 6, NONE)

    # MSAA: a 256x256 4x target with a triangle (128,20) (236,236) (20,236)
    target, tex = at('MSAA')
    if target:
        W, H = tex.width, tex.height
        check('the MSAA target is multisampled', tex.msSamp > 1, '{} samples'.format(tex.msSamp))
        oid = c.overlay(target, rd.DebugOverlay.Depth, 'MSAA', W, H)
        if oid:
            c.expect(oid, 'Depth passes inside the triangle', 128, 180, GREEN)
            c.expect(oid, 'Depth outside the triangle', 20, 20, NONE)
        oid = c.overlay(target, rd.DebugOverlay.Drawcall, 'MSAA', W, H)
        if oid:
            c.expect(oid, 'Drawcall inside the triangle', 128, 180, [0.8, 0.1, 0.8, 1.0])
        oid = c.overlay(target, rd.DebugOverlay.TriangleSizeDraw, 'MSAA', W, H)
        if oid:
            c.expect(oid, 'TriangleSizeDraw inside the triangle', 128, 180,
                     tri_size_colour(216.0 * 216.0 / 2.0))
            c.expect(oid, 'TriangleSizeDraw outside the triangle', 20, 20, NONE)
    else:
        print('  (the demo skips MSAA when 4x isn\'t supported)')

    print('\n{} passed, {} failed'.format(results['pass'], results['fail']))

    out.Shutdown()
    return results['fail'] == 0


if __name__ == '__main__':
    sys.exit(main())
