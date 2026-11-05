# Taken from https://github.com/mhammond/pywin32/blob/main/.github/workflows/install-vs-components.py

# See https://github.com/actions/runner-images/issues/9701
# Adapted from https://github.com/actions/runner-images/issues/9873#issuecomment-2139288682

import os
import platform
import sys
import datetime
from itertools import chain
from subprocess import check_call, check_output

os.chdir("C:/Program Files (x86)/Microsoft Visual Studio/Installer")
vs_install_path = check_output(
    (
        "vswhere.exe",
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.Component.MSBuild",
        "-property",
        "installationPath",
    ),
    text=True,
    shell=True,
).strip()
components_to_add = (
    ["Microsoft.VisualStudio.Component.VC.140"]
)
args = (
    "vs_installer.exe",
    "modify",
    "--installPath",
    vs_install_path,
    *chain.from_iterable([("--add", component) for component in components_to_add]),
    "--quiet",
    "--norestart",
)
print(*args)

# run 10 times because even running twice doesn't always work
for i in range(10):
    print("Run " + str(i))
    print(datetime.datetime.now().time())
    try:
        check_call(args)
    except:
        print("Call error:" + str(sys.exc_info()))
    print(datetime.datetime.now().time())
