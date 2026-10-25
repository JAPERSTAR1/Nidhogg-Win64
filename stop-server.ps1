$ErrorActionPreference = 'Stop'
$statePath = Join-Path $PSScriptRoot '.tools/server-processes.json'
if (-not (Test-Path -LiteralPath $statePath)) { Write-Host 'No managed Nidhogg server is running.'; return }
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
foreach ($entry in $state) {
    $process = Get-Process -Id $entry.id -ErrorAction SilentlyContinue
    # PID reuse must never stop another application.
    if ($process -and $process.ProcessName -eq $entry.name -and
        $process.StartTime.ToUniversalTime().Ticks.ToString() -eq $entry.started) {
        Stop-Process -Id $process.Id
    }
}
Remove-Item -LiteralPath $statePath -ErrorAction SilentlyContinue
Write-Host 'Stopped the managed Nidhogg lobby and tunnel.'
