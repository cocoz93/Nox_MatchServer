# sheet.ps1 — 프레임들에서 같은 자리를 잘라 가로로 늘어놓는다(라벨 포함).
param(
  [Parameter(Mandatory=$true)][string]$Dir,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$X=468, [int]$Y=330, [int]$W=100, [int]$H=130, [int]$Scale=3,
  [string]$Title = ''
)
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem $Dir -Filter 'f_*.png' | Sort-Object Name
if ($files.Count -eq 0) { throw "프레임 없음: $Dir" }
$cw = $W*$Scale; $ch = $H*$Scale
$pad = 6; $lab = 20; $top = 24
$sheetW = $files.Count*($cw+$pad)+$pad
$sheetH = $ch+$lab+$pad*2+$top
$dst = New-Object System.Drawing.Bitmap $sheetW, $sheetH
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.Clear([System.Drawing.Color]::FromArgb(24,24,24))
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$font = New-Object System.Drawing.Font('Consolas', 11)
$fontT = New-Object System.Drawing.Font('Consolas', 13, [System.Drawing.FontStyle]::Bold)
$br = [System.Drawing.Brushes]::White
if ($Title -ne '') { $g.DrawString($Title, $fontT, $br, 4, 3) }
$i = 0
foreach ($f in $files) {
  $src = [System.Drawing.Bitmap]::FromFile($f.FullName)
  $rx = [Math]::Min($X, $src.Width-$W); $ry = [Math]::Min($Y, $src.Height-$H)
  $rect = New-Object System.Drawing.Rectangle $rx,$ry,$W,$H
  $piece = $src.Clone($rect, $src.PixelFormat)
  $px = $pad+$i*($cw+$pad)
  $g.DrawImage($piece, $px, $top+$pad, $cw, $ch)
  $g.DrawString($f.BaseName, $font, $br, $px, $top+$pad+$ch+2)
  $piece.Dispose(); $src.Dispose()
  $i++
}
$g.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$dst.Dispose()
Write-Output ("OK {0} ({1}x{2}, {3}장)" -f $Out, $sheetW, $sheetH, $files.Count)