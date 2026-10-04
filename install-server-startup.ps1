param([Parameter(Mandatory=$true)][string]$TunnelConfig, [switch]$Remove)
$ErrorActionPreference = 'Stop'
$startup = [Environment]::GetFolderPath('Startup')
$shortcutPath = Join-Path $startup 'Nidhogg Lobby.lnk'
if ($Remove) {
    if (Test-Path -LiteralPath $shortcutPath) { Remove-Item -LiteralPath $shortcutPath }
    Write-Host 'Removed Nidhogg lobby startup shortcut.'
    return
}
$configPath = (Resolve-Path -LiteralPath $TunnelConfig).Path
if ((Get-Content -LiteralPath $configPath -Raw) -match 'REPLACE_') { throw 'Fill in the named tunnel configuration first.' }
if (-not (Test-Path (Join-Path $PSScriptRoot '.tools/cloudflared.exe'))) { throw 'Missing operator cloudflared executable.' }
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$shortcut.Arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot 'start-server.ps1') + '" -TunnelConfig "' + $configPath + '"'
$shortcut.WorkingDirectory = $PSScriptRoot
$shortcut.WindowStyle = 7
$shortcut.Save()
Write-Host 'Configured lobby startup at the next sign-in. Use stop-server.ps1 to stop it.'
