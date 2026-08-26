<#
.SYNOPSIS
Starts the Nidhogg Multiplayer Lobby & Binary WebSocket/UDP Relay Server (`NidhoggServer.exe`),
and optionally exposes it to the public internet via Cloudflare Tunnel (`lobby.japer.me` or Quick Tunnel).

.DESCRIPTION
Note: When self-hosting on this PC, the multiplayer server is available only while this PC
is powered on and `NidhoggServer.exe` (plus `cloudflared`, for public internet access) is running.

.EXAMPLE
.\start-server.ps1
Starts local server on http://0.0.0.0:27880 (and ws://0.0.0.0:27880/ws + udp://0.0.0.0:27881).

.EXAMPLE
.\start-server.ps1 -QuickTunnel
Starts NidhoggServer.exe AND opens a free public HTTPS/WSS Cloudflare Quick Tunnel URL immediately
with zero account login or router port forwarding.

.EXAMPLE
.\start-server.ps1 -NamedTunnel
Starts NidhoggServer.exe AND runs the configured `lobby.japer.me` Cloudflare Named Tunnel.
#>
param(
    [int]$Port = 27880,
    [switch]$QuickTunnel,
    [switch]$NamedTunnel,
    [switch]$Background
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srvCandidates = @(
    (Join-Path $root "NidhoggServer.exe"),
    (Join-Path $root "port\out\NidhoggServer.exe")
)
$srvExe = $srvCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $srvExe) {
    Write-Host "NidhoggServer.exe not found; building..." -ForegroundColor Yellow
    & (Join-Path $root "port\build.ps1")
    $srvExe = Join-Path $root "port\out\NidhoggServer.exe"
}

$existing = Get-Process NidhoggServer -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "NidhoggServer.exe is already running (PID $($existing[0].Id)) on port $Port." -ForegroundColor Cyan
} else {
    if ($Background -or $QuickTunnel -or $NamedTunnel) {
        $p = Start-Process -FilePath $srvExe -ArgumentList @("--port", "$Port") -PassThru -WindowStyle Minimized
