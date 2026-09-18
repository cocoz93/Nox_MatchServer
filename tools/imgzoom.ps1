# imgzoom.ps1 - crop a region of a screenshot and scale it up, for judging small sprites.
#   .\tools\imgzoom.ps1 -In shot.png -Out zoom.png -X 900 -Y 450 -W 400 -H 300 -Scale 3
param(
  [Parameter(Mandatory=$true)][string]$In,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$X = 0, [int]$Y = 0, [int]$W = 400, [int]$H = 300,
  [int]$Scale = 3
)
Add-Type -AssemblyName System.Drawing
$src = [Drawing.Bitmap]::FromFile((Resolve-Path $In))
if ($X + $W -gt $src.Width)  { $W = $src.Width  - $X }
if ($Y + $H -gt $src.Height) { $H = $src.Height - $Y }
$crop = $src.Clone((New-Object Drawing.Rectangle($X, $Y, $W, $H)), $src.PixelFormat)
$big = New-Object Drawing.Bitmap(($W * $Scale), ($H * $Scale))
$g = [Drawing.Graphics]::FromImage($big)
# nearest neighbour - keep pixel edges crisp so sprite shapes stay readable
$g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($crop, 0, 0, ($W * $Scale), ($H * $Scale))
$g.Dispose()
$big.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$big.Dispose(); $crop.Dispose(); $src.Dispose()
Write-Output ("zoom {0}x{1} @({2},{3}) x{4} -> {5}" -f $W, $H, $X, $Y, $Scale, $Out)
