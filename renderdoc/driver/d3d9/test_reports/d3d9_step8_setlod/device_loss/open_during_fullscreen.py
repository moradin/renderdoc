# python open_during_fullscreen.py <capture> [ex] : starts a D3D9 program that holds exclusive
# fullscreen for 4 seconds, opens the capture while it does, replays it until the program exits,
# then opens the capture again
import os
import subprocess
import sys
import time

sys.path.insert(0, r'E:\Projects\renderdoc\x64\Development\py3module\pymodules')
os.add_dll_directory(r'E:\Projects\renderdoc\x64\Development')
import renderdoc as rd

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
if len(sys.argv) > 2 and sys.argv[2] == 'ex':
    rd.SetConfigSetting('D3D9_ForceReplayOnExDevice').data.basic.b = True
    print('D3D9_ForceReplayOnExDevice set')


def open_capture(what):
    cap = rd.OpenCaptureFile()
    cap.OpenFile(sys.argv[1], '', None)
    t = time.monotonic()
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    print(f'{what}: OpenCapture {res} after {time.monotonic() - t:.2f}s')
    return cap, controller


def actions_of(controller):
    out = []

    def walk(l):
        for a in l:
            out.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    return out


def replay_round(controller, actions):
    for a in actions:
        controller.SetFrameEvent(a.eventId, True)
    return controller.GetTextureData(controller.GetTextures()[0].resourceId, rd.Subresource(0, 0, 0))


probe = subprocess.Popen([r'E:\Projects\renderdoc\util\test\demos_x64.exe', 'D3D9_Fullscreen_Probe',
                          '--log', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fullscreen_probe.log')])
# the probe's device is fullscreen within about half a second
time.sleep(1.5)
start = time.monotonic()
cap, controller = open_capture('While the program is fullscreen')
if controller is not None:
    actions = actions_of(controller)
    rounds = 0
    first_error = None
    while probe.poll() is None:
        replay_round(controller, actions)
        rounds += 1
        status = controller.GetFatalErrorStatus()
        if status.code != rd.ResultCode.Succeeded and first_error is None:
            first_error = f'{status.code} {status.Message()} (round {rounds})'
    print(f'{rounds} replay rounds until the program exited, first error: {first_error}')
    status = controller.GetFatalErrorStatus()
    print('after it exited:', status.code, status.Message())
    controller.Shutdown()
cap.Shutdown()
probe.wait()
print(f'program exit {probe.returncode}')

cap, controller = open_capture('Opened again afterwards')
if controller is not None:
    data = replay_round(controller, actions_of(controller))
    print(f'replayed every action, read {len(data)} bytes, status',
          controller.GetFatalErrorStatus().code)
    controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
