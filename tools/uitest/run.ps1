# Run one ui95 test script against the installed game and collect what it saw.
#
#   tools\uitest\run.ps1 -Script smoke.txt [-UiSize 1365x768] [-Renderer vulkan] [-Deploy] [-Png <dir>]
#
# -Deploy copies this worktree's Falcon4___x64_Release\FFViper.exe to <Install>\FFViper-ui.exe first,
# so the build the install normally runs (FFViper.exe) is never touched. Scripts live in
# <Install>\uitest\ (this folder's *.txt are copied there on every run). Results land in
# <Install>\uitest\out\: result.log, *.json layout dumps, and *.bmp, converted to PNG in -Png.
# The exit code is 0 only when the log ends in DONE with no FAIL lines. See UI-OVERHAUL.md.

param(
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$UiSize = "",
    [string]$UiScale = "",
    [string]$UiFilter = "",
    [string]$Renderer = "",
    [switch]$Deploy,
    [string]$Install = "C:\FreeFalcon6",
    [string]$Png = "",
    [int]$TimeoutSec = 240,
    [string]$ExeName = "FFViper-ui.exe" # another name when Andrew has FFViper-ui.exe open
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$exe = Join-Path $Install $ExeName
$testDir = Join-Path $Install "uitest"
$out = Join-Path $testDir "out"

if ($Deploy) { Copy-Item (Join-Path $repo "Falcon4___x64_Release\FFViper.exe") $exe -Force }
New-Item -ItemType Directory -Force $testDir | Out-Null
Copy-Item (Join-Path $PSScriptRoot "*.txt") $testDir -Force

# The code's own names for windows and controls (the userids.h enum), so scripts and logs can say
# CP_MAIN_CTRL. Values under 1000 are sound groups and list orders, not windows or controls.
$ids = Select-String -Path (Join-Path $repo "src\ui\include\userids.h") -Pattern '^\s*([A-Z][A-Z0-9_]*)\s*=\s*(\d+)\s*,' |
    Where-Object { [int64]$_.Matches[0].Groups[2].Value -ge 1000 } |
    ForEach-Object { "$($_.Matches[0].Groups[1].Value) $($_.Matches[0].Groups[2].Value)" }
Set-Content -Path (Join-Path $testDir "ids.txt") -Value $ids -Encoding ascii

# Only this exe matters: a copy Andrew is playing (FFViper.exe, or FFViper-ui.exe with -ExeName set
# to something else) can stay open; two scripted runs at once would share uitest\out.
if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($ExeName)) -ErrorAction SilentlyContinue) { throw "$ExeName is already running" }

# Old results would read as this run's.
if (Test-Path $out) { Get-ChildItem $out -File | Remove-Item -Force }

# FFDebug.log only grows (and rolls over at 4 MB); remember where this run starts.
$ffLog = Join-Path $Install "FFDebug.log"
$ffStart = if (Test-Path $ffLog) { (Get-Item $ffLog).Length } else { 0 }

$argList = "-nomovie -window -uitest $Script"
if ($UiSize) { $argList += " -uisize $UiSize" }
if ($UiScale) { $argList += " -uiscale $UiScale" }
if ($UiFilter -ne "") { $argList += " -uifilter $UiFilter" }
if ($Renderer) { $argList += " -renderer $Renderer" } # dx12 | vulkan, this run only
$p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $Install -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Stop-Process -Id $p.Id -Force
    Write-Output "KILLED after $TimeoutSec s (the in-game watchdog did not fire)"
}

$log = Join-Path $out "result.log"
if (-not (Test-Path $log)) { Write-Output "NO RESULT LOG -- the game died before the harness started; see $Install\FFCrash.log"; exit 2 }
Get-Content $log

# This run's slice of FFDebug.log, for the [UIADAPT] decisions and anything else the UI logs.
if (Test-Path $ffLog) {
    $len = (Get-Item $ffLog).Length
    if ($len -lt $ffStart) { $ffStart = 0 } # rolled over
    $fs = [System.IO.File]::Open($ffLog, 'Open', 'Read', 'ReadWrite')
    $fs.Seek($ffStart, 'Begin') | Out-Null
    $slice = (New-Object System.IO.StreamReader($fs)).ReadToEnd()
    $fs.Close()
    Set-Content -Path (Join-Path $out "ffdebug.txt") -Value $slice
    $adapt = @($slice -split "`n" | Where-Object { $_ -match '\[UIADAPT\]' })
    if ($adapt.Count) { Write-Output "$($adapt.Count) [UIADAPT] lines in out\ffdebug.txt" }
}

if ($Png) {
    Add-Type -AssemblyName System.Drawing
    New-Item -ItemType Directory -Force $Png | Out-Null
    # Keep this run's dumps and logs beside its PNGs; out\ is wiped by the next run.
    $tag = [IO.Path]::GetFileNameWithoutExtension($Script)
    Copy-Item (Join-Path $out "*.json") $Png -Force -ErrorAction SilentlyContinue
    Copy-Item $log (Join-Path $Png "$tag.result.log") -Force
    if (Test-Path (Join-Path $out "ffdebug.txt")) { Copy-Item (Join-Path $out "ffdebug.txt") (Join-Path $Png "$tag.ffdebug.txt") -Force }
    foreach ($b in Get-ChildItem $out -Filter *.bmp) {
        $img = [System.Drawing.Bitmap]::FromFile($b.FullName)
        $img.Save((Join-Path $Png ($b.BaseName + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
    }
}

$text = Get-Content $log -Raw
if ($text -match "(?m)^\s*[\d.]+ DONE" -and $text -notmatch "(?m)^\s*[\d.]+ FAIL") { exit 0 }
exit 1
