#!/usr/bin/env python3
# Runs run_tests.py on a local Windows build, with the arguments it's given, for machines where
# RenderDoc can't register its Vulkan layer:
#
#   python util\test\run_tests_local.py -t D3D9
#   python util\test\run_tests_local.py --x86 -t D3D9
#
# - --x86 runs the 32-bit demos (util\test\demos_x86.exe), captured and replayed by the 64-bit build
#   as qrenderdoc does with a 32-bit program. It needs renderdoc.sln and demos.sln built for x86 too.
# - --renderdoc defaults to x64\Development, and --pyrenderdoc to the module
#   util\test\build_pymodule_py3.ps1 builds against this Python (x64\Development\py3module\pymodules).
# - run_tests.py stops when the Vulkan layer needs registering, which needs admin rights when an
#   installed RenderDoc owns the registration. This makes NeedVulkanLayerRegistration return False,
#   so tests that don't use Vulkan run. Vulkan tests will fail or be skipped.
#
# Everything else is run_tests.py, unchanged.
import os
import runpy
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(here))
args = sys.argv[1:]


def arg(short, long, default):
    for name in (short, long):
        if name in args:
            return args[args.index(name) + 1]
    args.extend([long, default])
    return default


x86 = '--x86' in args
if x86:
    args.remove('--x86')

renderdoc_dir = arg('-r', '--renderdoc', os.path.join(root, 'x64', 'Development'))
module_dir = arg('-p', '--pyrenderdoc', os.path.join(root, 'x64', 'Development', 'py3module', 'pymodules'))

if not os.path.exists(os.path.join(module_dir, 'renderdoc.pyd')):
    sys.exit(f"No renderdoc.pyd in {module_dir}: run util\\test\\build_pymodule_py3.ps1 first")

if x86:
    # 32-bit demos, still captured and replayed by the 64-bit build. Launching a 32-bit process
    # from a 64-bit dev build goes through Win32\<config>\renderdoccmd.exe (win32_process.cpp),
    # which injects the 32-bit renderdoc.dll, so renderdoc.sln has to be built for x86 as well.
    arg('--demos-binary', '--demos-binary', os.path.join(here, 'demos_x86.exe'))
    config = os.path.basename(os.path.normpath(renderdoc_dir))
    for f in ('renderdoccmd.exe', 'renderdoc.dll'):
        path = os.path.join(root, 'Win32', config, f)
        if not os.path.exists(path):
            sys.exit(f"No {path}: build renderdoc.sln {config} x86 for --x86")

os.add_dll_directory(renderdoc_dir)
sys.path.insert(0, module_dir)
sys.path.insert(0, here)
import renderdoc

renderdoc.NeedVulkanLayerRegistration = lambda info: False

script = os.path.join(here, 'run_tests.py')
sys.argv = [script] + args
runpy.run_path(script, run_name='__main__')
