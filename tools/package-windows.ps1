param([string]$Endpoint = 'https://lobby.japer.me')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repo 'dist/Nidhogg-Windows'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'port/out/Nidhogg.exe') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/MULTIPLAYER.md') -Destination (Join-Path $destination 'MULTIPLAYER.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/NETWORK-VERIFICATION.md') -Destination (Join-Path $destination 'NETWORK-VERIFICATION.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'port/src/atrac9/LICENSE') -Destination (Join-Path $destination 'LibAtrac9-LICENSE.txt') -Force
$launch = '@echo off' + "`r`n" + 'start "Nidhogg Online" /D "%~dp0" "%~dp0Nidhogg.exe" --online --lobby-url "' + $Endpoint + '"' + "`r`n"
[IO.File]::WriteAllText((Join-Path $destination 'Play Online.cmd'), $launch, [Text.Encoding]::ASCII)
@'
Run Nidhogg.exe for the game, or Play Online.cmd for the online menu.
Select your CUSA00920 game folder when prompted. Original assets are not included.
F1 opens settings. Each player uses their local Player 1 controls online.
The endpoint in Play Online.cmd must be an actually deployed lobby.
lobby.japer.me is deployed; recent DNS migration may take time at some providers.
The operator PC must remain on for the lobby and relay to be available.
No VPN, Node.js, cloudflared or PowerShell is required on player PCs.
'@ | Set-Content -LiteralPath (Join-Path $destination 'READ-ME.txt') -Encoding UTF8
$archive = Join-Path $repo 'dist/Nidhogg-Windows.zip'
$manifest = @('Nidhogg.exe', 'Play Online.cmd', 'READ-ME.txt', 'MULTIPLAYER.md', 'NETWORK-VERIFICATION.md', 'THIRD_PARTY_NOTICES.md', 'LibAtrac9-LICENSE.txt')
$files = @($manifest | ForEach-Object { Join-Path $destination $_ })
Compress-Archive -LiteralPath $files -DestinationPath $archive -Force
foreach ($file in @((Join-Path $destination 'Nidhogg.exe'), $archive)) {
    $stream = [IO.File]::OpenRead($file)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { Write-Output ($file + ' SHA256=' + [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()) }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
