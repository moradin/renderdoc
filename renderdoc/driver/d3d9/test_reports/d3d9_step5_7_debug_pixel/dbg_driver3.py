# run with: qrenderdoc.exe <capture.rdc> --ui-python dbg_driver3.py ; UI_DIR = command/status folder
# Commands: write UI_DIR\cmd.txt with one line; the script answers in UI_DIR\status.txt as "<n> <text>".
# Every call into the UI goes through the UI thread (InvokeOntoUIThread), since this script runs on
# its own thread. No real mouse or keyboard input is used.
import os, time, traceback, threading
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
                res = {}

                def work(r):
                    res['trace'] = r.DebugPixel(x, y, rd.DebugPixelInputs())
                pyrenderdoc.Replay().BlockInvoke(work)
                trace = res['trace']

                def open_viewer():
                    ps = pyrenderdoc.CurPipelineState()
                    refl = ps.GetShaderReflection(rd.ShaderStage.Pixel)
                    w = pyrenderdoc.DebugShader(refl, ps.GetGraphicsPipelineObject(), trace,
                                                'Pixel %d,%d' % (x, y))
                    pyrenderdoc.AddDockWindow(w.Widget(), qrd.DockReference.MainToolArea, None)
                    viewer[0] = w
                if trace.debugger is not None:
                    ui(open_viewer)
                    time.sleep(3)
                status('debugging, debugger %s' % (trace.debugger is not None))
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
            elif cmd == 'status':
                ps = ui(lambda: pyrenderdoc.CurPipelineState().GetShaderReflection(rd.ShaderStage.Pixel))
                status('pixel debuggable %s "%s"' % (ps.debugInfo.debuggable, ps.debugInfo.debugStatus))
            elif cmd == 'quit':
                status('quit')
                break
            else:
                status('unknown ' + cmd)
        except Exception:
            status('EXC ' + traceback.format_exc().replace('\n', ' | '))
except Exception:
    status('EXC ' + traceback.format_exc().replace('\n', ' | '))
