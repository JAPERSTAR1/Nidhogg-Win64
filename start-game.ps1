param([string]$Endpoint = 'https://lobby.japer.me', [string]$Assets = '', [switch]$Online)
$exe = Join-Path $PSScriptRoot 'port/out/Nidhogg.exe'
if (-not (Test-Path $exe)) { throw 'Build the port with port/build.ps1 first.' }
$arguments = @('--lobby-url',('"' + $Endpoint + '"'))
if ($Online) { $arguments += '--online' }
if ($Assets) { $arguments += @('--assets',('"' + $Assets + '"')) }
Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
