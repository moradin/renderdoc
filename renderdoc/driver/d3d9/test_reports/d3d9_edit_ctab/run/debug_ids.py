# Round trip each shader of the D3D9_Shader_Editing capture, printing IDs and constant names at
# each step. usage: python debug_ids.py <capture>
import os
import sys
sys.path.insert(0, 'E:/Projects/renderdoc/x64/Development/py3module/pymodules')
os.environ['PATH'] = 'E:/Projects/renderdoc/x64/Development;' + os.environ['PATH']
os.add_dll_directory('E:/Projects/renderdoc/x64/Development')
import renderdoc as rd

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(sys.argv[1], '', None) == rd.ResultCode.Succeeded
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
assert res == rd.ResultCode.Succeeded, res


def names(refl):
    return ([(b.name, [v.name for v in b.variables]) for b in refl.constantBlocks], [s.name for s in refl.samplers])


shaders = {}


def walk(act):
    if act.flags & rd.ActionFlags.Drawcall:
        ctrl.SetFrameEvent(act.eventId, True)
        p = ctrl.GetD3D9PipelineState()
        for st, sh in (('vs', p.vertexShader), ('ps', p.pixelShader)):
            if sh.resourceId != rd.ResourceId.Null() and sh.resourceId not in shaders:
                shaders[sh.resourceId] = (act.eventId, st, sh.reflection)
    for c in act.children:
        walk(c)


for a in ctrl.GetRootActions():
    walk(a)

for id, (eid, st, refl) in shaders.items():
    print('==', id, 'event', eid, st, 'refl id', refl.resourceId, names(refl))
    disasm = ctrl.DisassembleShader(rd.ResourceId.Null(), refl, '')
    new, err = ctrl.BuildTargetShader('main', rd.ShaderEncoding.D3D9Asm, disasm.encode(), rd.ShaderCompileFlags(),
                                      refl.stage)
    built = ctrl.GetShader(rd.ResourceId.Null(), new, rd.ShaderEntryPoint('main', refl.stage))
    print('   built', new, 'refl id', built.resourceId, names(built))
    ctrl.ReplaceResource(id, new)
    built2 = ctrl.GetShader(rd.ResourceId.Null(), new, rd.ShaderEntryPoint('main', refl.stage))
    print('   replaced', names(built2), 'original now', refl.resourceId, names(refl))
    ctrl.RemoveReplacement(id)
    ctrl.FreeTargetResource(new)

ctrl.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
