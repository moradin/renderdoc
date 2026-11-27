# run with: qrenderdoc.exe <capture.rdc> --ui-python ui_driver.py ; UI_DIR = command/status folder
# Commands: write UI_DIR\cmd.txt with one line; the script answers in UI_DIR\status.txt as "<n> <text>".
# Every call into the UI goes through the UI thread (InvokeOntoUIThread), since this script runs on
# its own thread. No real mouse or keyboard input is used.
#
#   marker <name>          go to the last event in the marker (or the marker's own event)
#   texview <x> <y> [depth] the texture viewer on the event's colour target (or depth target), at a pixel
#   history <x> <y> [depth] a pixel history window, as the texture viewer's History button opens one
#   quit
import os, time, traceback, threading
import renderdoc as rd
import qrenderdoc as qrd

d = os.environ['UI_DIR']
seq = [0]
mqt = pyrenderdoc.Extensions().GetMiniQtHelper()
history_windows = []


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
    done.wait(120)
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


def find_action(name):
    def walk(actions):
        for a in actions:
            if a.customName == name:
                return a
            r = walk(a.children)
            if r:
                return r
        return None
    return walk(pyrenderdoc.CurRootActions())


def last_in(a):
    while a.children:
        a = a.children[-1]
    return a


def target(depth):
    ps = pyrenderdoc.CurPipelineState()
    return ps.GetDepthTarget().resource if depth else ps.GetOutputTargets()[0].resource


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
            parts = cmd.split()
            if cmd.startswith('marker '):
                a = find_action(cmd.split(None, 1)[1])
                # a marker's next action, or a push marker's last event
                eid = last_in(a).eventId if a.children else (a.next.eventId if a.next else a.eventId)
                ui(lambda: pyrenderdoc.SetEventID([], eid, eid))
                time.sleep(1.5)
                status('event %d' % pyrenderdoc.CurEvent())
            elif parts[0] == 'texview':
                x, y = int(parts[1]), int(parts[2])
                depth = len(parts) > 3

                def show():
                    pyrenderdoc.ShowTextureViewer()
                    tv = pyrenderdoc.GetTextureViewer()
                    tv.ViewTexture(target(depth), rd.CompType.Typeless, True)
                    tv.GotoLocation(x, y)
                    pyrenderdoc.RaiseDockWindow(tv.Widget())
                ui(show)
                time.sleep(2)
                status('texture viewer at %d, %d' % (x, y))
            elif parts[0] == 'history':
                # what TextureViewer::ShowPixelHistory does
                x, y = int(parts[1]), int(parts[2])
                depth = len(parts) > 3
                tex = ui(lambda: target(depth))
                t = time.time()
                modifs = replay(lambda r: r.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless))
                elapsed = time.time() - t

                def show():
                    tv = pyrenderdoc.GetTextureViewer()
                    disp = rd.TextureDisplay()
                    disp.resourceId = tex
                    disp.typeCast = rd.CompType.Typeless
                    disp.rangeMin = 0.0
                    disp.rangeMax = 1.0
                    for w in history_windows:
                        w.Widget().close() if hasattr(w.Widget(), 'close') else None
                    hist = pyrenderdoc.ViewPixelHistory(tex, x, y, 0, disp)
                    pyrenderdoc.AddDockWindow(hist.Widget(), qrd.DockReference.TransientPopupArea, tv.Widget(), 0.3)
                    hist.SetHistory(modifs)
                    history_windows.append(hist)
                ui(show)
                time.sleep(2)
                status('history at %d, %d: %d modifications %s in %.2fs' %
                       (x, y, len(modifs), [m.eventId for m in modifs][:12], elapsed))
            elif cmd == 'quit':
                status('quit')
                break
            else:
                status('unknown ' + cmd)
        except Exception:
            status('EXC ' + traceback.format_exc().replace('\n', ' | '))
except Exception:
    status('EXC ' + traceback.format_exc().replace('\n', ' | '))
