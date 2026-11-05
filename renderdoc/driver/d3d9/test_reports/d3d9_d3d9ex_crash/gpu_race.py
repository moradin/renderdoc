# Starts several overlapping GPU enumeration threads (each InitialiseReplay starts one without
# joining the previous), so several Vulkan proxy devices are created at once.
import os, time, renderdoc as rd
out = open(os.environ['RACE_OUT'], 'w', buffering=1)
for i in range(int(os.environ.get('RACE_N', '6'))):
    out.write('InitialiseReplay %d\n' % i)
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    time.sleep(float(os.environ.get('RACE_SLEEP', '0.02')))
out.write('waiting\n')
time.sleep(8)
out.write('log %s\n' % rd.GetLogFile())
out.write('done\n')
out.close()
os._exit(0)
