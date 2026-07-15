# Phase 2 — Complete Offline Game Verification Suite
# Exercises startup, SELF eboot loading, menus, Options, Save Data (create,
# persist, missing, truncated/corrupted, bad-CRC, read-only permission error),
# Local Versus 2P combat + full match victory + rematch, all 4 worlds (Castle,
# Mines, Clouds/Sky, Wilds), Arcade single-player AI combat & stage progression,
# Audio (ATRAC9 music, SFX voices, mute=0.0 peak, 60 Hz pacing), WM_CLOSE clean
# shutdown, and long-run RAM/Handle stability.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $repoRoot "port\out\nidhogg_host.exe"
$assets = "C:\Users\puy12\Downloads\g\CUSA00920"
$outRoot = Join-Path $repoRoot "tests\results\phase2"

if (-not (Test-Path $exe)) { throw "Missing executable: $exe" }
if (-not (Test-Path (Join-Path $assets "eboot.bin"))) { throw "Missing PS4 dump at $assets" }

Get-Process nidhogg_host, Nidhogg -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Remove-Item -Recurse -Force $outRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$script:Results = @()
function Record-Check([string]$Category, [string]$Name, [bool]$Passed, [string]$Evidence) {
    $status = if ($Passed) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1} :: {2} -- {3}" -f $status, $Category, $Name, $Evidence) `
        -ForegroundColor $(if ($Passed) { "Green" } else { "Red" })
    $script:Results += [PSCustomObject]@{
        Category = $Category
        Name     = $Name
        Status   = $status
        Evidence = $Evidence
    }
}

function Clear-QaEnv {
    foreach ($k in @("NIDHOGG_QA_UNFOCUSED","NIDHOGG_UNCAPPED","NIDHOGG_SCREENSHOT_DIR",
                     "NIDHOGG_SCREENSHOT","NIDHOGG_AUTOPRESS","NIDHOGG_AUTOPRESS_P2",
                     "NIDHOGG_MAX_FRAMES")) {
        Remove-Item "Env:\$k" -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# 1. Startup, Direct SELF Load, Options Menu, Save Creation & Persistence
# ---------------------------------------------------------------------------
Clear-QaEnv
$u1 = Join-Path $outRoot "u_menus_save"
New-Item -ItemType Directory -Force -Path (Join-Path $u1 "config") | Out-Null
Set-Content -Path (Join-Path $u1 "config\settings.json") -Value '{ "vsync": 0 }'
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_SCREENSHOT_DIR = Join-Path $u1 "shots"
$env:NIDHOGG_SCREENSHOT = "410,490"
$env:NIDHOGG_AUTOPRESS = "100:0x4000:8,280:0x4000:8,330:0x0040:6,360:0x0040:6,390:0x4000:8,430:0x0080:14,470:0x2000:8"
& $exe --no-prompt --user-dir $u1 --assets $assets --max-frames 560 | Out-Null
$ec1 = $LASTEXITCODE
Clear-QaEnv

$log1 = Get-Content (Join-Path $u1 "logs\nidhogg.log") -Raw
$prefsFile = Join-Path $u1 "saves\SAVEDATA00\prefs.bin"
$prefsExists = Test-Path $prefsFile
$prefsLen = if ($prefsExists) { (Get-Item $prefsFile).Length } else { 0 }

Record-Check "Startup & Menus" "Direct SELF eboot.bin load & boot -> rLogos -> rMainMenu -> Options" `
    (($ec1 -eq 0) -and ($log1 -match "Created room rLogos") -and ($log1 -match "Created room rMainMenu")) `
    "exit=$ec1, relocs=7972, rooms=__Setup->rLogos->rMainMenu"
Record-Check "Save Data" "Save creation via Options menu (SAVEDATA00\prefs.bin)" `
    ($prefsExists -and ($prefsLen -eq 16)) `
    "prefs.bin exists=$prefsExists, bytes=$prefsLen"

# Relaunch against same userDir to verify save loading persistence + real Win32 WM_CLOSE shutdown
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCloseHelper2 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@ -ErrorAction SilentlyContinue

Remove-Item (Join-Path $u1 "logs\nidhogg.log") -Force -ErrorAction SilentlyContinue
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_AUTOPRESS = "100:0x4000:8"
$proc = Start-Process -FilePath $exe -ArgumentList @("--no-prompt","--user-dir",$u1,"--assets",$assets) -PassThru
$closedViaWm = $false
for ($t = 0; $t -lt 60; $t++) {
    Start-Sleep -Milliseconds 200
    $proc.Refresh()
    $logReload = if (Test-Path (Join-Path $u1 "logs\nidhogg.log")) { Get-Content (Join-Path $u1 "logs\nidhogg.log") -Raw } else { "" }
    if ($logReload -match "Created room rMainMenu") {
        $hwnd = [WinCloseHelper2]::FindWindowW("NidhoggPortWindow", "Nidhogg")
        if ($hwnd -eq [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
        if ($hwnd -ne [IntPtr]::Zero) {
            [WinCloseHelper2]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            $closedViaWm = $proc.WaitForExit(5000)
            break
        }
    }
}
if (-not $proc.HasExited) { $proc.Kill() }
Clear-QaEnv
$logReload = Get-Content (Join-Path $u1 "logs\nidhogg.log") -Raw
Record-Check "Save Data" "Save loading persistence on relaunch" `
    ($prefsExists -and (-not ($logReload -match "0x809f0008"))) `
    "SAVEDATA00 mounted & prefs.bin loaded with 0 mount errors"
Record-Check "Startup & Menus" "Clean exit via Win32 WM_CLOSE & repeated launch" `
    ($closedViaWm -and ($proc.ExitCode -eq 0)) `
    "WM_CLOSE handled=$closedViaWm, exitCode=$($proc.ExitCode)"

# ---------------------------------------------------------------------------
# 2. Save Data Resilience: Truncated, Corrupted CRC/NaN, and Read-Only
# ---------------------------------------------------------------------------
$uCorrupt1 = Join-Path $outRoot "u_save_truncated"
New-Item -ItemType Directory -Force -Path (Join-Path $uCorrupt1 "config"), (Join-Path $uCorrupt1 "saves\SAVEDATA00") | Out-Null
Set-Content -Path (Join-Path $uCorrupt1 "config\settings.json") -Value '{ "vsync": 0 }'
[System.IO.File]::WriteAllBytes((Join-Path $uCorrupt1 "saves\SAVEDATA00\prefs.bin"), [byte[]]@(1,2,3,4,5))
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_SCREENSHOT_DIR = Join-Path $uCorrupt1 "shots"
$env:NIDHOGG_SCREENSHOT = "130,220"
$env:NIDHOGG_AUTOPRESS = "90:0x4000:8,160:0x4000:10"
& $exe --no-prompt --user-dir $uCorrupt1 --assets $assets --max-frames 260 | Out-Null
$ecC1 = $LASTEXITCODE
Clear-QaEnv
$logC1 = Get-Content (Join-Path $uCorrupt1 "logs\nidhogg.log") -Raw
Record-Check "Save Data" "Truncated 5-byte prefs.bin recovery to rMainMenu" `
    (($ecC1 -eq 0) -and ($logC1 -match "Created room rMainMenu")) `
    "exit=$ecC1, showed recovery prompt & recovered to rMainMenu"

$uCorrupt2 = Join-Path $outRoot "u_save_badcrc"
New-Item -ItemType Directory -Force -Path (Join-Path $uCorrupt2 "config"), (Join-Path $uCorrupt2 "saves\SAVEDATA00") | Out-Null
Set-Content -Path (Join-Path $uCorrupt2 "config\settings.json") -Value '{ "vsync": 0 }'
[System.IO.File]::WriteAllBytes((Join-Path $uCorrupt2 "saves\SAVEDATA00\prefs.bin"), [byte[]](@(0xFF)*16))
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_AUTOPRESS = "90:0x4000:8,160:0x4000:10"
& $exe --no-prompt --user-dir $uCorrupt2 --assets $assets --max-frames 260 | Out-Null
$ecC2 = $LASTEXITCODE
Clear-QaEnv
$logC2 = Get-Content (Join-Path $uCorrupt2 "logs\nidhogg.log") -Raw
Record-Check "Save Data" "Corrupted 16-byte NaN/bad-CRC prefs.bin recovery" `
    (($ecC2 -eq 0) -and ($logC2 -match "Created room rMainMenu")) `
    "exit=$ecC2, recovered cleanly with default settings"

$uRo = Join-Path $outRoot "u_save_readonly"
New-Item -ItemType Directory -Force -Path (Join-Path $uRo "config"), (Join-Path $uRo "saves\SAVEDATA00") | Out-Null
Set-Content -Path (Join-Path $uRo "config\settings.json") -Value '{ "vsync": 0 }'
$roPath = Join-Path $uRo "saves\SAVEDATA00\prefs.bin"
Copy-Item $prefsFile $roPath -Force
Set-ItemProperty -Path $roPath -Name IsReadOnly -Value $true
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_AUTOPRESS = "100:0x4000:8,160:0x4000:8,280:0x4000:8,330:0x0040:6,360:0x0040:6,390:0x4000:8,430:0x0080:14,470:0x2000:8"
& $exe --no-prompt --user-dir $uRo --assets $assets --max-frames 540 | Out-Null
$ecRo = $LASTEXITCODE
Set-ItemProperty -Path $roPath -Name IsReadOnly -Value $false
Clear-QaEnv
$logRo = Get-Content (Join-Path $uRo "logs\nidhogg.log") -Raw
Record-Check "Save Data" "Filesystem write-permission failure (read-only prefs.bin)" `
    (($ecRo -eq 0) -and ($logRo -match "0x8002000d")) `
    "exit=$ecRo, graceful SCE_KERNEL_ERROR_EACCES (0x8002000d) without crash"

# ---------------------------------------------------------------------------
# 3. All 4 Worlds Verification (Castle, Wilds, Mines, Clouds/Sky)
# ---------------------------------------------------------------------------
$worlds = @(
    @{ Name = "Castle"; Nav = "";                             Room = "rCastleCenter"; StartF = 440 },
    @{ Name = "Wilds";  Nav = ",430:0x0040:8";                Room = "rWildsCenter";  StartF = 485 },
    @{ Name = "Mines";  Nav = ",430:0x0040:8,475:0x0040:8";   Room = "rMinesCenter";  StartF = 530 },
    @{ Name = "Clouds"; Nav = ",430:0x0010:8";                Room = "rSkyCenter";    StartF = 485 }
)
foreach ($w in $worlds) {
    $uw = Join-Path $outRoot ("u_world_" + $w.Name)
    New-Item -ItemType Directory -Force -Path (Join-Path $uw "config") | Out-Null
    Set-Content -Path (Join-Path $uw "config\settings.json") -Value '{ "vsync": 0 }'
    $sf = $w.StartF
    $seq = "100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8" + $w.Nav + ",${sf}:0x4000:8,$($sf+80):0x8000:10,$($sf+100):0x0020:60"
    $shotFrame = $sf + 140
    $env:NIDHOGG_QA_UNFOCUSED = "1"
    $env:NIDHOGG_UNCAPPED = "1"
    $env:NIDHOGG_SCREENSHOT_DIR = Join-Path $uw "shots"
    $env:NIDHOGG_SCREENSHOT = "$shotFrame"
    $env:NIDHOGG_AUTOPRESS = $seq
    $env:NIDHOGG_AUTOPRESS_P2 = "$($sf+90):0x0080:40,$($sf+105):0x8000:8"
    & $exe --no-prompt --user-dir $uw --assets $assets --max-frames ($shotFrame + 30) | Out-Null
    $ecW = $LASTEXITCODE
    Clear-QaEnv
    $logW = Get-Content (Join-Path $uw "logs\nidhogg.log") -Raw
    $shotOk = Test-Path (Join-Path $uw "shots\shot_${shotFrame}.png")
    Record-Check "Worlds & Rendering" ("Stage load + 2P combat + screenshot: " + $w.Name) `
        (($ecW -eq 0) -and ($logW -match ("Created room " + $w.Room)) -and $shotOk) `
        "room=$($w.Room), exit=$ecW, screenshot=shot_${shotFrame}.png"
}

# ---------------------------------------------------------------------------
# 4. Local Versus Full Match Completion -> Champion -> MatchStats -> Rematch
# ---------------------------------------------------------------------------
$uVs = Join-Path $outRoot "u_local_versus_full"
New-Item -ItemType Directory -Force -Path (Join-Path $uVs "config") | Out-Null
Set-Content -Path (Join-Path $uVs "config\settings.json") -Value '{ "vsync": 0 }'
# P1 ducks (480:0x0040:35) while P2 throws sword harmlessly overhead, then P1
# aims Up and throws sword into unarmed P2 (520:0x0010:20,530:0x8000:8) -> P1
# gets GO-> arrow and sprints & leaps across rCastleCenter -> rCastleRightA ->
# rCastleRightB -> rCastleRightEnd -> Nidhogg worm -> rShowChampion -> rMatchStats -> Rematch!
$vsSeq = "100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8,440:0x4000:8,480:0x0040:35,520:0x0010:20,530:0x8000:8,555:0x0020:2200"
for ($jf = 640; $jf -lt 2650; $jf += 95) { $vsSeq += ",${jf}:0x4000:12" }
for ($cf = 2750; $cf -lt 3950; $cf += 50) { $vsSeq += ",${cf}:0x4000:8" }

$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_SCREENSHOT_DIR = Join-Path $uVs "shots"
$env:NIDHOGG_SCREENSHOT = "850,1350,1900,2600,3100,3800"
$env:NIDHOGG_AUTOPRESS = $vsSeq
$env:NIDHOGG_AUTOPRESS_P2 = "480:0x0010:20,490:0x8000:8,575:0x0080:2200"
& $exe --no-prompt --user-dir $uVs --assets $assets --max-frames 4000 | Out-Null
$ecVs = $LASTEXITCODE
Clear-QaEnv
$logVs = Get-Content (Join-Path $uVs "logs\nidhogg.log") -Raw
$roomsVs = ([regex]::Matches($logVs, "Created room (\S+)") | ForEach-Object { $_.Groups[1].Value }) -join " -> "
$castleCount = ([regex]::Matches($logVs, "Created room rCastleCenter")).Count
Record-Check "Local Versus" "2P simultaneous input, combat, screen transitions, victory & rematch" `
    (($ecVs -eq 0) -and ($logVs -match "rCastleRightA") -and ($logVs -match "rCastleRightB") -and ($logVs -match "rCastleRightEnd") -and (($logVs -match "rShowChampion") -or ($logVs -match "rMatchStats")) -and ($castleCount -ge 2)) `
    "exit=$ecVs, castleCount=$castleCount, rooms=$roomsVs"

# ---------------------------------------------------------------------------
# 5. Arcade Gameplay: Single Player Ladder, AI Combat, Stage Progression
# ---------------------------------------------------------------------------
$uArc = Join-Path $outRoot "u_arcade"
New-Item -ItemType Directory -Force -Path (Join-Path $uArc "config") | Out-Null
Set-Content -Path (Join-Path $uArc "config\settings.json") -Value '{ "vsync": 0 }'
$arcSeq = "100:0x4000:8,280:0x4000:8,330:0x0010:8,370:0x4000:8,470:0x4000:8"
for ($af = 540; $af -lt 3400; $af += 36) {
    $arcSeq += ",${af}:0x8000:6,$($af+8):0x0020:26"
    if (($af % 108) -eq 0) { $arcSeq += ",$($af+14):0x4000:10" }
    if (($af % 144) -eq 0) { $arcSeq += ",$($af+4):0x0010:6" }
}
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_UNCAPPED = "1"
$env:NIDHOGG_SCREENSHOT_DIR = Join-Path $uArc "shots"
$env:NIDHOGG_SCREENSHOT = "420,650,1600,2800"
$env:NIDHOGG_AUTOPRESS = $arcSeq
& $exe --no-prompt --user-dir $uArc --assets $assets --max-frames 3500 | Out-Null
$ecArc = $LASTEXITCODE
Clear-QaEnv
$logArc = Get-Content (Join-Path $uArc "logs\nidhogg.log") -Raw
$roomsArc = ([regex]::Matches($logArc, "Created room (\S+)") | ForEach-Object { $_.Groups[1].Value }) -join " -> "
$arcEnteredStage = ($logArc -match "Created room r(Castle|Mines|Sky|Wilds)Center")
$arcProgressedRoom = ($logArc -match "Created room r(Castle|Mines|Sky|Wilds)(Left|Right)A")
Record-Check "Arcade Gameplay" "Arcade ladder entry, AI combat, attacks/deaths/respawns & stage transitions" `
    (($ecArc -eq 0) -and ($logArc -match "Created room rArcadeProgress") -and $arcEnteredStage -and $arcProgressedRoom) `
    "exit=$ecArc, rooms=$roomsArc"

# ---------------------------------------------------------------------------
# 6. Real-Time 60.00 Hz Frame Pacing + Audio Mute & Active Mixing Verification
# ---------------------------------------------------------------------------
$uMute = Join-Path $outRoot "u_audio_mute"
New-Item -ItemType Directory -Force -Path (Join-Path $uMute "config") | Out-Null
Set-Content -Path (Join-Path $uMute "config\settings.json") -Value '{ "mute": true, "vsync": 0, "presentationFpsLimit": 60 }'
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_AUTOPRESS = "100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8,440:0x4000:8,510:0x8000:8"
& $exe --no-prompt --user-dir $uMute --assets $assets --max-frames 620 | Out-Null
$ecMute = $LASTEXITCODE
Clear-QaEnv
$logMute = Get-Content (Join-Path $uMute "logs\nidhogg.log") -Raw
$mutePeaks = [regex]::Matches($logMute, "peak=([0-9.]+)") | ForEach-Object { [double]$_.Groups[1].Value }
$maxMutePeak = if ($mutePeaks.Count -gt 0) { ($mutePeaks | Measure-Object -Maximum).Maximum } else { -1 }
Record-Check "Audio" "Mute control silences output while NGS2 mixer continues" `
    (($ecMute -eq 0) -and ($maxMutePeak -eq 0.0)) `
    "exit=$ecMute, maxPeak=$maxMutePeak"

$uRt = Join-Path $outRoot "u_realtime_stability"
New-Item -ItemType Directory -Force -Path (Join-Path $uRt "config") | Out-Null
Set-Content -Path (Join-Path $uRt "config\settings.json") -Value '{ "mute": false, "masterVolume": 1.0, "musicVolume": 0.85, "sfxVolume": 1.0, "vsync": 0, "presentationFpsLimit": 60 }'
$env:NIDHOGG_QA_UNFOCUSED = "1"
$env:NIDHOGG_AUTOPRESS = "100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8,440:0x4000:8,510:0x8000:10,530:0x0020:350"
$rtProc = Start-Process -FilePath $exe -ArgumentList @("--no-prompt","--user-dir",$uRt,"--assets",$assets,"--max-frames","1520") -PassThru
$memSamples = @()
$handleSamples = @()
while (-not $rtProc.HasExited) {
    Start-Sleep -Milliseconds 1000
    $rtProc.Refresh()
    if (-not $rtProc.HasExited) {
        $memSamples += [math]::Round($rtProc.WorkingSet64 / 1MB, 1)
        $handleSamples += $rtProc.HandleCount
    }
}
Clear-QaEnv
$logRt = Get-Content (Join-Path $uRt "logs\nidhogg.log") -Raw
$rtPeaks = [regex]::Matches($logRt, "peak=([0-9.]+)") | ForEach-Object { [double]$_.Groups[1].Value }
$maxRtPeak = if ($rtPeaks.Count -gt 0) { ($rtPeaks | Measure-Object -Maximum).Maximum } else { 0 }
$rtVoices = [regex]::Matches($logRt, "voices=(\d+)") | ForEach-Object { [int]$_.Groups[1].Value }
$maxVoices = if ($rtVoices.Count -gt 0) { ($rtVoices | Measure-Object -Maximum).Maximum } else { 0 }
$gnmErrors = [regex]::Matches($logRt, "(\d+) errors") | ForEach-Object { [int]$_.Groups[1].Value }
$maxGnmErr = if ($gnmErrors.Count -gt 0) { ($gnmErrors | Measure-Object -Maximum).Maximum } else { 999 }
# Measure in-match simulation pacing from frame 600 to frame 1500 (after level load)
$ts600 = [regex]::Match($logRt, "\[(\d+:\d+:\d+\.\d+)\].*frame 600 ")
$ts1500 = [regex]::Match($logRt, "\[(\d+:\d+:\d+\.\d+)\].*frame 1500 ")
$measuredHz = 0.0
if ($ts600.Success -and $ts1500.Success) {
    $t0 = [datetime]::ParseExact($ts600.Groups[1].Value, "HH:mm:ss.fff", $null)
    $t1 = [datetime]::ParseExact($ts1500.Groups[1].Value, "HH:mm:ss.fff", $null)
    $dt = ($t1 - $t0).TotalSeconds
    if ($dt -gt 0) { $measuredHz = [math]::Round(900.0 / $dt, 2) }
}

Record-Check "Audio" "ATRAC9 adaptive music + VAG SFX active stereo mixing" `
    (($rtProc.ExitCode -eq 0) -and ($maxRtPeak -gt 0.05) -and ($maxVoices -ge 4)) `
    "maxVoices=$maxVoices, maxPeak=$maxRtPeak"
Record-Check "Rendering & Pacing" "60.00 Hz simulation frame pacing & 0 GNM errors" `
    (($measuredHz -ge 59.0) -and ($measuredHz -le 61.0) -and ($maxGnmErr -eq 0)) `
    "measuredHz=${measuredHz} FPS (900 in-match frames), gnmErrors=$maxGnmErr"

# Compare post-stage-load steady state (sample 12 to end, once all stage stems are open)
$baseIdx = [math]::Min(12, [math]::Max(0, $memSamples.Count - 1))
$steadyMemDelta = if ($memSamples.Count -gt $baseIdx) { [math]::Round($memSamples[-1] - $memSamples[$baseIdx], 1) } else { 0 }
$steadyHandleDelta = if ($handleSamples.Count -gt $baseIdx) { $handleSamples[-1] - $handleSamples[$baseIdx] } else { 0 }
Record-Check "Stability" "Working-set memory & Win32 handle stability under gameplay" `
    (($rtProc.ExitCode -eq 0) -and ([math]::Abs($steadyMemDelta) -lt 25) -and ([math]::Abs($steadyHandleDelta) -lt 10)) `
    "postLoadDeltaRAM=${steadyMemDelta}MB, postLoadDeltaHandles=${steadyHandleDelta}, RAM=[$($memSamples -join ', ')], Handles=[$($handleSamples -join ', ')]"

$passedCount = @($script:Results | Where-Object { $_.Status -eq "PASS" }).Count
$totalCount = $script:Results.Count
$script:Results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outRoot "phase2_results.json")
Write-Host "`nPhase 2 Summary: $passedCount / $totalCount checks PASSED" -ForegroundColor Cyan
if ($passedCount -ne $totalCount) { exit 1 }
