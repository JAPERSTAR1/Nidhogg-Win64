<#
.SYNOPSIS
Comprehensive Automated Multiplayer Verification Suite (`test-multiplayer.ps1`).
Tests:
  1. NidhoggServer.exe startup + `Test-NetConnection 127.0.0.1 -Port 27880` + `/api/health`
  2. REST API security & room rules:
     - Public room creation & discovery
     - Private room creation (verified NEVER shown in public listing or Quick Match)
     - Join by 6-char room code + invalid code 404 rejection
     - Room capacity = 2 enforcement (3rd player rejected with HTTP 409)
     - Live visibility switching (Private -> Public -> Private) without dropping connected guest
     - Host kick player (guest receives kicked=true)
     - Protocol version mismatch rejection (HTTP 426)
  3. Two-Instance Real Gameplay Match A (Direct P2P Encrypted UDP, Public Room + Quick Match,
     combat + kills + screen transitions + victory + rematch + 0 state-hash desyncs + screenshots)
  4. Two-Instance Real Gameplay Match B (Private Room + 6-char Room Code + Binary WebSocket
     Relay `NIDHOGG_FORCE_RELAY=1` simulating Symmetric NAT / CGNAT, 0 desyncs)
  5. Network Conditions Matrix: 30 ms, 80 ms, and 150 ms RTT + Jitter + Packet Loss across
     two live Nidhogg instances with frame-by-frame state-hash verification (`DESYNC = 0`)
  6. Mid-match peer disconnect handling & server-outage clean error handling
  7. Live Public Internet Cloudflare Edge HTTPS + WSS Tunnel test (`-IncludeCloudflareEdge`)
#>
param(
    [switch]$IncludeCloudflareEdge = $true
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$gameExe = Join-Path $root "port\out\Nidhogg.exe"
$srvExe  = Join-Path $root "port\out\NidhoggServer.exe"
$assets  = "C:\Users\puy12\Downloads\g\CUSA00920"
$outDir  = Join-Path $root "tests\results\multiplayer"

if (-not (Test-Path $gameExe) -or -not (Test-Path $srvExe)) {
    & (Join-Path $root "port\build.ps1")
}

Get-Process nidhogg_host, Nidhogg, NidhoggServer, cloudflared -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Start-Sleep -Milliseconds 300
Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$script:Checks = @()
function Assert-Mp([string]$Group, [string]$Title, [bool]$Ok, [string]$Detail) {
    $st = if ($Ok) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1} :: {2} -- {3}" -f $st, $Group, $Title, $Detail) `
        -ForegroundColor $(if ($Ok) { "Green" } else { "Red" })
    $script:Checks += [PSCustomObject]@{ Group = $Group; Title = $Title; Status = $st; Detail = $Detail }
