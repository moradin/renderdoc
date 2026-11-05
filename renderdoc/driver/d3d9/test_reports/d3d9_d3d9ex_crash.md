# D3D9Ex_Wrapping_Check null-call crash hunt

Same machine and build as retest 5: Windows 11, RTX 4070 Ti, Development x64, c1049db4f. No code changes.

**Summary:**
- `pipe_extra.py` ran 60 times on D3D9Ex_Wrapping_Check, each in a new qrenderdoc. **Nothing crashed: all 60 exited 0, with identical output.**
- The original 1-in-18 crash left a dump on 2026-11-04 at 08:06, build b8b377051. That dump points at a race between two GPU-enumeration threads in the Vulkan replay driver, not at D3D9.
- I reproduced that race on purpose without code changes. It crashed 9 times in 20 runs. With this build's PDBs, every one of those dumps is a call through a null entry in the shared Vulkan replay dispatch table. The old dump has the same stack signature.

## Setup and one deviation

- **Dumps are minidumps, not full dumps.** This session isn't elevated, so I couldn't write the HKLM key.
  - I tried the same key under HKCU, but WER ignored it.
  - The machine already has an empty `HKLM\...\Windows Error Reporting\LocalDumps` key. Its defaults are minidumps, `%LOCALAPPDATA%\CrashDumps`, and 10 kept.
  - A test crash confirmed qrenderdoc dumps land there. The runner copied each new dump to `C:\dumps\<run>_<name>.dmp`.
  - Minidumps have registers, stacks and the module list, which is enough for the commands below. For full dumps, someone needs to run the HKLM commands in an admin PowerShell.
- **Runner:** the same as `d3d9_phase3_retest.md` item 6. For each run:
  - start a new `x64\Development\qrenderdoc.exe --python run_check.py`, with `run_check.py` running `pipe_extra.py <rdc>`;
  - wait up to 300 s and record the exit code;
  - keep the script output and the RenderDoc log for that run;
  - after a non-zero exit, wait for a WER dump.

  The capture is `D3D9Ex_Wrapping_Check_frame42274.rdc`, the one from the phase 3 retest.

## 1. pipe_extra.py, 60 runs: no crash

| Runs | Exit code | Output |
|---|---|---|
| 1–60 | 0 for all | Identical in all 60, last event `=== event 1 IDirect3DDevice9::Clear()`, no timeouts, no new dumps |

The per-run lines are in `d3d9_d3d9ex_crash/pipe_extra_60_runs.txt`. Counting all rounds, the crash has now happened **once in 78 loads**.

The logs show the window the crash needs:
- **Two GPU enumeration threads in every run.** Every log has two "Replay application ... launched" lines, two sets of proxy devices (D3D11, D3D12, Vulkan) and two "GPU:" groups. Section 2 explains why.
- **Overlapping Vulkan creation in 3 runs (39, 46 and 50).** Their two "Creating a VulkanReplay replay device" lines come before the first "Created device.", so the two Vulkan proxy creations overlapped.
- **The D3D12 assertion in 3 runs (43, 46 and 50).** These runs logged `d3d12_replay.cpp(109/110) Assertion failed: 'GetModuleHandleA("d3d12.dll") == 0'`. That is the two D3D12 proxies overlapping, the same overlap seen from the D3D12 side.
- **No other errors or warnings** besides the usual adb, ToolWindowManager and QFileSystemWatcher lines.

## 2. The original crash: 2026-11-04 08:06, build b8b377051

`C:\dumps\old_20261104_0806_qrenderdoc.exe.49300.dmp` is the dump from the one crashing run.

- **Symbols:** renderdoc.dll has been rebuilt since, and Development links incrementally, so a rebuild of b8b377 wouldn't match its layout. Offsets into renderdoc stay unresolved.
- **cdb output:** `d3d9_d3d9ex_crash/cdb_20261104_0806_requested.txt` is the full output of the requested command.
  - `ub poi(@rsp)` fails because the old image is gone, and that failure aborts the rest of the command line. So `~*kn 25` is in a second run, `cdb_20261104_0806_threads.txt`.
- **Script progress:** none. The log ends inside `D3D9Replay` creation on the main thread, after "Capture was made on ..." and before "Created replay driver.". `pipe_extra.py` prints only after the capture loads, so it had printed no event.

The key part:

```
rax=00007ff89cc4c560 rbx=0000000000000000 rcx=0000023e7958bf70
rdx=0000000000000000 rsi=0000000000000000 rdi=0000005a543fec98
rip=0000000000000000 rsp=0000005a543fe658 rbp=0000000000000000
 r8=0000005a543fe680  r9=0000000000000000
 # Child-SP          RetAddr               Call Site
00 0000005a`543fe658 00007ff8`9b2be348     0x0
01 0000005a`543fe660 00007ff8`9cfc17d8     renderdoc+0x2efe348
0000005a`543fe658  00007ff8`9b2be348 renderdoc+0x2efe348
0000005a`543fe660  00007ff8`9cfc17d8 renderdoc+0x4c017d8
0000005a`543fe688  00007ff9`0d68b72c vulkan_1!vkResetEvent+0xda05c
0000005a`543fe6b8  00007ff9`0d5fbd95 vulkan_1!vkResetEvent+0x4a6c5
0000005a`543fe6e8  00007ff9`0d5fbd05 vulkan_1!vkResetEvent+0x4a635
```

- **Thread 4 (crashed)** is a renderdoc background thread, not the main thread. It made a null call from renderdoc+0x2efe348, with `vulkan_1` frames above it on the stack.
- **The arguments match `EnumerateDeviceExtensionProperties(pd, NULL, &count, NULL)`:** `rax` points into renderdoc's data section, `rdx` = 0, `r8` = rsp+0x28 and `r9` = 0.
- **Thread 0 (main)** was in `d3d9!CEnum::CreateDeviceEx`, called from renderdoc+0x1ea96f3. The NVIDIA driver was in `WinVerifyTrust` there. That is ordinary D3D9 replay device creation. It's on the stack only because this capture is D3D9, and it isn't involved in the crash.

### Last 40 lines of its RenderDoc log

The full log is 45 lines and is also in `d3d9_d3d9ex_crash/log_20261104_0806_last40.txt`.

```
RDOC 049300: [08:06:25]         settings.cpp( 471) - Log     - Loading config from C:\Users\Balazs\AppData\Roaming\renderdoc\renderdoc.conf
QTRD 049300: [08:06:25]       qrenderdoc.cpp( 196) - Log     - QRenderDoc initialising.
RDOC 049300: [08:06:25]             core.cpp( 848) - Log     - Replay application 'E:\Projects\renderdoc\x64\Development\qrenderdoc.exe' launched
RDOC 049300: [08:06:25]     d3d11_replay.cpp(4044) - Debug   - Creating a D3D11 replay device
QTRD 049300: [08:06:25]    PythonContext.cpp( 379) - Log     - Not modifying stubs of version  "1.48 (b8b377)" in "C:/Users/Balazs/AppData/Roaming/qrenderdoc/pystubs/v1_48"
QTRD 049300: [08:06:25]    PythonContext.cpp( 379) - Log     - Not modifying stubs of version  "1.48 (b8b377)" in "C:/Users/Balazs/AppData/Roaming/qrenderdoc/pystubs/latest"
QTRD 049300: [08:06:25]    PythonContext.cpp( 435) - Log     - Stubs generation processed in 1 ms
RDOC 049300: [08:06:25]     d3d11_replay.cpp(4358) - Debug   - Creating D3D11 replay device, NULL adapter, driver type D3D_DRIVER_TYPE_HARDWARE, flags 0, 2 feature levels (first D3D_FEATURE_LEVEL_11_1)
RDOC 049300: [08:06:25]     d3d11_device.cpp( 234) - Debug   - Couldn't get ID3D11InfoQueue.
RDOC 049300: [08:06:25]     d3d11_replay.cpp( 147) - Log     - Running replay on nVidia / NVIDIA GeForce RTX 4070 Ti 32.0.16.1088
RDOC 049300: [08:06:25]     d3d12_replay.cpp(4536) - Debug   - Creating a D3D12 replay device
RDOC 049300: [08:06:25] d3d12_sdk_select.cpp( 376) - Log     - System D3D12 runtime is version 616
QTRD 049300: [08:06:25]       MainWindow.cpp(3353) - Log     - LoadLayout  0
RDOC 049300: [08:06:25]    android_tools.cpp( 347) - Log     - Initialising adb using 'adb'
RDOC 049300: [08:06:25]    android_tools.cpp( 354) - Warning - Couldn't locate adb. Ensure adb is in PATH, ANDROID_SDK or ANDROID_HOME is set, or you configure your SDK location
QTRD 049300: [08:06:25] ToolWindowManager.cpp(1571) - Warning - area parameter ignored for this type
QTRD 049300: [08:06:25]                     (   0) - Warning - QFileSystemWatcher::addPaths: list is empty
QTRD 049300: [08:06:25]       qrenderdoc.cpp( 710) - Log     - running "C:\\Users\\Balazs\\AppData\\Local\\Temp\\claude\\90f834f7a055dbe2\\a648b5f7-d682-4d16-a0ea-7456b36dec91\\scratchpad\\p3\\run_check.py"
RDOC 049300: [08:06:25]             core.cpp( 848) - Log     - Replay application 'E:\Projects\renderdoc\x64\Development\qrenderdoc.exe' launched
RDOC 049300: [08:06:25]          rdcfile.cpp( 246) - Log     - Opening RDCFile C:\Users\Balazs\AppData\Local\Temp\claude\90f834f7a055dbe2\a648b5f7-d682-4d16-a0ea-7456b36dec91\scratchpad\p3\cap\D3D9Ex_Wrapping_Check\D3D9Ex_Wrapping_Check_frame42274.rdc
RDOC 049300: [08:06:25]     d3d11_replay.cpp(4044) - Debug   - Creating a D3D11 replay device
RDOC 049300: [08:06:25]          rdcfile.cpp( 311) - Debug   - Opened capture file for read
RDOC 049300: [08:06:25]             core.cpp( 246) - Log     - Not enabling API validation during replay
RDOC 049300: [08:06:25]             core.cpp( 251) - Log     - Using default GPU replay selection algorithm
RDOC 049300: [08:06:25]             core.cpp( 260) - Log     - Replay optimisation level: Balanced
RDOC 049300: [08:06:25]     d3d11_replay.cpp(4358) - Debug   - Creating D3D11 replay device, NULL adapter, driver type D3D_DRIVER_TYPE_HARDWARE, flags 0, 2 feature levels (first D3D_FEATURE_LEVEL_11_1)
RDOC 049300: [08:06:25]     d3d12_device.cpp( 937) - Debug   - Couldn't get ID3D12InfoQueue.
RDOC 049300: [08:06:25]     d3d12_replay.cpp( 149) - Log     - Running replay on nVidia / NVIDIA GeForce RTX 4070 Ti 32.0.16.1088
RDOC 049300: [08:06:25]     d3d11_device.cpp( 234) - Debug   - Couldn't get ID3D11InfoQueue.
RDOC 049300: [08:06:25]     d3d11_replay.cpp( 147) - Log     - Running replay on nVidia / NVIDIA GeForce RTX 4070 Ti 32.0.16.1088
RDOC 049300: [08:06:25]        vk_replay.cpp(5727) - Debug   - Creating a VulkanReplay replay device
RDOC 049300: [08:06:25]     d3d12_replay.cpp(4536) - Debug   - Creating a D3D12 replay device
RDOC 049300: [08:06:26]     d3d12_device.cpp( 937) - Debug   - Couldn't get ID3D12InfoQueue.
RDOC 049300: [08:06:26]     d3d12_replay.cpp( 149) - Log     - Running replay on nVidia / NVIDIA GeForce RTX 4070 Ti 32.0.16.1088
RDOC 049300: [08:06:26]        vk_replay.cpp(5727) - Debug   - Creating a VulkanReplay replay device
RDOC 049300: [08:06:26]        vk_replay.cpp(5810) - Log     - Created device.
RDOC 049300: [08:06:26]        vk_replay.cpp(5810) - Log     - Created device.
RDOC 049300: [08:06:26]             core.cpp( 997) - Log     - GPU: nVidia - NVIDIA GeForce RTX 4070 Ti (driver: --)
RDOC 049300: [08:06:26]             core.cpp( 997) - Log     - GPU: Software - WARP Rasterizer (driver: --)
RDOC 049300: [08:06:26]             core.cpp(1015) - Log     - Driver: 'C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispig.inf_amd64_cc526ccc3287f5cd\nvwgf2umx.dll' 32.0.16.1088 2026-08-18
RDOC 049300: [08:06:26]      d3d9_replay.cpp( 903) - Debug   - Creating a D3D9 replay device
RDOC 049300: [08:06:26]      d3d9_replay.cpp( 938) - Log     - Capture was made on NVIDIA GeForce RTX 4070 Ti
```

The log shows both Vulkan proxy creations in flight together, then "Created device." twice, but only **one** "GPU:" group. That makes sense if one enumeration thread finished while the other crashed after its "Created device.". That is where `GetInitialDriverVersion` runs (`vk_replay.cpp:5815`).

There is also an older dump, `C:\dumps\old_20261104_0735_qrenderdoc.exe.43804.dmp`, from 07:35. It's a different crash: an access violation in `d3d9!CD3DBase::GetPaletteEntries+0x5e`, from an earlier round's palette checks. It isn't this bug.

## 3. Cause

There are two parts, and both are upstream RenderDoc code, not from the D3D9 branch.

### Two GPU enumeration threads run at once

- **How a second thread starts:** `RenderDoc::InitialiseReplay` (`renderdoc/core/core.cpp:860-862`) starts a GPU enumeration thread and stores it in `m_AvailableGPUThread`. It doesn't check whether one is already running.
- **Who calls it twice:** qrenderdoc calls `InitialiseReplay` at startup. `pipe_extra.py:75`, like the other util/test style scripts I run inside qrenderdoc, calls `rd.InitialiseReplay(...)` again.
- **What happens to the first thread:** the second call overwrites the handle, so `SyncAvailableGPUThread` (`core.cpp:1915`) joins only the second thread. The first is orphaned: it's never joined, and it keeps creating proxy drivers while the capture loads.
- **Other effects of the overlap:**
  - The two threads also `push_back` into `m_AvailableGPUs` with no lock.
  - `D3D12Replay::Shutdown` tears down process-wide state while the other thread may still use it: it calls `D3D12_CleanupReplaySDK()` and `Threading::JobSystem::Shutdown()`, and it asserts that d3d12.dll is unloaded.

### The Vulkan replay dispatch tables are global, and get zeroed during creation

- **The tables are shared:** in replay, `GetInstanceDispatchTable` / `GetDeviceDispatchTable` return the single static `replayInstanceTable` / `replayDeviceTable` (`vk_dispatchtables.cpp:262-263, 281-282`).
- **Every creation zeroes them:** `Vulkan_CreateReplayDevice` calls `InitReplayTables(module)` (`vk_replay.cpp:5781`). That `memset`s both tables to 0 (`vk_dispatchtables.cpp:51, 59`) with no lock, then refills only the core entries.
- **One entry stays null for a long time:** `EnumerateDeviceExtensionProperties` isn't refilled there. Only `InitInstanceExtensionTables` refills it (`vk_dispatchtables.cpp:130`), and that runs inside `WrappedVulkan::Initialise`, after `vkCreateInstance`. So that entry is null for the whole of the other thread's `vkCreateInstance`, which includes loading the ICDs.
- **The crash:** a thread that reaches `GetPhysicalDeviceDriverProperties` (`vk_common.cpp:1207`) during that window calls 0. That function runs from `VulkanReplay::GetInitialDriverVersion` (`vk_replay.cpp:1182`) or `VulkanReplay::GetAvailableGPUs` (`vk_replay.cpp:139`).

### Why it shows up with D3D9Ex_Wrapping_Check

- It isn't specific to this capture. Any capture loaded by a script that calls `InitialiseReplay` inside qrenderdoc can hit it.
- The enumeration threads run during the capture load, so whatever the main thread is loading at that moment is what appears on its stack.

## 4. Reproduction with this build: 9 crashes in 20 runs

`d3d9_d3d9ex_crash/gpu_race.py` calls `rd.InitialiseReplay(rd.GlobalEnvironment(), [])` 6 times, 20 ms apart, inside qrenderdoc, then waits 8 s. That starts 6 overlapping enumeration threads. I ran it in a new qrenderdoc 20 times (`gpu_race_20_runs.txt`).

| Run | Exit code | Script got to | Top of the crashing stack |
|---|---|---|---|
| 1 | 0xC0000374 | `waiting` | `0x0 <- GetPhysicalDeviceDriverProperties+0x48 <- VulkanReplay::GetInitialDriverVersion+0x24c <- Vulkan_CreateReplayDevice+0xbe3` |
| 4 | 0xC0000005 | `waiting` | the same as run 1 |
| 7 | 0xC0000005 | `waiting` | `0x0 <- GetPhysicalDeviceDriverProperties+0x48 <- VulkanReplay::GetAvailableGPUs+0x213 <- enumeration lambda` |
| 11 | 0xC0000005 | `waiting` | the same as run 1 |
| 12 | 0xC0000005 | `waiting` | the same as run 7 |
| 14 | 0xC0000005 | `waiting` | the same as run 1 |
| 15 | 0xC0000005 | `waiting` | the same as run 1 |
| 18 | 0xC0000005 | `waiting` | `0x0 <- InitInstanceExtensionTables+0x1555 <- WrappedVulkan::Initialise+0x2a16 <- Vulkan_CreateReplayDevice+0xa87`. That is `table->GetInstanceProcAddr`, read while another thread's `memset` had zeroed it |
| 20 | 0xC0000005 | `waiting` | the same as run 1 |

The other 11 runs exited 0. In run 1, WER's dump shows the null call, but the process then died with heap corruption, 0xC0000374. That could be the unlocked `m_AvailableGPUs` writes.

**race04 in detail** (`cdb_race04.txt`, the full output of the requested command):

```
rax=00007ffd37ef2720 rbx=0000000000000000 rcx=000001e3ee67dee0
rdx=0000000000000000 rsi=0000000000000000 rdi=000000c9096feba8
rip=0000000000000000 rsp=000000c9096fe568 rbp=0000000000000000
 r8=000000c9096fe590  r9=0000000000000000
 # Child-SP          RetAddr               Call Site
00 000000c9`096fe568 00007ffd`3654af38     0x0
01 000000c9`096fe570 00007ffd`366a461c     renderdoc!GetPhysicalDeviceDriverProperties+0x48
02 000000c9`096fe920 00007ffd`366aa373     renderdoc!VulkanReplay::GetInitialDriverVersion+0x24c
03 000000c9`096fef00 00007ffd`33a40596     renderdoc!Vulkan_CreateReplayDevice+0xbe3
04 000000c9`096ff360 00007ffd`33b6004f     renderdoc!<lambda_62b9cfd8acc8756ac4fd38425d0e04c0>::operator()+0x1f6
...
08 000000c9`096ffdc0 00007ffe`62f4dda7     renderdoc!Threading::sThreadInit+0x70
000000c9`096fe598  00007ffc`f740b72c vulkan_1!vkResetEvent+0xda05c
000000c9`096fe5c8  00007ffc`f737bd95 vulkan_1!vkResetEvent+0x4a6c5
000000c9`096fe5f8  00007ffc`f737bd05 vulkan_1!vkResetEvent+0x4a635
renderdoc!GetPhysicalDeviceDriverProperties+0x1e:
...
00007ffd`3654af2d 488b8424b0030000 mov     rax,qword ptr [rsp+3B0h]
00007ffd`3654af35 ff5060          call    qword ptr [rax+60h]
```

- **The table:** `ln 00007ffd37ef2720` gives `renderdoc!replayInstanceTable`. `dt VkInstDispatchTable` puts `EnumerateDeviceExtensionProperties` at +0x060.
- **The slot at dump time:** it holds `vulkan_1!vkEnumerateDeviceExtensionProperties` again, so it was null only for a moment, which fits the window above.
- **The other threads:** threads 12, 13, 15 and 16 had crashed at the same place and were parked in `kernel32!WerpReportFault`. `.cxr` on their exception contexts gives the same three frames (`cdb_race04_other_threads.txt`).
- **Match with the old dump:** the stack under the null call is word for word the same as section 2's dump, apart from addresses. The return address and a renderdoc data pointer sit at [rsp] and [rsp+8]. `vulkan_1!vkResetEvent+0xda05c`, `+0x4a6c5` and `+0x4a635` sit at rsp+0x30, +0x60 and +0x90. `rdx` = 0, `r8` = rsp+0x28 and `r9` = 0. So the 2026-11-04 crash is this same `call [replayInstanceTable+0x60]`.
- **The log:** the last 40 lines of race04's log are in `log_race04_last40.txt`. It ends with several overlapping "Creating a VulkanReplay replay device" and "Created device." lines, after the d3d12 assertion.

## Possible fixes (for the cloud session to decide)

- **Stop the second thread:** in `RenderDoc::InitialiseReplay`, call `SyncAvailableGPUThread()` before starting another enumeration thread, or skip the enumeration if one has already run. That removes the second thread that every run here has.
- **Fix the shared tables:** `InitReplayTables` shouldn't zero tables that another live replay or proxy device is using. Two options:
  - initialise them once, under a lock;
  - fill a local table, then copy it over.

  Either way, fill `EnumerateDeviceExtensionProperties` / `EnumerateDeviceLayerProperties` from the module, as `InitInstanceTable` does at lines 256–257.
- **Test scripts:** scripts run inside qrenderdoc shouldn't call `rd.InitialiseReplay` / `rd.ShutdownReplay`, because qrenderdoc has already initialised replay. That only avoids the trigger; it doesn't fix the race.

## Files

- **`d3d9_d3d9ex_crash/`:**
  - the cdb outputs
  - both log tails
  - the per-run summaries
  - `gpu_race.py`
- **Dumps (not committed):** kept locally in `C:\dumps`. These are `old_20261104_0806_*`, `old_20261104_0735_*` and `race01/04/07/11/12/14/15/18/20_*`, about 39–53 MB each.
- **Scratchpad:** the 60 runs' outputs and logs are in the scratchpad, `exc\runs\run01–60.txt/.log`.
