#!/usr/bin/env python3
# Checks D3D9 post-transform vertices (phase 3e) against the reference D3D9_Mesh_Output writes.
# Run it with the renderdoc python module from a build, like d3d9_replay_check.py:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_mesh_check.py capture.rdc [D3D9_Mesh_Output_reference.txt]
#
# The demo writes the reference to its tmp folder: util/test/tmp/D3D9_Mesh_Output, or
# <dir>/D3D9_Mesh_Output when it's run with --tmp <dir> (_reference_rdoc.txt under RenderDoc). The
# reference defaults to D3D9_Mesh_Output_reference.txt next to the capture, then in the current
# directory. The D3D9_Mesh_Output test (util/test/tests/D3D9) calls check_capture().
#
# For each draw in the reference it finds the draw in the marker of the same name, gets its VS Out
# data with GetPostVSData for each instance, and compares every row with the reference within a
# tolerance, printing PASS or FAIL per draw and the VS In and VS Out vertex counts. A draw whose
# reference failed natively counts as a failure. References from ProcessVertices are screen space (the viewport applied), and their
# colours are 8 bits; ours are converted to match. It also checks the VS In data of
# DrawPrimitiveUP draws can be read.
#
# For lit fixed function draws and scaled points the reference also has 'hw' rows: the colours
# and point sizes the GPU drew, from each vertex drawn on its own. Where they're given, ours are
# compared with those, and ProcessVertices' values are only noted if they differ from the GPU's.

import os
import struct
import sys

import renderdoc as rd


def parse_reference(path):
    viewport = [0.0, 0.0, 1.0, 1.0, 0.0, 1.0]
    draws = []
    cur = None
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == 'viewport':
                viewport = [float(x) for x in parts[1:7]]
            elif parts[0] == 'draw':
                cur = {'name': line[5:].strip(), 'space': 'clip', 'rows': {}, 'hw': {}, 'error': None}
                draws.append(cur)
            elif parts[0] == 'space':
                cur['space'] = parts[1]
            elif parts[0] == 'error':
                cur['error'] = line[6:].strip()
            elif parts[0] in ('row', 'hw'):
                vals = {}
                for kv in parts[3:]:
                    k, v = kv.split('=')
                    vals[k] = [float(x) for x in v.split(',')]
                cur['rows' if parts[0] == 'row' else 'hw'][(int(parts[1]), int(parts[2]))] = vals
    return viewport, draws


def find_draws(actions, sdfile, parent, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            out.append((parent, a))
        if len(a.children) > 0:
            find_draws(a.children, sdfile, a.GetName(sdfile), out)
    return out


def to_screen(p, vp):
    x, y, z, w = p
    if w == 0.0:
        return [float('inf')] * 4
    return [(x / w + 1.0) * 0.5 * vp[2] + vp[0], (1.0 - y / w) * 0.5 * vp[3] + vp[1],
            z / w * (vp[5] - vp[4]) + vp[4], 1.0 / w]


def saturate(v):
    return [min(max(x, 0.0), 1.0) for x in v]


def close(a, b, rel, absolute):
    return abs(a - b) <= absolute + rel * max(abs(a), abs(b))


def compare(key, ours, ref, space, vp, hw=False):
    # returns None if they match, otherwise a description
    n = min(len(ours), len(ref))
    if hw and key == 'PSIZE.0':
        # the width of the square the GPU drew, in whole pixels
        tols = [(0.0, 1.0)]
    elif hw:
        # 8-bit colours, which the GPU may round differently
        ours = saturate(ours)
        tols = [(0.0, 2.5 / 255.0)] * 4
    elif key.startswith('POSITION.') and space == 'screen':
        ours = to_screen(ours, vp)
        tols = [(0.0, 0.05), (0.0, 0.05), (1e-4, 1e-5), (1e-4, 1e-6)]
    elif key.startswith('COLOR.') or key == 'FOG.0':
        if space == 'screen':
            # D3DCOLOR outputs: saturated and 8-bit
            ours = saturate(ours)
            tols = [(0.0, 1.5 / 255.0)] * 4
        else:
            tols = [(1e-4, 1e-4)] * 4
    else:
        tols = [(1e-3, 1e-4)] * 4
    for i in range(n):
        if not close(ours[i], ref[i], tols[i][0], tols[i][1]):
            return '{} ours {} reference {}'.format(key, ['%.6g' % x for x in ours[:n]],
                                                    ['%.6g' % x for x in ref[:n]])
    return None


def read_rows(controller, mesh, sig):
    # the VS Out rows: the position first, then the signature in order, compCount floats each
    posidx = -1
    for i, s in enumerate(sig):
        if s.systemValue == rd.ShaderBuiltin.Position:
            posidx = i
    order = ([posidx] if posidx >= 0 else []) + [i for i in range(len(sig)) if i != posidx]
    stride = mesh.vertexByteStride

    vdata = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
    indices = None
    if mesh.indexResourceId != rd.ResourceId.Null() and mesh.indexByteStride:
        idata = controller.GetBufferData(mesh.indexResourceId, mesh.indexByteOffset, 0)
        fmt = 'I' if mesh.indexByteStride == 4 else 'H'
        count = len(idata) // mesh.indexByteStride
        indices = struct.unpack('<{}{}'.format(count, fmt), idata[:count * mesh.indexByteStride])

    rows = []
    for r in range(mesh.numIndices):
        v = indices[r] if indices is not None else r
        v += mesh.baseVertex
        off = v * stride
        vals = {}
        for o in order:
            s = sig[o]
            n = s.compCount
            if off + n * 4 <= len(vdata):
                vals['{}.{}'.format(s.semanticName, s.semanticIndex)] = list(
                    struct.unpack_from('<{}f'.format(n), vdata, off))
            off += n * 4
        rows.append(vals)
    unique = len(vdata) // stride if stride else 0
    return rows, unique


def check_draw(controller, action, ref, vp):
    name = ref['name']
    if ref['error']:
        print('SKIP {}: no reference ({})'.format(name, ref['error']))
        return None

    controller.SetFrameEvent(action.eventId, True)
    pipe = controller.GetPipelineState()
    refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    if refl is None:
        print('FAIL {}: no vertex shader reflection (or fixed function one)'.format(name))
        return False
    sig = list(refl.outputSignature)

    problems = []
    notes = []
    counts = ''
    for inst in range(max(1, action.numInstances)):
        mesh = controller.GetPostVSData(inst, 0, rd.MeshDataStage.VSOut)
        if mesh.vertexResourceId == rd.ResourceId.Null():
            problems.append('instance {}: no VS Out data: {}'.format(inst, mesh.status))
            continue
        rows, unique = read_rows(controller, mesh, sig)
        if inst == 0:
            counts = 'VS In {} vertices, VS Out {} rows ({} unique), unproject {}'.format(
                action.numIndices, mesh.numIndices, unique, mesh.unproject)
        if mesh.numIndices != action.numIndices:
            problems.append('instance {}: {} rows, expected {}'.format(inst, mesh.numIndices,
                                                                     action.numIndices))
        for (rinst, r), expected in sorted(ref['rows'].items()):
            if rinst != inst:
                continue
            if r >= len(rows):
                problems.append('row {} missing'.format(r))
                continue
            hw = ref['hw'].get((rinst, r), {})
            for key, val in list(expected.items()) + [(k, v) for k, v in hw.items() if k not in expected]:
                if key not in rows[r]:
                    problems.append('instance {} row {}: no {} output (have {})'.format(
                        inst, r, key, sorted(rows[r].keys())))
                    continue
                if key in hw:
                    err = compare(key, rows[r][key], hw[key], ref['space'], vp, hw=True)
                    if err:
                        problems.append('instance {} row {}: (GPU) {}'.format(inst, r, err))
                    if key in expected:
                        pverr = compare(key, hw[key], val, ref['space'], vp)
                        if pverr:
                            notes.append('instance {} row {}: ProcessVertices differs from the GPU: {}'.format(
                                inst, r, pverr.replace('ours', 'GPU', 1)))
                    continue
                err = compare(key, rows[r][key], val, ref['space'], vp)
                if err:
                    problems.append('instance {} row {}: {}'.format(inst, r, err))

    if ref['hw']:
        counts += ', {} rows checked against the GPU'.format(len(ref['hw']))

    if problems:
        print('FAIL {} ({}): {} problems'.format(name, counts, len(problems)))
        for p in problems[:8]:
            print('    ' + p)
    else:
        print('PASS {} ({})'.format(name, counts))
    for n in notes[:4]:
        print('    NOTE ' + n)
    if len(notes) > 4:
        print('    NOTE ... {} more rows where ProcessVertices differs from the GPU'.format(len(notes) - 4))
    return not problems


def check_vs_in(controller, action, name):
    # the mesh viewer's VS In: the streams the pipeline state lists must have data
    controller.SetFrameEvent(action.eventId, True)
    pipe = controller.GetPipelineState()
    vbs = pipe.GetVBuffers()
    if not vbs or vbs[0].resourceId == rd.ResourceId.Null():
        print('FAIL {}: VS In has no stream 0'.format(name))
        return False
    data = controller.GetBufferData(vbs[0].resourceId, vbs[0].byteOffset, 0)
    ib = pipe.GetIBuffer()
    idx = ''
    if action.flags & rd.ActionFlags.Indexed:
        idata = controller.GetBufferData(ib.resourceId, ib.byteOffset, 0) if ib.resourceId != rd.ResourceId.Null() else b''
        idx = ', {} index bytes'.format(len(idata))
        if not idata:
            print('FAIL {}: VS In has no indices'.format(name))
            return False
    if not data:
        print('FAIL {}: VS In stream 0 is empty'.format(name))
        return False
    print('PASS {} VS In ({} vertex bytes, stride {}{})'.format(name, len(data), vbs[0].byteStride, idx))
    return True


def main():
    if len(sys.argv) < 2:
        print('usage: d3d9_mesh_check.py capture.rdc [reference.txt]')
        return 1

    capture = sys.argv[1]
    refpath = sys.argv[2] if len(sys.argv) > 2 else None
    if refpath is None:
        for p in [os.path.join(os.path.dirname(os.path.abspath(capture)), 'D3D9_Mesh_Output_reference.txt'),
                  'D3D9_Mesh_Output_reference.txt']:
            if os.path.exists(p):
                refpath = p
                break
    if refpath is None:
        print('No D3D9_Mesh_Output_reference.txt found, pass its path')
        return 1

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(capture, '', None) != rd.ResultCode.Succeeded:
        print('Couldn\'t open ' + capture)
        return 1
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1

    ok = check_capture(controller, refpath)

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0 if ok else 2


def check_capture(controller, refpath):
    # Checks every draw in the reference on an open D3D9_Mesh_Output capture. Returns True if they
    # all passed. The D3D9 tests in util/test/tests/D3D9 call this too.
    vp, refs = parse_reference(refpath)
    print('Reference {}: {} draws, viewport {}'.format(refpath, len(refs), vp))

    sdfile = controller.GetStructuredFile()
    draws = find_draws(controller.GetRootActions(), sdfile, '', [])

    passed = failed = skipped = 0
    for ref in refs:
        found = [a for parent, a in draws if parent == ref['name']]
        if not found:
            print('FAIL {}: no draw in a marker of that name'.format(ref['name']))
            failed += 1
            continue
        ok = check_draw(controller, found[0], ref, vp)
        if ok is True:
            passed += 1
        elif ok is False:
            failed += 1
        else:
            skipped += 1
        if 'PrimitiveUP' in ref['name']:
            if check_vs_in(controller, found[0], ref['name']):
                passed += 1
            else:
                failed += 1

    print('\n{} passed, {} failed, {} skipped (no reference)'.format(passed, failed, skipped))
    # a draw whose reference failed natively isn't checked, so it counts against the result
    return failed == 0 and skipped == 0 and len(refs) > 0


if __name__ == '__main__':
    sys.exit(main())
