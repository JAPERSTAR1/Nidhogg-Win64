<#
.SYNOPSIS
One-time interactive helper to bind permanent `https://lobby.japer.me` via Cloudflare Tunnel
while preserving `japer.me` on GitHub Pages.

.DESCRIPTION
Prerequisites (one-time in browser):
  1. Create or sign in to a free Cloudflare account and add `japer.me` (Free plan).
  2. Confirm Cloudflare imported the 4 GitHub Pages A records for `@` (`japer.me`):
       185.199.108.153, 185.199.109.153, 185.199.110.153, 185.199.111.153 (DNS Only)
     so `https://japer.me` on GitHub Pages remains completely untouched.
  3. Update `japer.me` nameservers in Namecheap from BasicDNS to the two Cloudflare nameservers.
  4. Run this script (`.\deploy\setup-lobby-japer-me.ps1`) to authenticate `cloudflared`,
     create the `nidhogg-lobby` tunnel, add ONLY the `lobby.japer.me` CNAME, and write
     `deploy\cloudflared-config.yml` (credentials stay in `%USERPROFILE%\.cloudflared\`).
#>
param(
    [string]$TunnelName = "nidhogg-lobby",
    [string]$Hostname = "lobby.japer.me",
    [int]$LocalPort = 27880,
    [switch]$InstallStartupTask
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$cfExe = Join-Path $repoRoot "tools\cloudflared.exe"
if (-not (Test-Path $cfExe)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cfExe) | Out-Null
    Invoke-WebRequest -Uri "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe" -OutFile $cfExe -UseBasicParsing
}

$cfHome = Join-Path $env:USERPROFILE ".cloudflared"
if (-not (Test-Path (Join-Path $cfHome "cert.pem"))) {
    Write-Host "Step 1: Opening browser for Cloudflare Tunnel login (authorize japer.me)..." -ForegroundColor Cyan
    & $cfExe tunnel login
}

Write-Host "Step 2: Creating or locating tunnel '$TunnelName'..." -ForegroundColor Cyan
$listJson = & $cfExe tunnel list --output json | ConvertFrom-Json
$existing = $listJson | Where-Object { $_.name -eq $TunnelName } | Select-Object -First 1
if (-not $existing) {
    & $cfExe tunnel create $TunnelName
    $listJson = & $cfExe tunnel list --output json | ConvertFrom-Json
    $existing = $listJson | Where-Object { $_.name -eq $TunnelName } | Select-Object -First 1
}
if (-not $existing) { throw "Failed to create or locate tunnel '$TunnelName'." }
$tunnelId = $existing.id
$credPath = Join-Path $cfHome "$tunnelId.json"

Write-Host "Step 3: Routing DNS CNAME $Hostname -> $tunnelId.cfargotunnel.com (apex japer.me untouched)..." -ForegroundColor Cyan
& $cfExe tunnel route dns $TunnelName $Hostname

$cfgOut = Join-Path $repoRoot "deploy\cloudflared-config.yml"
@"
tunnel: $tunnelId
credentials-file: $credPath

ingress:
  - hostname: $Hostname
    service: http://127.0.0.1:$LocalPort
    originRequest:
      noTLSVerify: true
      connectTimeout: 10s
      keepAliveTimeout: 90s
  - service: http_status:404
"@ | Set-Content -Path $cfgOut -Encoding UTF8

Write-Host "Wrote $cfgOut" -ForegroundColor Green

if ($InstallStartupTask) {
    $startScript = Join-Path $repoRoot "start-server.ps1"
    $action = New-ScheduledTaskAction -Execute "powershell.exe" `
        -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$startScript`" -NamedTunnel"
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    Register-ScheduledTask -TaskName "NidhoggLobbyServer" -Action $action -Trigger $trigger -Force | Out-Null
    Write-Host "Registered Windows Logon Task 'NidhoggLobbyServer'." -ForegroundColor Green
}
