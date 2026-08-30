# Locate MSVC, build the host, run it.
#   .\build.ps1            compile
#   .\build.ps1 run        compile and run
#   .\build.ps1 clean

param([string]$Target = "build")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root "out"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no MSVC x64 toolset found" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat missing at $vcvars" }

if ($Target -eq "clean") {
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    return
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

New-Item -ItemType Directory -Force -Path (Join-Path $out "c") | Out-Null
$cpp = Get-ChildItem (Join-Path $root "src") -Recurse -Filter *.cpp | ForEach-Object { '"' + $_.FullName + '"' }
$csrc = Get-ChildItem (Join-Path $root "src") -Recurse -Filter *.c | ForEach-Object { '"' + $_.FullName + '"' }
$cStep = if ($csrc) { "cl /nologo /c /TC /O2 /MT /W0 /Fo:`"$out\c\\`" $($csrc -join ' ') && " } else { "" }
$objs = if ($csrc) { "`"$out\c\*.obj`"" } else { "" }
$cmd = "call `"$vcvars`" >nul && ${cStep}cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /WX- /Fe:`"$out\nidhogg_host.exe`" /Fo:`"$out\\`" $($cpp -join ' ') $objs /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib windowscodecs.lib xinput.lib winmm.lib d3d11.lib d3dcompiler.lib dxgi.lib ws2_32.lib bcrypt.lib winhttp.lib"
Write-Host "building..." -ForegroundColor DarkGray
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $out "nidhogg_host.exe"
Write-Host "built $exe" -ForegroundColor Green

if ($Target -eq "run") {
    Push-Location (Split-Path -Parent $root)
    try { & $exe (Join-Path $root "..\work\nidhogg.elf") } finally { Pop-Location }
}
