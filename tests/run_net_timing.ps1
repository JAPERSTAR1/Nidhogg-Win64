$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 toolchain not found' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$out = Join-Path $root 'port/out/net-timing-test'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$source = Join-Path $PSScriptRoot 'net_timing.cpp'
$cmd = "call `"$vcvars`" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /Fe:`"$out\timing.exe`" /Fo:`"$out\timing.obj`" `"$source`" && `"$out\timing.exe`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Network timing model tests failed' }
