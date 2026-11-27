# prints pixel histories of the D3D9_Pixel_History capture given, as the API returns them
import os, sys, time
os.add_dll_directory(r"E:\Projects\renderdoc\x64\Development")
sys.path.insert(0, r"E:\Projects\renderdoc\x64\Development\py3module\pymodules")
import renderdoc as rd

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


batch = find('Batch: Basic')
last = batch
while last.children:
    last = last.children[-1]
c.SetFrameEvent(last.eventId, True)
pipe = c.GetPipelineState()
col = pipe.GetOutputTargets()[0].resource
ds = pipe.GetDepthTarget().resource
print('APIProperties.pixelHistory', c.GetAPIProperties().pixelHistory)
print('at event', last.eventId, 'colour target', col, 'depth target', ds)


def val(v, depth):
    s = ''
    if not depth:
        s += '(%.4f, %.4f, %.4f, %.4f)' % tuple(v.col.floatValue)
    if v.depth != -1.0 or v.stencil != -1:
        s += ' depth %.8f stencil %d' % (v.depth, v.stencil)
    return s


for (x, y) in [(275, 258), (110, 100), (150, 250), (60, 30), (330, 30), (355, 60)]:
    for tex, depth in [(col, False), (ds, True)]:
        t = time.time()
        h = c.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        el = time.time() - t
        print('\n(%d, %d) %s: %d modifications in %.3fs' % (x, y, 'depth stencil' if depth else 'colour', len(h), el))
        for m in h:
            flags = []
            if m.directShaderWrite:
                flags.append('direct write')
            if not m.shaderOut.IsValid():
                flags.append('shaderOut unavailable')
            print('  EID %d %-24s before %s  after %s  primitive %s %s' %
                  (m.eventId, names.get(m.eventId, ''), val(m.preMod, depth), val(m.postMod, depth),
                   'unknown' if m.primitiveID == 0xFFFFFFFF else m.primitiveID, ', '.join(flags)))

c.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
