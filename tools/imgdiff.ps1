# imgdiff.ps1 — 캡처 두 장의 **다른 픽셀 수·무게중심·경계상자**를 낸다.
#
# 왜 (2026-09-11) — 자세 비교를 눈으로 하다 두 번 헛다리를 짚었다. 화면에는 횃불·물결처럼
#   **가만히 있어도 변하는 것**이 있어서, 「다르다」만으로는 아무것도 못 가른다.
#   그래서 늘 **대조군(같은 조건 두 장)과 짝지어** 읽는다 — 실험 차이가 잡음 바닥보다
#   뚜렷이 커야 신호다.
#
#   .\tools\imgdiff.ps1 -A p0_stand.png -B p2_stand.png
param(
  [Parameter(Mandatory=$true)][string]$A,
  [Parameter(Mandatory=$true)][string]$B,
  [string]$Dir = '',
  [int]$Threshold = 24,     # 채널 합 차이가 이보다 커야 「다름」
  [int]$CropX = -1, [int]$CropY = -1, [int]$CropW = 0, [int]$CropH = 0
)
Add-Type -AssemblyName System.Drawing
if ($Dir -eq '') { $Dir = Join-Path (Split-Path -Parent $PSScriptRoot) '_evidence\desk1' }
$pa = if (Test-Path $A) { $A } else { Join-Path $Dir $A }
$pb = if (Test-Path $B) { $B } else { Join-Path $Dir $B }
$ia = [Drawing.Bitmap]::FromFile($pa)
$ib = [Drawing.Bitmap]::FromFile($pb)
if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) {
  Write-Host '[멈춤] 크기가 다르다' -ForegroundColor Red; exit 2
}
$r = New-Object Drawing.Rectangle(0,0,$ia.Width,$ia.Height)
$la = $ia.LockBits($r,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
$lb = $ib.LockBits($r,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
$n = $la.Stride * $ia.Height
$ba = New-Object byte[] $n; $bb = New-Object byte[] $n
[Runtime.InteropServices.Marshal]::Copy($la.Scan0,$ba,0,$n)
[Runtime.InteropServices.Marshal]::Copy($lb.Scan0,$bb,0,$n)
$stride = $la.Stride
$ia.UnlockBits($la); $ib.UnlockBits($lb)

$xa = 0; $ya = 0; $xz = $ia.Width; $yz = $ia.Height
if ($CropW -gt 0) { $xa = $CropX; $ya = $CropY; $xz = $CropX + $CropW; $yz = $CropY + $CropH }
$cnt=0; $sx=0.0; $sy=0.0; $x0=999999;$y0=999999;$x1=-1;$y1=-1
for ($y=$ya;$y -lt $yz;$y++) {
  $row=$y*$stride
  for ($x=$xa;$x -lt $xz;$x++) {
    $i=$row+$x*4
    $d=[Math]::Abs($ba[$i]-$bb[$i])+[Math]::Abs($ba[$i+1]-$bb[$i+1])+[Math]::Abs($ba[$i+2]-$bb[$i+2])
    if ($d -gt $Threshold) { $cnt++; $sx+=$x; $sy+=$y
      if($x -lt $x0){$x0=$x}; if($x -gt $x1){$x1=$x}; if($y -lt $y0){$y0=$y}; if($y -gt $y1){$y1=$y} }
  }
}
$tot = ($xz-$xa)*($yz-$ya)
Write-Host ("{0} vs {1}" -f (Split-Path -Leaf $pa), (Split-Path -Leaf $pb))
Write-Host ("  다른 픽셀 {0} / {1} ({2:N3}%)" -f $cnt,$tot,(100.0*$cnt/$tot))
if ($cnt -gt 0) {
  Write-Host ("  무게중심 ({0:N0},{1:N0}) · 상자 x {2}~{3} · y {4}~{5}" -f ($sx/$cnt),($sy/$cnt),$x0,$x1,$y0,$y1)
}
$ia.Dispose(); $ib.Dispose()
exit 0
