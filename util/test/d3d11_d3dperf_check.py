#!/usr/bin/env python3
# Checks that D3DPERF markers show up in D3D11 captures, with a D3D11_D3DPERF_Markers capture. The
# fork's D3D9 driver shares d3d9.dll's D3DPERF hooks with upstream's D3D11 driver, which records
# them as D3D11 markers (driver/d3d11/d3d9_hooks.cpp). Run it with the renderdoc python module from
# a build, like d3d9_replay_check.py:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d11_d3dperf_check.py capture.rdc
#
# It prints the capture's markers and draws, checks they nest as the demo made them, and prints
# the marker colours.

import sys

import renderdoc as rd

# markers as (name, children), draws as 'Draw'
EXPECTED = [
    ('D3DPERF region', [
        'Draw',
        ('D3DPERF marker', []),
        ('D3DPERF nested region', ['Draw']),
    ]),
    'Draw',
    ('Annotation region', ['Draw']),
]

COLOURS = {
    'D3DPERF region': (1.0, 0.0, 0.0),
    'D3DPERF marker': (0.0, 1.0, 0.0),
    'D3DPERF nested region': (0.0, 0.0, 1.0),
}


def tree(actions, sdfile, colours):
    out = []
    for a in actions:
        if a.flags & (rd.ActionFlags.Clear | rd.ActionFlags.Present):
            continue
        if a.flags & rd.ActionFlags.Drawcall:
            out.append('Draw')
        elif a.flags & (rd.ActionFlags.PushMarker | rd.ActionFlags.SetMarker) or a.children:
            name = a.GetName(sdfile)
            c = a.markerColor
            colours[name] = (c.x, c.y, c.z)
            out.append((name, tree(a.children, sdfile, colours)))
    return out


def show(t, depth=0):
    for e in t:
        if isinstance(e, tuple):
            print('  ' * depth + e[0])
            show(e[1], depth + 1)
        else:
            print('  ' * depth + e)


def main():
    if len(sys.argv) < 2:
        print('usage: d3d11_d3dperf_check.py capture.rdc')
        return 1

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(sys.argv[1], '', None) != rd.ResultCode.Succeeded:
        print('Couldn\'t open ' + sys.argv[1])
        return 1
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1

    props = controller.GetAPIProperties()
    print('pipelineType ' + str(props.pipelineType))

    colours = {}
    got = tree(controller.GetRootActions(), controller.GetStructuredFile(), colours)
    print('Markers and draws (clears and Present left out):')
    show(got, 1)

    ok = props.pipelineType == rd.GraphicsAPI.D3D11 and got == EXPECTED
    if not ok:
        print('Expected:')
        show(EXPECTED, 1)

    for name, want in COLOURS.items():
        have = colours.get(name)
        print('Colour of {}: {} (the demo set {})'.format(
            name, '-' if have is None else '({:.3g}, {:.3g}, {:.3g})'.format(*have), want))

    print('\nD3DPERF markers in the D3D11 capture: ' + ('PASS' if ok else 'FAIL'))

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0 if ok else 2


if __name__ == '__main__':
    sys.exit(main())
