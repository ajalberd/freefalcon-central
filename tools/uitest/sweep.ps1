# Run every screen script at one size and tile the results into one contact sheet.
#
#   tools\uitest\sweep.ps1 -Out <dir> [-UiSize 3200x1300] [-UiScale 1.35] [-Deploy] [-Scripts a,b]
#
# Prints one line per script (exit code and any FAIL/TIMEOUT), then any [UIADAPT] "wanted ... has"
# mismatches, then writes <dir>\sheet.png. See run.ps1 and UI-OVERHAUL.md.

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$UiSize = "",
    [string]$UiScale = "",
    [switch]$Deploy,
    [string[]]$Scripts = @("campaign", "recon", "te", "te_plan", "te_brief", "te_munitions",
                           "te_new", "te_builder", "campaign_screens", "campaign_intel",
                           "screen_logbook", "screen_tacref", "screen_acmi", "screen_setup", "screen_comms",
                           "screen_theater", "screen_ia", "screen_dogfight")
)

$run = Join-Path $PSScriptRoot "run.ps1"
$first = $true

foreach ($s in $Scripts) {
    $a = @{ Script = "$s.txt"; Png = $Out; TimeoutSec = 260 }
    if ($UiSize) { $a.UiSize = $UiSize }
    if ($UiScale) { $a.UiScale = $UiScale }
    if ($Deploy -and $first) { $a.Deploy = $true }
    $first = $false
    $r = & $run @a
    "{0,-16} exit {1} {2}" -f $s, $LASTEXITCODE, (($r | Select-String "FAIL|TIMEOUT|KILLED|NO RESULT") -join " | ")
}

Get-ChildItem (Join-Path $Out "*.ffdebug.txt") -ErrorAction SilentlyContinue | Select-String "wanted" | ForEach-Object { $_.Line.Trim() }

# The on-screen captures where a script took one, else the surface shot.
$names = @("main", "select", "strat", "cpmap_window", "recon_window", "te_play_window", "plan_window", "brief_window",
           "munitions_window", "logbook", "tacref", "acmi", "setup", "comms", "theater", "ia", "dogfight")
& (Join-Path $PSScriptRoot "montage.ps1") -Dir $Out -Out (Join-Path $Out "sheet.png") -Cols 4 -CellW 480 -Names $names
