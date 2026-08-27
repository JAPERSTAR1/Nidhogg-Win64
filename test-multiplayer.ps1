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
}

function Clear-NetEnv {
    foreach ($k in @("NIDHOGG_QA_UNFOCUSED","NIDHOGG_UNCAPPED","NIDHOGG_SCREENSHOT_DIR",
                     "NIDHOGG_SCREENSHOT","NIDHOGG_AUTOPRESS","NIDHOGG_AUTOPRESS_P2",
                     "NIDHOGG_AUTOPRESS_MATCH","NIDHOGG_OPEN_SETTINGS","NIDHOGG_OPEN_ONLINE",
                     "NIDHOGG_SERVER_URL","NIDHOGG_NET_AUTO","NIDHOGG_NET_ARG",
                     "NIDHOGG_NET_CODE_OUT","NIDHOGG_NET_STAGE","NIDHOGG_NET_SIM_LATENCY_MS",
                     "NIDHOGG_NET_SIM_JITTER_MS","NIDHOGG_NET_SIM_LOSS_PCT",
                     "NIDHOGG_FORCE_RELAY","NIDHOGG_SKIP_STUN","NIDHOGG_PROTOCOL_OVERRIDE")) {
        Remove-Item "Env:\$k" -ErrorAction SilentlyContinue
    }
}

function Invoke-Api([string]$Method, [string]$Url, [string]$Body = "", [hashtable]$ExtraHeaders = @{}) {
    $hdrs = @{ "X-Nidhogg-Version" = "nidhogg-win64-1.0" }
    foreach ($kv in $ExtraHeaders.GetEnumerator()) { $hdrs[$kv.Key] = $kv.Value }
    try {
        if ($Method -eq "GET") {
            $r = Invoke-WebRequest -Uri $Url -Method Get -Headers $hdrs -UseBasicParsing -TimeoutSec 5
        } else {
            $r = Invoke-WebRequest -Uri $Url -Method $Method -Headers $hdrs -ContentType "application/json" -Body $Body -UseBasicParsing -TimeoutSec 5
        }
        return [PSCustomObject]@{ Code = [int]$r.StatusCode; Json = ($r.Content | ConvertFrom-Json); Raw = $r.Content }
    } catch {
        $code = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
        return [PSCustomObject]@{ Code = $code; Json = $null; Raw = $_.Exception.Message }
    }
}

# ---------------------------------------------------------------------------
# 1. Start NidhoggServer.exe (uses 27880 if free, or 27885 if an unrelated app holds 27880)
# ---------------------------------------------------------------------------
$testPort = 27880
$preCheck = Invoke-Api "GET" "http://127.0.0.1:27880/api/health"
$occupiedByOther = (Get-NetTCPConnection -LocalPort 27880 -State Listen -ErrorAction SilentlyContinue) -and `
                   (-not ($preCheck.Raw -match "nidhogg-lobby"))
if ($occupiedByOther) {
    Write-Host "Note: 127.0.0.1:27880 is currently bound by an unrelated local process; using port 27885 without disturbing it." -ForegroundColor Yellow
    $testPort = 27885
}
$baseHttp = "http://127.0.0.1:$testPort"
$srvProc = Start-Process -FilePath $srvExe -ArgumentList @("--port","$testPort") -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500
$tnc = Test-NetConnection 127.0.0.1 -Port $testPort -WarningAction SilentlyContinue
$health = Invoke-Api "GET" "$baseHttp/api/health"
Assert-Mp "Backend" "Test-NetConnection 127.0.0.1:$testPort & /api/health" `
    ($tnc.TcpTestSucceeded -and ($health.Code -eq 200) -and $health.Json.ok -and ($health.Json.service -eq "nidhogg-lobby")) `
    "TcpTestSucceeded=$($tnc.TcpTestSucceeded), port=$testPort, version=$($health.Json.version)"

# ---------------------------------------------------------------------------
# 2. Public & Private Room Rules, Codes, Visibility Switching, Capacity=2, Kick, Version Check
# ---------------------------------------------------------------------------
$verMismatch = Invoke-Api "GET" "$baseHttp/api/rooms" "" @{ "X-Nidhogg-Version" = "nidhogg-win64-0.1-old" }
Assert-Mp "Backend" "Protocol version mismatch rejected (HTTP 426)" `
    ($verMismatch.Code -eq 426) "status=$($verMismatch.Code)"

# Create Private Room -> must NEVER appear in GET /api/rooms or QuickMatch
$priv = Invoke-Api "POST" "$baseHttp/api/rooms/create" '{"roomName":"Secret Duel","hostName":"Alice","region":"us-east","isPublic":false,"stageIndex":0,"lanIp":"127.0.0.1","lanPort":40001}'
$pubList1 = Invoke-Api "GET" "$baseHttp/api/rooms?available=0"
$qmEmpty  = Invoke-Api "POST" "$baseHttp/api/quickmatch" '{"region":"us-east","playerName":"Searcher","lanIp":"127.0.0.1","lanPort":40002}'
Assert-Mp "Rooms" "Private room hidden from public browser & Quick Match" `
    (($priv.Code -eq 200) -and ($pubList1.Json.rooms.Count -eq 0) -and ($qmEmpty.Code -eq 404)) `
    "privateCode=$($priv.Json.roomCode), publicListed=$($pubList1.Json.rooms.Count), qmStatus=$($qmEmpty.Code)"

# Invalid room code rejected
$badCode = Invoke-Api "POST" "$baseHttp/api/rooms/join_code" '{"roomCode":"ZZZZZZ","playerName":"Bob","lanIp":"127.0.0.1","lanPort":40002}'
Assert-Mp "Rooms" "Invalid 6-char room code rejected (HTTP 404)" ($badCode.Code -eq 404) "status=$($badCode.Code)"

# Valid room code join -> succeeds (2/2)
$joinPriv = Invoke-Api "POST" "$baseHttp/api/rooms/join_code" ("{`"roomCode`":`"$($priv.Json.roomCode)`",`"playerName`":`"Bob`",`"lanIp`":`"127.0.0.1`",`"lanPort`":40002}")
# 3rd player attempt -> rejected (HTTP 409 Capacity=2)
$joinThird = Invoke-Api "POST" "$baseHttp/api/rooms/join_code" ("{`"roomCode`":`"$($priv.Json.roomCode)`",`"playerName`":`"Charlie`",`"lanIp`":`"127.0.0.1`",`"lanPort`":40003}")
Assert-Mp "Rooms" "Join Private Room by code & enforce max 2 players (HTTP 409)" `
    (($joinPriv.Code -eq 200) -and ($joinPriv.Json.playerCount -eq 2) -and ($joinThird.Code -eq 409)) `
    "p2Join=$($joinPriv.Code) (count=$($joinPriv.Json.playerCount)), p3Join=$($joinThird.Code)"

# Live visibility switch Private -> Public while Bob stays connected!
$visPub = Invoke-Api "POST" "$baseHttp/api/rooms/visibility" ("{`"roomId`":`"$($priv.Json.roomId)`",`"authToken`":`"$($priv.Json.authToken)`",`"isPublic`":true}")
$pubList2 = Invoke-Api "GET" "$baseHttp/api/rooms?available=0"
$visPriv = Invoke-Api "POST" "$baseHttp/api/rooms/visibility" ("{`"roomId`":`"$($priv.Json.roomId)`",`"authToken`":`"$($priv.Json.authToken)`",`"isPublic`":false}")
$pubList3 = Invoke-Api "GET" "$baseHttp/api/rooms?available=0"
Assert-Mp "Rooms" "Live room visibility switching (Private <-> Public) keeps players connected" `
    (($visPub.Json.guestConnected -eq $true) -and ($pubList2.Json.rooms.Count -eq 1) -and ($pubList3.Json.rooms.Count -eq 0)) `
    "publicCountAfterPub=$($pubList2.Json.rooms.Count), publicCountAfterPriv=$($pubList3.Json.rooms.Count), guestStillConnected=$($visPriv.Json.guestConnected)"

# Host Kick Guest -> Bob's next heartbeat reports kicked=true
$kick = Invoke-Api "POST" "$baseHttp/api/rooms/kick" ("{`"roomId`":`"$($priv.Json.roomId)`",`"authToken`":`"$($priv.Json.authToken)`"}")
$bobHb = Invoke-Api "POST" "$baseHttp/api/rooms/heartbeat" ("{`"roomId`":`"$($priv.Json.roomId)`",`"authToken`":`"$($joinPriv.Json.authToken)`"}")
$leave = Invoke-Api "POST" "$baseHttp/api/rooms/leave" ("{`"roomId`":`"$($priv.Json.roomId)`",`"authToken`":`"$($priv.Json.authToken)`"}")
Assert-Mp "Rooms" "Host Kick Player & Close Room" `
    (($kick.Json.playerCount -eq 1) -and ($bobHb.Json.kicked -eq $true)) `
    "afterKickCount=$($kick.Json.playerCount), bobKicked=$($bobHb.Json.kicked)"

# ---------------------------------------------------------------------------
# Helper: Run Two Independent Nidhogg.exe Processes in a Real Synchronized Match
