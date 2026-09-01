<#
.SYNOPSIS
Launches Nidhogg (or NidhoggLauncher.exe) with optional Online Multiplayer or Server URL overrides.
Ordinary players can simply double-click `NidhoggLauncher.exe` or `Nidhogg.exe` directly.
#>
param(
    [switch]$Launcher,
    [switch]$Online,
    [string]$ServerUrl = "",
    [string]$UserDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Launcher) {
    $lnch = @(
        (Join-Path $root "NidhoggLauncher.exe"),
        (Join-Path $root "port\out\NidhoggLauncher.exe")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $lnch) { throw "NidhoggLauncher.exe not found. Run port\build.ps1 first." }
    Start-Process -FilePath $lnch
    return
}

$game = @(
    (Join-Path $root "Nidhogg.exe"),
    (Join-Path $root "port\out\Nidhogg.exe"),
    (Join-Path $root "port\out\nidhogg_host.exe")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $game) { throw "Nidhogg.exe not found. Run port\build.ps1 first." }

if ($ServerUrl) { $env:NIDHOGG_SERVER_URL = $ServerUrl }
$argList = @()
if ($Online) { $argList += "--online" }
if ($UserDir) { $argList += @("--user-dir", $UserDir) }

Start-Process -FilePath $game -ArgumentList $argList
