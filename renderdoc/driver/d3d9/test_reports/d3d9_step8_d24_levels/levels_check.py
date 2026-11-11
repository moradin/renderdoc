# python levels_check.py <capture> : the D3D9_Capture_Edge_Cases depth levels, as this build
# replays them: the first 8 values read at the start of the frame, and which pixels of the ZFUNC
# EQUAL draw aren't green
import os
import struct
import sys

sys.path.insert(0, r'E:\Projects\renderdoc\x64\Development\py3module\pymodules')
sys.path.insert(0, r'E:\Projects\renderdoc\util\test')
os.add_dll_directory(r'E:\Projects\renderdoc\x64\Development')
import renderdoc as rd
import d3d9_replay_check

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
res = cap.OpenFile(sys.argv[1], '', None)
if res != rd.ResultCode.Succeeded:
    print('open failed', res)
    sys.exit(1)
res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded:
    print('replay failed', res)
    sys.exit(1)


def find(actions, name):
    for a in actions:
        if a.customName == name or a.GetName(controller.GetStructuredFile()) == name:
            return a
        r = find(a.children, name)
        if r is not None:
            return r
    return None


marker = find(controller.GetRootActions(), 'Depth levels')
draw = [c for c in marker.children if c.flags & rd.ActionFlags.Drawcall][0]
tex = [t for t in controller.GetTextures() if t.resourceId == draw.depthOut][0]

controller.SetFrameEvent(0, True)
values = d3d9_replay_check.depth_values(controller, tex, 0)
print('first 8 values at the start of the frame:', ' '.join('0x%08X' % v for v in values[0:8]))
for name, idx in (('INTZ', None),):
    pass

controller.SetFrameEvent(draw.eventId, True)
data = controller.GetTextureData(draw.outputs[0], rd.Subresource(0, 0, 0))
px = struct.unpack('<4096I', data[0:4096 * 4])
bad = [i for i, v in enumerate(px) if v != 0xFF00FF00]
print('not green after the ZFUNC EQUAL draw: %d of 4096' % len(bad), bad[:20])
for i in bad[:20]:
    print('  pixel %d: colour 0x%08X, depth stencil value 0x%08X' % (i, px[i], values[i]))

# INTZ, from the INTZ marker-less resource list: the texture named Depth Texture 24
for r in controller.GetResources():
    if r.name == 'Depth Texture 24':
        t = [t for t in controller.GetTextures() if t.resourceId == r.resourceId][0]
        controller.SetFrameEvent(0, True)
        v = d3d9_replay_check.depth_values(controller, t, 0)
        print('INTZ (Depth Texture 24) values:', sorted(set(v)))

controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
