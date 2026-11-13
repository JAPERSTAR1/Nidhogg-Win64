param([string]$Endpoint = 'https://lobby.japer.me', [string]$Version = '1.1.0-rc.2')
$ErrorActionPreference = 'Stop'
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
$repo = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repo 'dist/Nidhogg-Windows'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'port/out/Nidhogg.exe') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/MULTIPLAYER.md') -Destination (Join-Path $destination 'MULTIPLAYER.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/NETWORK-VERIFICATION.md') -Destination (Join-Path $destination 'NETWORK-VERIFICATION.md') -Force
foreach ($guide in @('NATIVE-FRONTEND.md','INTEGRATED-UI-PERFORMANCE.md','REDUCED-FLASHES.md')) {
    Copy-Item -LiteralPath (Join-Path $repo "docs/$guide") -Destination $destination -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'docs/AZURE-VERIFICATION.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/AZURE-DEPLOYMENT.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $repo 'port/src/atrac9/LICENSE') -Destination (Join-Path $destination 'LibAtrac9-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'port/resources/nidhogg-logo.png') -Destination (Join-Path $destination 'Nidhogg.png') -Force
Copy-Item -LiteralPath (Join-Path $repo "docs/RELEASE-v$Version.md") -Destination (Join-Path $destination 'RELEASE-NOTES.md') -Force
$verification = Join-Path $repo "docs/RELEASE-VERIFICATION-v$Version.md"
if (-not (Test-Path -LiteralPath $verification)) { $verification = Join-Path $repo 'docs/RELEASE-VERIFICATION.md' }
Copy-Item -LiteralPath $verification -Destination (Join-Path $destination 'RELEASE-VERIFICATION.md') -Force
$launch = '@echo off' + "`r`n" + 'start "Nidhogg Online" /D "%~dp0" "%~dp0Nidhogg.exe" --online --lobby-url "' + $Endpoint + '"' + "`r`n"
[IO.File]::WriteAllText((Join-Path $destination 'Play Online.cmd'), $launch, [Text.Encoding]::ASCII)
@'
Run Nidhogg.exe for the game, or Play Online.cmd for the online menu.
Select your CUSA00920 game folder when prompted. Original assets are not included.
Alternatively place your CUSA00920 folder beside Nidhogg.exe, then launch it.
Online multiplayer is a release candidate; independent-network play is unverified.
Options/F1 opens the full settings screen. F3 toggles the connection HUD.
Multiplayer lists public sessions and offers public/private hosting or code joining.
The host chooses MAP and starts once a second player connects. No Ready step.
Both players must use this updated runtime. Each uses local Player 1 controls.
Match startup keeps the same application process and window.
240 FPS is a presentation option; gameplay and animation remain 60 Hz.
The endpoint in Play Online.cmd must be an actually deployed lobby.
The hosted service runs independently of the operator PC, within its credit limits.
No VPN, Node.js, cloudflared or PowerShell is required on player PCs.
'@ | Set-Content -LiteralPath (Join-Path $destination 'READ-ME.txt') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $repo 'docs/QUICKSTART-AR.txt') -Destination (Join-Path $destination 'READ-ME-AR.txt') -Force
$sourceCommit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the source revision' }
$release = [ordered]@{
    version = $Version
    sourceCommit = $sourceCommit
    platform = 'Windows x64'
    originalAssetsIncluded = $false
    onlineStatus = 'experimental; independent-network gameplay unverified'
    executableSha256 = Get-Sha256 (Join-Path $destination 'Nidhogg.exe')
}
$release | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'release-manifest.json') -Encoding UTF8
$archive = Join-Path $repo 'dist/Nidhogg-Windows.zip'
$manifest = @('Nidhogg.exe', 'Nidhogg.png', 'Play Online.cmd', 'READ-ME.txt', 'READ-ME-AR.txt', 'RELEASE-NOTES.md', 'RELEASE-VERIFICATION.md', 'release-manifest.json', 'MULTIPLAYER.md', 'NETWORK-VERIFICATION.md', 'AZURE-VERIFICATION.md', 'AZURE-DEPLOYMENT.md', 'THIRD_PARTY_NOTICES.md', 'LibAtrac9-LICENSE.txt', 'NATIVE-FRONTEND.md', 'INTEGRATED-UI-PERFORMANCE.md', 'REDUCED-FLASHES.md')
$files = @($manifest | ForEach-Object { Join-Path $destination $_ })
Compress-Archive -LiteralPath $files -DestinationPath $archive -Force
$versionedArchive = Join-Path $repo "dist/Nidhogg-Windows-v$Version-x64.zip"
Copy-Item -LiteralPath $archive -Destination $versionedArchive -Force
$archiveHash = Get-Sha256 $versionedArchive
$checksum = $archiveHash + '  ' + (Split-Path -Leaf $versionedArchive) + "`n"
$checksum += $release.executableSha256 + '  Nidhogg.exe' + "`n"
[IO.File]::WriteAllText((Join-Path $repo 'dist/SHA256SUMS.txt'), $checksum, [Text.UTF8Encoding]::new($false))
foreach ($file in @((Join-Path $destination 'Nidhogg.exe'), $archive)) {
    $stream = [IO.File]::OpenRead($file)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { Write-Output ($file + ' SHA256=' + [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()) }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
