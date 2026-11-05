#!/usr/bin/env python3
# Checks how RenderDoc decodes each D3D9 texture format against what the GPU samples, with a
# D3D9_Texture_Formats capture and the reference that demo writes. Run it with the renderdoc python
# module from a build, like d3d9_replay_check.py:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_formats_check.py capture.rdc [D3D9_Texture_Formats_reference.txt]
#
# The reference defaults to D3D9_Texture_Formats_reference.txt next to the capture, then in the
# current directory. It's the demo's float render target from a native run: each format's 16x16
# texture drawn 1:1 with point sampling, so it holds what the GPU's sampler returns for each texel.
#
# For each format the demo drew, it compares:
# - every texel's PickPixel on the texture (RenderDoc's CPU decoding, the values the texture viewer
#   shows) with the reference. Formats are decoded to the channels they're described with, which
#   isn't always how D3D9 samples them (L8 samples as LLL1 but decodes to L001), so MAPPING lists
#   which decoded channel should equal which sampled one. When a format fails it also prints which
#   sampled channels each decoded channel matches, e.g. "decoded r = GPU g", which shows a channel
#   order mix-up such as ATI2's red and green swapped.
# - the render target at replay with the reference, which checks the capture reproduces the texture
#   data and the GPU samples it the same way at replay.
#
# It prints PASS or FAIL per format with the largest difference, and a summary at the end.

import os
import re
import struct
import sys

import renderdoc as rd

# (decoded channel, sampled channel) pairs that should be equal. None: not compared, only printed.
RGBA = [(0, 0), (1, 1), (2, 2), (3, 3)]
LUM = [(0, 0), (0, 1), (0, 2), (3, 3)]  # decoded (L, 0, 0, 1), sampled (L, L, L, 1)
LUM_ALPHA = [(0, 0), (0, 1), (0, 2), (1, 3)]  # decoded (L, A, 0, 1), sampled (L, L, L, A)
TWO = [(0, 0), (1, 1), (3, 3)]  # decoded (x, y, 0, 1), sampled (x, y, 1, 1)
ONE = [(0, 0), (3, 3)]  # decoded (x, 0, 0, 1), sampled (x, 1, 1, 1)

MAPPING = {
    'L8': LUM,
    'L16': LUM,
    'A8L8': LUM_ALPHA,
    'A4L4': LUM_ALPHA,
    'V8U8': TWO,
    'V16U16': TWO,
    'G16R16': TWO,
    'G16R16F': TWO,
    'G32R32F': TWO,
    'R16F': ONE,
    'R32F': ONE,
    # what D3D9 returns in the other channels of these isn't documented
    'ATI1': [(0, 0)],
    'ATI2': [(0, 0), (1, 1)],
    # palette indices, and YUV decoded as (Cr, Y, Cb) rather than converted to RGB
    'P8': None,
    'A8P8': None,
    'YUY2': None,
    'UYVY': None,
}

BLOCK_FORMATS = ['DXT1', 'DXT2', 'DXT3', 'DXT4', 'DXT5', 'ATI1', 'ATI2']
FLOAT_FORMATS = ['R16F', 'G16R16F', 'A16B16G16R16F', 'R32F', 'G32R32F', 'A32B32G32R32F']


def tolerance(name):
    # block decoders round their interpolated colours differently from GPUs: NVIDIA's DXT green and
    # its BC4/BC5 6-value mode differ from the D3D spec's interpolation by up to 0.024
    if name in BLOCK_FORMATS:
        return 0.03
    if name in FLOAT_FORMATS:
        return 1e-5
    return 0.01


def parse_reference(path):
    refs = {}
    cur = None
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == 'format':
                cur = {'name': parts[1], 'x': int(parts[2]), 'y': int(parts[3]),
                       'w': int(parts[4]), 'h': int(parts[5]), 'rows': []}
                refs[cur['name']] = cur
            elif cur is not None:
                vals = [float(v) for v in parts]
                cur['rows'].append([vals[i:i + 4] for i in range(0, len(vals), 4)])
    return refs


def find_formats(actions, sdfile, out, unsupported):
    for a in actions:
        name = a.GetName(sdfile)
        m = re.match(r'^(\S+) at (\d+),(\d+)$', name)
        if m and a.children:
            draws = [c for c in a.children if c.flags & rd.ActionFlags.Drawcall]
            if draws:
                out.append((m.group(1), int(m.group(2)), int(m.group(3)), draws[0]))
        elif name.startswith('Not supported: '):
            unsupported.append(name[len('Not supported: '):])
        find_formats(a.children, sdfile, out, unsupported)
    return out


def get_texture(controller, resid):
    return next(t for t in controller.GetTextures() if t.resourceId == resid)


def fmtv(v):
    return '(' + ', '.join('{:.6g}'.format(x) for x in v) + ')'


def check_format(controller, name, draw, ref):
    controller.SetFrameEvent(draw.eventId, True)
    pipe = controller.GetPipelineState()
    reads = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
    if not reads:
        print('FAIL {}: no texture bound at event {}'.format(name, draw.eventId))
        return False
    tex = reads[0].descriptor.resource
    desc = get_texture(controller, tex)

    decoded = []
    for y in range(ref['h']):
        decoded.append([list(controller.PickPixel(tex, x, y, rd.Subresource(0, 0, 0),
                                                  rd.CompType.Typeless).floatValue)
                        for x in range(ref['w'])])

    mapping = MAPPING.get(name, RGBA)
    if mapping is None:
        print('INFO {:14s} not compared ({}): texel 0,0 decoded {}, GPU {}'.format(
            name, desc.format.Name(), fmtv(decoded[0][0]), fmtv(ref['rows'][0][0])))
        return None

    tol = tolerance(name)
    maxdiff = 0.0
    bad = []
    for y in range(ref['h']):
        for x in range(ref['w']):
            d = decoded[y][x]
            s = ref['rows'][y][x]
            diff = max(abs(d[dc] - s[sc]) for dc, sc in mapping)
            maxdiff = max(maxdiff, diff)
            if diff > tol:
                bad.append((x, y))

    channels = ','.join('rgba'[dc] + ('' if dc == sc else '=' + 'rgba'[sc]) for dc, sc in mapping)
    if not bad:
        print('PASS {:14s} {} texels, max diff {:.6f} ({}, compared {})'.format(
            name, ref['w'] * ref['h'], maxdiff, desc.format.Name(), channels))
        return True

    x, y = bad[0]
    print('FAIL {:14s} {} of {} texels differ by more than {}, max diff {:.6f} ({}, compared {})'.format(
        name, len(bad), ref['w'] * ref['h'], tol, maxdiff, desc.format.Name(), channels))
    print('       first at {},{}: decoded {}, GPU {}'.format(x, y, fmtv(decoded[y][x]),
                                                          fmtv(ref['rows'][y][x])))
    # which sampled channels each decoded channel agrees with on every texel
    for dc in range(4):
        matches = []
        for sc in range(4):
            if all(abs(decoded[yy][xx][dc] - ref['rows'][yy][xx][sc]) <= tol
                   for yy in range(ref['h']) for xx in range(ref['w'])):
                matches.append('rgba'[sc])
        print('       decoded {} = GPU {}'.format('rgba'[dc], ','.join(matches) if matches else 'none'))
    return False


def check_replay_target(controller, found, refs):
    # the render target after the last format's draw holds every format's region
    name, x, y, draw = found[-1]
    controller.SetFrameEvent(draw.eventId, True)
    targets = [t.resource for t in controller.GetPipelineState().GetOutputTargets()
               if t.resource != rd.ResourceId.Null()]
    if not targets:
        print('FAIL replay render target: none bound at event {}'.format(draw.eventId))
        return False
    desc = get_texture(controller, targets[0])
    data = bytes(controller.GetTextureData(targets[0], rd.Subresource(0, 0, 0)))
    if len(data) != desc.width * desc.height * 16:
        print('FAIL replay render target: {} bytes for {}x{} {}'.format(
            len(data), desc.width, desc.height, desc.format.Name()))
        return False

    failed = []
    maxdiff = 0.0
    for name, x, y, draw in found:
        ref = refs.get(name)
        if ref is None:
            continue
        worst = 0.0
        for ty in range(ref['h']):
            for tx in range(ref['w']):
                off = ((ref['y'] + ty) * desc.width + ref['x'] + tx) * 16
                got = struct.unpack_from('4f', data, off)
                worst = max([worst] + [abs(got[c] - ref['rows'][ty][tx][c]) for c in range(4)])
        maxdiff = max(maxdiff, worst)
        if worst > 1e-6:
            failed.append('{} ({:.6f})'.format(name, worst))

    if failed:
        print('FAIL replay render target differs from the native one for: ' + ', '.join(failed))
        return False
    print('PASS replay render target matches the native one for all {} formats (max diff {:.6g})'.format(
        len(found), maxdiff))
    return True


def main():
    if len(sys.argv) < 2:
        print('usage: d3d9_formats_check.py capture.rdc [reference.txt]')
        return 1

    capture = sys.argv[1]
    refpath = sys.argv[2] if len(sys.argv) > 2 else None
    if refpath is None:
        for p in [os.path.join(os.path.dirname(os.path.abspath(capture)),
                               'D3D9_Texture_Formats_reference.txt'),
                  'D3D9_Texture_Formats_reference.txt']:
            if os.path.exists(p):
                refpath = p
                break
    if refpath is None:
        print('No D3D9_Texture_Formats_reference.txt found, pass its path')
        return 1

    refs = parse_reference(refpath)
    print('Reference {}: {} formats'.format(refpath, len(refs)))

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture, '', None) != rd.ResultCode.Succeeded:
        print('Couldn\'t open ' + capture)
        return 1
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1

    if controller.GetAPIProperties().pipelineType != rd.GraphicsAPI.D3D9:
        print('Not a D3D9 capture')

    unsupported = []
    found = find_formats(controller.GetRootActions(), controller.GetStructuredFile(), [], unsupported)

    passed, failed, info = [], [], []
    for name, x, y, draw in found:
        ref = refs.get(name)
        if ref is None:
            print('FAIL {}: not in the reference'.format(name))
            failed.append(name)
            continue
        if (ref['x'], ref['y']) != (x, y):
            print('FAIL {}: drawn at {},{} in the capture but {},{} in the reference'.format(
                name, x, y, ref['x'], ref['y']))
            failed.append(name)
            continue
        ok = check_format(controller, name, draw, ref)
        (passed if ok is True else failed if ok is False else info).append(name)

    missing = [n for n in refs if n not in [f[0] for f in found]]
    for n in missing:
        print('FAIL {}: in the reference but not drawn in the capture'.format(n))
        failed.append(n)

    replay_ok = check_replay_target(controller, found, refs) if found else False

    print('\n{} formats passed, {} failed{}, {} not compared{}'.format(
        len(passed), len(failed), ' (' + ', '.join(failed) + ')' if failed else '', len(info),
        ' (' + ', '.join(info) + ')' if info else ''))
    print('Not supported by the GPU: ' + (', '.join(unsupported) if unsupported else 'none'))
    print('Replay render target: ' + ('PASS' if replay_ok else 'FAIL'))

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0 if not failed and replay_ok else 2


if __name__ == '__main__':
    sys.exit(main())
