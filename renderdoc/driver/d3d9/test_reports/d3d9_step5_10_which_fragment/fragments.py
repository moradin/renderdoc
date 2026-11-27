# Debugs every probe of the zoo's which-fragment draws in a capture and says, per draw, how many of
# the pixels' oC0 and oC1 are the GPU's bit for bit (natively, from the demo's reference). The
# replay's choices are switched with environment variables a temporary build reads:
#   RD_FRAG_REPLACEMENT  a replacement shader, not the original with its inputs appended
#   RD_FRAG_NODEPTHCOPY  no copy of the depth stencil: depth and stencil writes off
#   RD_FRAG_FLOATC0      the original's oC0 to an A32B32G32R32F target, not the draw's format
# usage: python fragments.py <capture> <pixels.txt> <label>
import math, os, struct, sys

ROOT = r'E:\Projects\renderdoc\x64\Development'
os.add_dll_directory(ROOT)
sys.path.insert(0, ROOT + r'\py3module\pymodules')
import renderdoc as rd

DRAWS = ['ps_3_0 depth', 'ps_2_0 texkill', 'ps_2_x alpha test', 'ps_3_0 stencil',
         'ps_3_0 alpha test 8-bit', 'ps_2_x point sprites', 'ps_3_0 centroid']
UNORM8 = {'ps_3_0 alpha test 8-bit': ['oC0']}


def bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def unorm8(v):
    c = math.floor(min(max(v, 0.0), 1.0) * 255.0 + 0.5)
    return struct.unpack('<f', struct.pack('<f', c / 255.0))[0]


def f32(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]


cap_path, ref_path, label = sys.argv[1], sys.argv[2], sys.argv[3]
ref = {}
for line in open(ref_path):
    p = line.rstrip('\n').split('\t')
    if p[0] in DRAWS:
        v = [f32(h) for h in p[3:]]
        ref.setdefault(p[0], []).append(((int(p[1]), int(p[2])), v[:4], v[4:8]))

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
cap.OpenFile(cap_path, '', None)
status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded, str(status)


def find(name):
    def walk(actions):
        for a in actions:
            if a.customName == name:
                for c in a.children:
                    if c.flags & rd.ActionFlags.Drawcall:
                        return c
            r = walk(a.children)
            if r:
                return r
        return None
    return walk(controller.GetRootActions())


print(label)
for name in DRAWS:
    draw = find(name)
    controller.SetFrameEvent(draw.eventId, True)
    right = 0
    detail = []
    for (x, y), n0, n1 in ref[name]:
        trace = controller.DebugPixel(x, y, rd.DebugPixelInputs())
        if trace.debugger is None:
            detail.append(f'({x},{y}) not debugged')
            controller.FreeTrace(trace)
            continue
        vals = {}
        while True:
            states = controller.ContinueDebug(trace.debugger)
            if len(states) == 0:
                break
            for s in states:
                for c in s.changes:
                    vals[c.after.name] = list(c.after.value.f32v[0:4])
        controller.FreeTrace(trace)
        ok = True
        for reg, native in (('oC0', n0), ('oC1', n1)):
            got = vals.get(reg, [0.0] * 4)
            if reg in UNORM8.get(name, []):
                got = [unorm8(v) for v in got]
            if [bits(v) for v in got] != [bits(v) for v in native]:
                ok = False
        right += ok
        if not ok:
            detail.append(f'({x},{y}) id {vals.get("oC1", [0, 0, 0])[2]:g}, the GPU wrote {n1[2]:g}')
    print(f'  {name}: {right} of {len(ref[name])} pixels the GPU\'s' + (': ' + '; '.join(detail) if detail else ''))

controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
