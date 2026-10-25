param([int]$Port = 27880, [string]$TunnelConfig = '')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw 'Server operator: install Node.js 20 or newer. Players do not need Node.' }
    if (-not (Test-Path 'server/node_modules/ws')) {
        Push-Location server
        try { & npm.cmd ci --ignore-scripts --no-fund; if ($LASTEXITCODE) { throw 'Dependency installation failed' } }
        finally { Pop-Location }
    }
    $env:PORT = "$Port"
    $env:BIND = '127.0.0.1'
    $tunnel = $null
    New-Item -ItemType Directory -Force -Path '.tools' | Out-Null
    $statePath = Join-Path $PSScriptRoot '.tools/server-processes.json'
    if (Test-Path -LiteralPath $statePath) {
        foreach ($entry in (Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json)) {
            $existing = Get-Process -Id $entry.id -ErrorAction SilentlyContinue
            if ($existing -and $existing.ProcessName -eq $entry.name -and
                $existing.StartTime.ToUniversalTime().Ticks.ToString() -eq $entry.started) {
                throw 'A managed lobby is already running. Stop it with stop-server.ps1 first.'
            }
        }
    }
    if ($TunnelConfig) {
        $cloudflared = Join-Path $PSScriptRoot '.tools/cloudflared.exe'
        if (-not (Test-Path $cloudflared)) { throw 'Place cloudflared.exe in .tools first; see docs/MULTIPLAYER.md' }
        $resolvedConfig = (Resolve-Path -LiteralPath $TunnelConfig).Path
        $tunnel = Start-Process -FilePath $cloudflared -ArgumentList @('tunnel','--config',('"' + $resolvedConfig + '"'),'run') -WindowStyle Hidden -PassThru
    }
    $node = (Get-Command node).Source
    $serverPath = Join-Path $PSScriptRoot 'server/server.mjs'
    $process = $null
    try {
        $process = Start-Process -FilePath $node -ArgumentList ('"' + $serverPath + '"') -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $PSScriptRoot '.tools/server.log') `
            -RedirectStandardError (Join-Path $PSScriptRoot '.tools/server.err.log')
        $owned = @($process)
        if ($tunnel) { $owned += $tunnel }
        @($owned | ForEach-Object { @{ id = $_.Id; name = $_.ProcessName; started = $_.StartTime.ToUniversalTime().Ticks.ToString() } }) |
            ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
        Write-Host "Lobby process $($process.Id); health http://127.0.0.1:$Port/health. Ctrl+C or stop-server.ps1 stops it."
        $process.WaitForExit()
        if ($process.ExitCode) { throw "Lobby stopped with code $($process.ExitCode); see .tools/server.err.log" }
    }
    finally {
        if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
        if ($tunnel -and -not $tunnel.HasExited) { Stop-Process -Id $tunnel.Id }
        Remove-Item -LiteralPath $statePath -ErrorAction SilentlyContinue
    }
} finally { Pop-Location }
