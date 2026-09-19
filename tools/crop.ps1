# crop.ps1 - crop a region out of a PNG and scale it up with nearest-neighbour.
param(
  [Parameter(Mandatory=$true)][string]$In,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$X=0, [int]$Y=0, [int]$W=200, [int]$H=200, [int]$Scale=4
)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In))
$rect = New-Object System.Drawing.Rectangle $X,$Y,$W,$H
$piece = $src.Clone($rect, $src.PixelFormat)
$dst = New-Object System.Drawing.Bitmap ($W*$Scale), ($H*$Scale)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($piece, 0, 0, ($W*$Scale), ($H*$Scale))
$g.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$dst.Dispose(); $piece.Dispose(); $src.Dispose()
Write-Output ("OK {0} {1}x{2}" -f $Out, ($W*$Scale), ($H*$Scale))
