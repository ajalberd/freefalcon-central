# Tile a folder of PNG shots into one labelled contact sheet, for reviewing a sweep at a glance.
#
#   tools\uitest\montage.ps1 -Dir <png dir> -Out sheet.png [-Cols 3] [-CellW 640] [-Names a,b,c]

param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Cols = 3,
    [int]$CellW = 640,
    [string[]]$Names = @()
)

Add-Type -AssemblyName System.Drawing
$files = if ($Names.Count) { $Names | ForEach-Object { Join-Path $Dir "$_.png" } | Where-Object { Test-Path $_ } }
         else { (Get-ChildItem $Dir -Filter *.png | Sort-Object Name).FullName }
if (-not $files) { throw "no PNGs in $Dir" }

$first = [System.Drawing.Image]::FromFile(@($files)[0])
$cellH = [int]($CellW * $first.Height / $first.Width)
$first.Dispose()
$label = 22
$rows = [math]::Ceiling(@($files).Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($Cols * $CellW), ($rows * ($cellH + $label))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::FromArgb(40, 40, 40))
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
$font = New-Object System.Drawing.Font "Consolas", 12
$i = 0
foreach ($f in $files) {
    $img = [System.Drawing.Image]::FromFile($f)
    $x = ($i % $Cols) * $CellW
    $y = [math]::Floor($i / $Cols) * ($cellH + $label)
    $g.DrawString([IO.Path]::GetFileNameWithoutExtension($f), $font, [System.Drawing.Brushes]::White, $x + 4, $y + 2)
    $g.DrawImage($img, $x, $y + $label, $CellW, $cellH)
    $img.Dispose()
    $i++
}
$g.Dispose()
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
Write-Output "$i shots -> $Out"
