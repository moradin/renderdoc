#!/usr/bin/env python3
# A vertex and pixel debugging session on a D3D9_Shader_Debug_Zoo capture through the python
# bindings alone, checking each part of the API the shader viewer uses:
#
#   python debug_api.py <capture.rdc> <D3D9_Shader_Debug_Zoo_pixels.txt>
#
# with the renderdoc module from a build on PYTHONPATH and the build's folder (with renderdoc.dll)
# in RENDERDOC_DLL_DIR, since python 3.8 and later don't look for DLLs on PATH. It also runs inside
# qrenderdoc's embedded python 3.6 (through a wrapper that sets sys.argv), so it stays 3.6
# compatible. Prints what it found and ends with the number of failed checks.
import os
import struct
import sys

if 'RENDERDOC_DLL_DIR' in os.environ and hasattr(os, 'add_dll_directory'):
    os.add_dll_directory(os.environ['RENDERDOC_DLL_DIR'])

import renderdoc as rd

failures = []


def name(e):
    # python 3.11 and later print an IntEnum as its number
    return getattr(e, 'name', str(e))


def check(cond, text):
    print(('  ok   ' if cond else '  FAIL ') + text)
    if not cond:
        failures.append(text)


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def find_marker_draw(controller, name):
    for a in flatten(controller.GetRootActions(), []):
        if a.customName == name:
            for c in flatten(a.children, []):
                if c.flags & rd.ActionFlags.Drawcall:
                    return c
    raise RuntimeError('no marker ' + name)


def float_of(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]


def read_reference(path):
    ref = {}
    with open(path) as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) == 11:
                ref.setdefault(p[0], []).append(((int(p[1]), int(p[2])), [float_of(h) for h in p[3:]]))
    return ref


def run_trace(controller, trace):
    """Every state, stepping with ContinueDebug until it returns nothing, and the registers'
    final values by name, from the first state and each step's changes."""
    states = []
    while True:
        batch = controller.ContinueDebug(trace.debugger)
        if len(batch) == 0:
            break
        states += batch
    values = {}
    for s in states:
        for c in s.changes:
            values[c.after.name] = c.after
    return states, values


def check_states(states, trace):
    check(all(type(s) is rd.ShaderDebugState for s in states), 'ContinueDebug gives ShaderDebugState')
    check([s.stepIndex for s in states] == list(range(len(states))),
          '%d states, stepIndex 0 to %d in order' % (len(states), len(states) - 1))
    check(all(s.nextInstruction < len(trace.instInfo) for s in states),
          'every nextInstruction has an instInfo entry')
    # every change after the first state names a register and has before and after
    changes = [c for s in states[1:] for c in s.changes]
    check(len(changes) > 0 and all(type(c) is rd.ShaderVariableChange for c in changes) and
          all(c.before.name == c.after.name for c in changes),
          '%d changes after the first state, each before and after the same register' % len(changes))
    check(all(all(type(f) is str for f in s.callstack) and list(s.callstack)[0:1] == ['main']
              for s in states), 'every callstack is a list of str starting with "main"')


def describe_trace(trace):
    print('  stage %s, debugger %s' % (name(trace.stage), type(trace.debugger).__name__))
    print('  inputs: %s' % ', '.join('%s (%dx%d %s)' % (v.name, v.rows, v.columns, name(v.type))
                                     for v in trace.inputs))
    print('  constantBlocks: %s' % ', '.join('%s [%d]' % (v.name, len(v.members))
                                             for v in trace.constantBlocks))
    print('  readOnlyResources: %s' % ', '.join(v.name for v in trace.readOnlyResources))
    print('  samplers: %s' % ', '.join(v.name for v in trace.samplers))
    print('  readWriteResources: %d' % len(trace.readWriteResources))
    print('  sourceVars: %d, e.g. %s' % (len(trace.sourceVars), ', '.join(
        '%s -> %s' % (sv.name, ' '.join('%s.%d(%s)' % (r.name, r.component, name(r.type)) for r in sv.variables))
        for sv in trace.sourceVars[0:4])))
    print('  instInfo: %d, first lines %s' % (len(trace.instInfo), [
        (i.instruction, i.lineInfo.disassemblyLine) for i in trace.instInfo[0:4]]))


def reflection(controller, stage):
    refl = controller.GetPipelineState().GetShaderReflection(stage)
    di = refl.debugInfo
    print('  reflection: %s, entry %s, encoding %s, source files %d (%s), source info %s, debuggable %s, '
          'status "%s"' % (name(refl.stage), refl.entryPoint, name(refl.encoding), len(di.files),
                           name(di.encoding), di.sourceDebugInformation, di.debuggable, di.debugStatus))
    return refl


def vertex_session(controller):
    print('vertex: "vs_2_x calls", the first vertex')
    draw = find_marker_draw(controller, 'vs_2_x calls')
    controller.SetFrameEvent(draw.eventId, True)
    refl = reflection(controller, rd.ShaderStage.Vertex)
    check(refl.debugInfo.debuggable, 'the vertex shader is debuggable')

    trace = controller.DebugVertex(0, 0, draw.vertexOffset, 0)
    check(type(trace) is rd.ShaderDebugTrace and type(trace.debugger) is rd.ShaderDebugger,
          'DebugVertex gives a ShaderDebugTrace with a ShaderDebugger')
    describe_trace(trace)
    check(trace.stage == rd.ShaderStage.Vertex, 'the trace is a vertex shader\'s')
    states, values = run_trace(controller, trace)
    check_states(states, trace)
    check(len(controller.ContinueDebug(trace.debugger)) == 0, 'ContinueDebug after the end gives nothing')
    deepest = max(len(s.callstack) for s in states)
    check(deepest >= 3, 'the deepest call stack is %s' % max(states, key=lambda s: len(s.callstack)).callstack)

    # the debugged oPos against VS Out's first vertex
    postvs = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    data = controller.GetBufferData(postvs.vertexResourceId, postvs.vertexByteOffset, postvs.vertexByteStride)
    vsout = list(struct.unpack_from('4f', data, 0))
    opos = values['oPos'].value.f32v[0:4] if 'oPos' in values else None
    check(opos is not None and list(opos) == vsout, 'oPos %s is VS Out\'s position %s' % (opos, vsout))
    controller.FreeTrace(trace)


def pixel_session(controller, ref, marker, inputs_list):
    (x, y), gpu = ref[marker][0]
    print('pixel: "%s" at (%d, %d)' % (marker, x, y))
    draw = find_marker_draw(controller, marker)
    controller.SetFrameEvent(draw.eventId, True)
    refl = reflection(controller, rd.ShaderStage.Pixel)
    check(refl.debugInfo.debuggable, 'the pixel shader is debuggable')
    results = []
    for label, inputs in inputs_list:
        trace = controller.DebugPixel(x, y, inputs)
        check(type(trace) is rd.ShaderDebugTrace and type(trace.debugger) is rd.ShaderDebugger,
              'DebugPixel with %s gives a ShaderDebugTrace with a ShaderDebugger' % label)
        if trace.debugger is None:
            continue
        if len(results) == 0:
            describe_trace(trace)
            check(trace.stage == rd.ShaderStage.Pixel, 'the trace is a pixel shader\'s')
        states, values = run_trace(controller, trace)
        if len(results) == 0:
            check_states(states, trace)
            samples = [s.stepIndex for s in states if s.flags & rd.ShaderEvents.SampleLoadGather]
            print('  steps flagged SampleLoadGather: %s' % samples)
        oc0 = list(values['oC0'].value.f32v[0:4])
        check(oc0 == gpu[0:4], 'with %s oC0 is %s, the GPU wrote %s' % (label, oc0, gpu[0:4]))
        results.append(oc0)
        controller.FreeTrace(trace)
    return results


def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(sys.argv[1], '', None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError('open failed: %s' % result)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError('replay failed: %s' % result)
    ref = read_reference(sys.argv[2])

    print('python %d.%d.%d, API %s' % (sys.version_info[0], sys.version_info[1], sys.version_info[2],
                                       name(controller.GetAPIProperties().pipelineType)))
    props = controller.GetAPIProperties()
    check(props.shaderDebugging, 'APIProperties.shaderDebugging is True')
    check(not props.pixelHistory, 'APIProperties.pixelHistory is False')

    vertex_session(controller)

    default = rd.DebugPixelInputs()
    check(default.sample == rd.ReplayController.NoPreference and default.primitive == rd.ReplayController.NoPreference and
          default.view == rd.ReplayController.NoPreference, 'DebugPixelInputs() is NoPreference for all three')
    pixel_session(controller, ref, 'ps_3_0', [('the defaults', default)])
    pixel_session(controller, ref, 'ps_3_0 anisotropic', [('the defaults', default)])

    # D3D9 debugs the fragment covering the pixel centre whatever sample, primitive and view say
    chosen = []
    for sample in range(4):
        i = rd.DebugPixelInputs()
        i.sample = sample
        i.primitive = 0
        i.view = 0
        chosen.append(('sample %d, primitive 0, view 0' % sample, i))
    results = pixel_session(controller, ref, 'ps_3_0 multisampled', [('the defaults', default)] + chosen)
    check(len(results) == 5 and all(r == results[0] for r in results),
          'sample, primitive and view change nothing')

    print('not debuggable')
    draw = find_marker_draw(controller, 'ps_1_1')
    controller.SetFrameEvent(draw.eventId, True)
    refl = reflection(controller, rd.ShaderStage.Pixel)
    trace = controller.DebugPixel(40, 210, default)
    check(not refl.debugInfo.debuggable and trace.debugger is None,
          'ps_1_1: not debuggable, and DebugPixel gives a trace without a debugger')
    check(len(controller.ContinueDebug(trace.debugger)) == 0, 'ContinueDebug(None) gives nothing')
    controller.FreeTrace(trace)

    for label, fn in (('DebugThread', controller.DebugThread), ('DebugMeshThread', controller.DebugMeshThread)):
        trace = fn((0, 0, 0), (0, 0, 0))
        check(type(trace) is rd.ShaderDebugTrace and trace.debugger is None,
              '%s gives a trace without a debugger' % label)
        controller.FreeTrace(trace)

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    print('%d checks failed' % len(failures))
    for f in failures:
        print('  ' + f)
    return len(failures)


if __name__ == '__main__':
    sys.exit(main())
