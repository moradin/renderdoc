#!/bin/bash

# find new msbuild
MSBUILD="$(/c/Program\ Files\ \(x86\)/Microsoft\ Visual\ Studio/Installer/vswhere.exe -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')"

# build with new enough toolset to link shaderc
"$MSBUILD" -p:'Configuration=Release;Platform=x64' -p:'PlatformToolset=v143'
