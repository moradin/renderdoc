# python device_loss.py <capture> [ex] : opens a capture, then keeps replaying it while a fullscreen
# D3D9 probe takes the display, and reports whether and when the replay device was lost
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

cap = rd.OpenCaptureFile()
print('open', cap.OpenFile(sys.argv[1], '', None))
res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
print('replay', res)

actions = []


def walk(l):
    for a in l:
        actions.append(a)
        walk(a.children)


walk(controller.GetRootActions())
tex = controller.GetTextures()[0].resourceId


def replay_round():
    for a in actions:
        controller.SetFrameEvent(a.eventId, True)
    controller.GetTextureData(tex, rd.Subresource(0, 0, 0))


replay_round()
print('before:', controller.GetFatalErrorStatus().code)

probe = subprocess.Popen([r'E:\Projects\renderdoc\util\test\demos_x64.exe', 'D3D9_Fullscreen_Probe',
                          '--log', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fullscreen_probe.log')])
start = time.monotonic()
lost_at = None
rounds = 0
while time.monotonic() - start < 8.0:
    replay_round()
    rounds += 1
    status = controller.GetFatalErrorStatus()
    if status.code != rd.ResultCode.Succeeded and lost_at is None:
        lost_at = time.monotonic() - start
        print(f'fatal error after {lost_at:.2f}s (round {rounds}): {status.code} {status.Message()}')
probe.wait()
print(f'{rounds} replay rounds while the probe ran, probe exit {probe.returncode}')
print('after:', controller.GetFatalErrorStatus().code, controller.GetFatalErrorStatus().Message())

controller.Shutdown()
cap.Shutdown()
rd.ShutdownReplay()
