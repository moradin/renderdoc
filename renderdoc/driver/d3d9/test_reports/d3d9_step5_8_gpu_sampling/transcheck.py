import sys
import math
import struct
from fractions import Fraction as F
sys.path.insert(0, r'E:\Projects\renderdoc\x64\Development\py3module\pymodules')
import os
os.add_dll_directory(r'E:\Projects\renderdoc\x64\Development')
import renderdoc as rd

# The transcendentals draw's pixels that weren't bit for bit: its inputs as the debugger read them,
# then rsq's and sincos's arguments worked out as the bytecode says (fxc's mad, frc, mad before
# sincos), and the results the C library gives for them, against the GPU's native values.
#   mad r0.x, v1.y, v1.y, c0.y(0.01)   rsq oC0.y, r0.x
#   mad r0.x, v2.x, c0.z(0.795774698), c0.w(0.5)   frc r0.x, r0.x
#   mad r0.x, r0.x, c1.x(6.28318548), c1.y(-3.14159274)   sincos r1.xy, r0.x   (oC1.x sin, oC1.y cos)

CAP = sys.argv[1]
REF = sys.argv[2]

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
cap.OpenFile(CAP, '', None)
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


def fma32(a, b, c):
    return r32(F(a) * F(b) + F(c))


def ulps(a, b):
    ia = struct.unpack('<i', struct.pack('<f', a))[0]
    ib = struct.unpack('<i', struct.pack('<f', b))[0]
    return ia - ib


def f32(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]


native = {}
for line in open(REF):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'ps_3_0 transcendentals':
        native[(int(p[1]), int(p[2]))] = [f32(h) for h in p[3:]]

draw = find(controller.GetRootActions(), 'ps_3_0 transcendentals')
controller.SetFrameEvent(draw.eventId, True)

c0z, c0w, c1x, c1y = r32(0.795774698), 0.5, r32(6.28318548), r32(-3.14159274)

for (x, y) in [(140, 210), (141, 210), (140, 211), (141, 211)]:
    trace = controller.DebugPixel(x, y, rd.DebugPixelInputs())
    inp = {v.name: list(v.value.f32v[0:4]) for v in trace.inputs}
    print(sorted(inp.keys()))
    controller.FreeTrace(trace)
    v1y, v2x = r32(inp['v1'][1]), r32(inp['v2'][0])
    nat = native[(x, y)]
    print(f'({x}, {y}) v1.y {v1y!r} v2.x {v2x!r}')

    arg = fma32(v1y, v1y, r32(0.01))
    apart = r32(r32(v1y * v1y) + r32(0.01))
    for name, a in [('mad fused', arg), ('mul, add apart', apart)]:
        cpu = r32(1.0 / math.sqrt(a))
        print(f'  rsq of {name} {a!r}: C library {cpu!r}, {ulps(cpu, nat[1])} ulps from the GPU\'s {nat[1]!r}')

    m = fma32(v2x, c0z, c0w)
    fr = r32(m - math.floor(m))
    a = fma32(fr, c1x, c1y)
    s, c = r32(math.sin(a)), r32(math.cos(a))
    print(f'  sincos of {a!r} (frc {fr!r}): sin {s!r} ({ulps(s, nat[4])} ulps from the GPU\'s {nat[4]!r}), '
          f'cos {c!r} ({ulps(c, nat[5])} ulps from {nat[5]!r})')
    # the same angle without the rounding of the last mad: 2 pi frc - pi, exactly
    exact = 2 * math.pi * fr - math.pi
    s2, c2 = r32(math.sin(exact)), r32(math.cos(exact))
    print(f'  sincos of 2 pi frc - pi unrounded: sin {s2!r} ({ulps(s2, nat[4])} ulps), cos {c2!r} ({ulps(c2, nat[5])} ulps)')

controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
