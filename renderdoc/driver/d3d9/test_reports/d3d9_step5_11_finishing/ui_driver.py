# run with: qrenderdoc.exe <capture.rdc> --ui-python dbg_driver.py ; UI_DIR = command/status folder
# Commands: write UI_DIR\cmd.txt with one line; the script answers in UI_DIR\status.txt as "<n> <text>".
# Every call into the UI goes through the UI thread (InvokeOntoUIThread), since this script runs on
# its own thread. No real mouse or keyboard input is used.
import os, time, traceback, threading, struct
import renderdoc as rd
import qrenderdoc as qrd

d = os.environ['UI_DIR']
seq = [0]
viewer = [None]
mqt = pyrenderdoc.Extensions().GetMiniQtHelper()


def ui(fn):
    done = threading.Event()
    res = {}

    def call():
        try:
            res['ret'] = fn()
        except Exception:
            res['exc'] = traceback.format_exc()
        done.set()
    mqt.InvokeOntoUIThread(call)
    done.wait(60)
    if 'exc' in res:
        raise RuntimeError(res['exc'])
    return res.get('ret')


def replay(fn):
    res = {}

    def work(r):
        try:
            res['ret'] = fn(r)
        except Exception:
            res['exc'] = traceback.format_exc()
    pyrenderdoc.Replay().BlockInvoke(work)
    if 'exc' in res:
        raise RuntimeError(res['exc'])
    return res.get('ret')


def status(text):
    seq[0] += 1
    tmp = os.path.join(d, 'status.tmp')
    with open(tmp, 'w') as f:
        f.write('%d %s\n' % (seq[0], text))
    os.replace(tmp, os.path.join(d, 'status.txt'))


def find_marker(name):
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
    return walk(pyrenderdoc.CurRootActions())


def open_viewer(stage, trace, context):
    def fn():
        ps = pyrenderdoc.CurPipelineState()
        refl = ps.GetShaderReflection(stage)
        w = pyrenderdoc.DebugShader(refl, ps.GetGraphicsPipelineObject(), trace, context)
        pyrenderdoc.AddDockWindow(w.Widget(), qrd.DockReference.MainToolArea, None)
        viewer[0] = w
    ui(fn)
    time.sleep(3)


try:
    t0 = time.time()
    while not pyrenderdoc.IsCaptureLoaded() and time.time() - t0 < 120:
        time.sleep(0.25)
    time.sleep(2)
    status('loaded')
    cmdf = os.path.join(d, 'cmd.txt')
    while True:
        if not os.path.exists(cmdf):
            time.sleep(0.1)
            continue
        with open(cmdf) as f:
            cmd = f.read().strip()
        os.remove(cmdf)
        try:
            if cmd.startswith('marker '):
                draw = find_marker(cmd.split(None, 1)[1])
                ui(lambda: pyrenderdoc.SetEventID([], draw.eventId, draw.eventId))
                time.sleep(1.5)
                status('event %d' % pyrenderdoc.CurEvent())
            elif cmd.startswith('mesh'):
                # the mesh viewer, VS In's row scrolled to
                row = int(cmd.split()[1]) if len(cmd.split()) > 1 else 0

                def show():
                    pyrenderdoc.ShowMeshPreview()
                    bv = pyrenderdoc.GetMeshPreview()
                    bv.ShowMeshData(rd.MeshDataStage.VSOut)
                    bv.ScrollToRow(row, rd.MeshDataStage.VSIn)
                    bv.ScrollToRow(row, rd.MeshDataStage.VSOut)
                    pyrenderdoc.RaiseDockWindow(bv.Widget())
                ui(show)
                time.sleep(2)
                status('mesh viewer, row %d' % row)
            elif cmd.startswith('debugvtx '):
                # as the mesh viewer's "Debug this Vertex": its vertex and index (IDX)
                vtx = int(cmd.split()[1])
                inst = int(cmd.split()[2]) if len(cmd.split()) > 2 else 0
                draw = pyrenderdoc.CurAction()

                def work(r):
                    idx = draw.vertexOffset + vtx
                    if draw.flags & rd.ActionFlags.Indexed:
                        ib = r.GetPipelineState().GetIBuffer()
                        width = ib.byteStride
                        data = r.GetBufferData(ib.resourceId, ib.byteOffset + (draw.indexOffset + vtx) * width, width)
                        idx = struct.unpack('<H' if width == 2 else '<I', data)[0] + draw.baseVertex
                    t = r.DebugVertex(vtx, inst, idx, 0)
                    return (t, idx)
                trace, idx = replay(work)
                if trace.debugger is not None:
                    ctx = 'Vertex %d' % vtx
                    if draw.numInstances > 1:
                        ctx += ' - Instance %d' % inst
                    open_viewer(rd.ShaderStage.Vertex, trace, ctx)
                status('debugging vertex %d (idx %d) instance %d, debugger %s' % (vtx, idx, inst, trace.debugger is not None))
            elif cmd.startswith('texview '):
                # the texture viewer on the draw's first target, at a pixel
                x, y = int(cmd.split()[1]), int(cmd.split()[2])

                def show():
                    pyrenderdoc.ShowTextureViewer()
                    tv = pyrenderdoc.GetTextureViewer()
                    ps = pyrenderdoc.CurPipelineState()
                    tex = ps.GetOutputTargets()[0].resource
                    tv.ViewTexture(tex, rd.CompType.Typeless, True)
                    tv.GotoLocation(x, y)
                    pyrenderdoc.RaiseDockWindow(tv.Widget())
                ui(show)
                time.sleep(2)
                status('texture viewer at %d, %d' % (x, y))
            elif cmd.startswith('debugpix '):
                x, y = int(cmd.split()[1]), int(cmd.split()[2])
                trace = replay(lambda r: r.DebugPixel(x, y, rd.DebugPixelInputs()))
                if trace.debugger is not None:
                    open_viewer(rd.ShaderStage.Pixel, trace, 'Pixel %d,%d' % (x, y))
                status('debugging pixel, debugger %s' % (trace.debugger is not None))
            elif cmd.startswith('bp '):
                n = int(cmd.split()[1])
                ui(lambda: viewer[0].ToggleBreakpointOnInstruction(n))
                status('breakpoint on instruction %d' % n)
            elif cmd == 'run':
                ui(lambda: viewer[0].RunForward())
                time.sleep(2)
                status('step %d' % ui(lambda: viewer[0].CurrentStep()))
            elif cmd.startswith('step '):
                n = int(cmd.split()[1])
                ui(lambda: viewer[0].SetCurrentStep(n))
                time.sleep(1)
                status('step %d' % ui(lambda: viewer[0].CurrentStep()))
            elif cmd == 'end':
                ui(lambda: viewer[0].SetCurrentStep(100000))
                time.sleep(1)
                status('step %d' % ui(lambda: viewer[0].CurrentStep()))
            elif cmd.startswith('watch '):
                expr = cmd.split(None, 1)[1]
                ui(lambda: viewer[0].AddWatch(expr))
                time.sleep(0.5)
                status('watch ' + expr)
            elif cmd.startswith('status '):
                stage = rd.ShaderStage.Vertex if cmd.split()[1] == 'vs' else rd.ShaderStage.Pixel
                ps = ui(lambda: pyrenderdoc.CurPipelineState().GetShaderReflection(stage))
                if ps is None:
                    status('no reflection')
                else:
                    status('debuggable %s "%s"' % (ps.debugInfo.debuggable, ps.debugInfo.debugStatus))
            elif cmd == 'pyside':
                try:
                    import PySide2
                    status('PySide2 ' + PySide2.__version__)
                except Exception as e:
                    status('no PySide2: %s' % e)
            elif cmd == 'quit':
                status('quit')
                break
            else:
                status('unknown ' + cmd)
        except Exception:
            status('EXC ' + traceback.format_exc().replace('\n', ' | '))
except Exception:
    status('EXC ' + traceback.format_exc().replace('\n', ' | '))
