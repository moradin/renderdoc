import sys
import struct
from fractions import Fraction as F
sys.path.insert(0, r'E:\Projects\renderdoc\x64\Development\py3module\pymodules')
import os
os.add_dll_directory(r'E:\Projects\renderdoc\x64\Development')
import renderdoc as rd

# ps_2_x's oC0 = v + ddx(v) * 16 with v = t1 * t1.yxwz + v0, at (140, 60), the quad's top left.
# Its inputs and those of (141, 60) are read by debugging each pixel. Then oC0 with the mul and add
# apart (the bytecode) and fused (what the driver may do).

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
cap.OpenFile(r'E:\Projects\renderdoc\util\test\tmp\D3D9_Shader_Debug_Zoo\capture_frame5.rdc', '', None)
res, controller = cap.OpenCapture(rd.ReplayOptions(), None)


def find(actions, marker):
    for a in actions:
        if a.customName == marker:
            return a.children[0]
        r = find(a.children, marker)
        if r:
            return r
    return None


def r32(x):
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]


def hx(x):
    return struct.pack('>f', x).hex()


draw = find(controller.GetRootActions(), 'ps_2_x')
controller.SetFrameEvent(draw.eventId, True)


def inputs(x, y):
    trace = controller.DebugPixel(x, y, rd.DebugPixelInputs())
    ret = {v.name: list(v.value.f32v[0:4]) for v in trace.inputs}
    controller.FreeTrace(trace)
    return ret


tl = inputs(140, 60)
tr = inputs(141, 60)
yxwz = [1, 0, 3, 2]


def v(inp, fused):
    t1, c = inp['t1'], inp['v0']
    out = []
    for i in range(4):
        if fused:
            out.append(r32(F(t1[i]) * F(t1[yxwz[i]]) + F(c[i])))
        else:
            out.append(r32(r32(t1[i] * t1[yxwz[i]]) + c[i]))
    return out


native = {'z': 0.0519179367}
for fused in [False, True]:
    a, b = v(tl, fused), v(tr, fused)
    oc0 = [r32(a[i] + r32(r32(b[i] - a[i]) * 16.0)) for i in range(4)]
    print('fused' if fused else 'apart', ['%.9g' % f for f in oc0], [hx(f) for f in oc0])
print('GPU z 0.0519179367, debugger z 0.051918')

controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
