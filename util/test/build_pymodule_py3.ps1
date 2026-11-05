# Builds the renderdoc python module against the installed Python 3 (python on PATH, or -Python),
# for util\test\run_tests.py, which needs Python 3.7 or later. The solution build's module in
# x64\<Config>\pymodules is built against the bundled Python 3.6 for qrenderdoc, and is left as it is.
#
#   powershell -File util\test\build_pymodule_py3.ps1 [-Config Development] [-Python python]
#
# The module ends up in x64\<Config>\py3module\pymodules: pass that to run_tests.py's --pyrenderdoc
# (util\test\run_tests_local.py does). Build renderdoc.sln first; rebuild this after changes to
# renderdoc's public API.
param([string]$Config = 'Development', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'

$root = Resolve-Path "$PSScriptRoot\..\.."
$out = "$root\x64\$Config\py3module"
$pm = "$root\x64\$Config\pymodules"

$base = & $Python -c "import sys; print(sys.base_prefix)"
$ver = & $Python -c "import sys; print('%d%d' % sys.version_info[:2])"
if (-not (Test-Path "$base\include\Python.h") -or -not (Test-Path "$base\libs\python$ver.lib")) {
  throw "$base has no include\Python.h or libs\python$ver.lib"
}

# python.props wants include\, the .lib and a pythonXY.zip (the embedded standard library, which only
# qrenderdoc uses) in the override folder
$override = "$out\python$ver"
New-Item -ItemType Directory -Force "$override\libs" | Out-Null
Copy-Item "$base\include" $override -Recurse -Force
Copy-Item "$base\libs\python$ver.lib" "$override\libs" -Force
if (-not (Test-Path "$override\python$ver.zip")) {
  & $Python -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1], 'w').close()" "$override\python$ver.zip"
}

# the project writes to pymodules, so keep the 3.6 module aside and put it back afterwards
$bak = "$out\pymodules36"
if (Test-Path $bak) { Remove-Item $bak -Recurse -Force }
Copy-Item $pm $bak -Recurse
# the solution's module is newer than this build's objects, so without this the link is skipped
Remove-Item "$pm\renderdoc.pyd"

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
try {
  & $msbuild "$root\qrenderdoc\Code\pyrenderdoc\pyrenderdoc_module.vcxproj" /p:Configuration=$Config /p:Platform=x64 `
    /p:BuildProjectReferences=false "/p:SolutionDir=$root\" "/p:VSPythonOverridePath=$override" "/p:IntDir=$out\obj\" `
    /v:minimal /nologo
  $code = $LASTEXITCODE
  if ($code -eq 0) {
    New-Item -ItemType Directory -Force "$out\pymodules" | Out-Null
    Copy-Item "$pm\renderdoc.pyd" "$out\pymodules\renderdoc.pyd" -Force
  }
} finally {
  Copy-Item "$bak\*" $pm -Recurse -Force
}
if ($code -ne 0) { throw "The build failed ($code)" }

$imports = (Select-String -Path "$out\pymodules\renderdoc.pyd" -Pattern 'python3\d+\.dll' -AllMatches).Matches.Value | Select-Object -Unique
"Built $out\pymodules\renderdoc.pyd, which imports $imports"
