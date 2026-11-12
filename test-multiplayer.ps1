param([string]$Endpoint = 'https://lobby.japer.me', [string]$Assets = $env:NIDHOGG_ASSETS, [int]$Frames = 2900, [switch]$Combat)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not $Assets) { throw 'Set NIDHOGG_ASSETS or supply -Assets with your CUSA00920 folder.' }
    if ($Endpoint -eq 'http://127.0.0.1:27880') {
        if (-not (Test-NetConnection 127.0.0.1 -Port 27880 -InformationLevel Quiet)) { throw 'Start start-server.cmd first.' }
    }
    $health = Invoke-RestMethod -Uri ($Endpoint.TrimEnd('/') + '/health') -TimeoutSec 10
    if (-not $health.ok) { throw 'Lobby health check failed' }
    & node --test server/test.mjs server/udp-relay.test.mjs
    if ($LASTEXITCODE) { throw 'Backend tests failed' }
    $arguments = @('tests/multiplayer_pair.py','--assets',$Assets,'--endpoint',$Endpoint,'--frames',"$Frames")
    if ($Combat) { $arguments += '--combat' }
    & python @arguments
    if ($LASTEXITCODE) { throw 'Two-process verification failed; see tests/results/internet-local/report.json' }
} finally { Pop-Location }
