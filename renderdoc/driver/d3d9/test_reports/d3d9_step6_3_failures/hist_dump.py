# prints pixel histories of the D3D9_Pixel_History capture given, with why each draw failed, as
# the API returns them, and how long each took
import os, sys, time
os.add_dll_directory(r"E:\Projects\renderdoc\x64\Development")
sys.path.insert(0, r"E:\Projects\renderdoc\x64\Development\py3module\pymodules")
import renderdoc as rd

FLAGS = ['backfaceCulled', 'viewClipped', 'scissorClipped', 'sampleMasked', 'shaderDiscarded',
         'depthTestFailed', 'stencilTestFailed', 'depthClipped', 'depthBoundsFailed', 'predicationSkipped']

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
cap.OpenFile(sys.argv[1], '', None)
r, c = cap.OpenCapture(rd.ReplayOptions(), None)

names = {}


def walk(actions, marker):
    for a in actions:
        if a.flags & rd.ActionFlags.SetMarker:
            marker[0] = a.customName
        names[a.eventId] = marker[0]
        walk(a.children, marker)


walk(c.GetRootActions(), [''])


def find(name):
    def w(actions):
        for a in actions:
            if a.customName == name:
                return a
            x = w(a.children)
            if x:
                return x
    return w(c.GetRootActions())


def history(batch_name, probes, depth=False, brief=False):
    last = find('Batch: ' + batch_name)
    while last.children:
        last = last.children[-1]
    c.SetFrameEvent(last.eventId, True)
    pipe = c.GetPipelineState()
    tex = pipe.GetDepthTarget().resource if depth else pipe.GetOutputTargets()[0].resource
    for (x, y) in probes:
        t = time.time()
        h = c.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        el = time.time() - t
        print('\n%s %s (%d, %d): %d modifications in %.3fs' %
              (batch_name, 'depth stencil' if depth else 'colour', x, y, len(h), el))
        if brief:
            failed = sum(1 for m in h if not m.Passed())
            print('  %d failed' % failed)
            continue
        for m in h:
            flags = [f for f in FLAGS if getattr(m, f)]
            print('  EID %-5d %-22s %s' % (m.eventId, names.get(m.eventId, ''), ', '.join(flags) or 'passed'))


history('Basic', [(170, 140), (170, 160), (150, 250), (106, 248), (26, 268), (46, 268), (66, 268), (86, 268),
                  (106, 268), (106, 282), (126, 268), (146, 268)])
history('Basic', [(170, 140), (106, 268)], depth=True)
history('Colour Only', [(170, 140), (106, 268), (146, 268)])
history('Multisampled', [(146, 268), (166, 268), (126, 268)])
# the pixel the 300 draws cover
history('Basic', [(60, 130)], brief=True)
history('Basic', [(60, 130)], depth=True, brief=True)

c.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
