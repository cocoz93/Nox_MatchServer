# findblob.ps1 — 찍은 화면에서 색 덩어리를 찾아 화면 좌표로 돌려준다.
#
# 왜 필요한가 — 월드 좌표로 걸어가려면 화면축↔월드축 사상이 필요한데,
# 아이소 사영이라 각이 구간마다 달라지고(최대 49도) 벽을 만나면 두 자리를 오간다.
# **눈에 보이는 것을 그대로 누르면** 그 사상이 통째로 필요 없다.
#
# 불(주황·노랑)이 첫 쓸모다. 죽음 표본을 잡으려면 불로 걸어 들어가야 한다.
#
# 쓰는 법
#   .\tools\findblob.ps1 -Png <파일> -Kind fire
#   .\tools\findblob.ps1 -Png <파일> -MinR 200 -MaxB 100 -MinG 90 -MaxG 230
#
# 내놓는 것 — 덩어리마다 한 줄: 화소수 · 화면중심 x,y · 창 안 픽셀 x,y
#   화면중심 좌표는 nav.ps1 에 그대로 넣을 수 있는 1280x960 기준 값이다.
param(
  [Parameter(Mandatory)][string]$Png,
  [string]$Kind = 'fire',
  [int]$MinR = 210, [int]$MaxB = 110, [int]$MinG = 80, [int]$MaxG = 235,
  [int]$MinPix = 60,          # 이보다 작은 덩어리는 잡음으로 버린다
  [int]$Border = 8,           # 창 테두리 (Shot 은 GetWindowRect 로 찍는다)
  [int]$Title = 31,           # 제목줄
  [int]$Grid = 24             # 이 칸 크기로 묶어 덩어리를 센다
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ($Kind -eq 'fire') { }   # 기본값이 곧 불이다. 다른 종류를 넣을 자리.

$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Png))
$w = $bmp.Width; $h = $bmp.Height
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$stride = $bd.Stride
$buf = New-Object byte[] ($stride * $h)
[System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $buf.Length)
$bmp.UnlockBits($bd)
$bmp.Dispose()

# 칸별로 맞는 화소를 센다 (연결성분 대신 격자 묶음 — 불은 뭉쳐 있어 이걸로 충분하다)
$gw = [int][math]::Ceiling($w / $Grid); $gh = [int][math]::Ceiling($h / $Grid)
$cnt = New-Object 'int[]' ($gw * $gh)
$sx = New-Object 'long[]' ($gw * $gh)
$sy = New-Object 'long[]' ($gw * $gh)
$hit = 0
for ($y = 0; $y -lt $h; $y++) {
  $row = $y * $stride
  for ($x = 0; $x -lt $w; $x++) {
    $o = $row + $x * 4
    $b = $buf[$o]; $g = $buf[$o + 1]; $r = $buf[$o + 2]
    if ($r -ge $MinR -and $b -le $MaxB -and $g -ge $MinG -and $g -le $MaxG) {
      $i = [int]($y / $Grid) * $gw + [int]($x / $Grid)
      $cnt[$i]++; $sx[$i] += $x; $sy[$i] += $y; $hit++
    }
  }
}

# 이웃한 칸을 이어 붙여 덩어리로 만든다 (4방향 넓히기)
$seen = New-Object 'bool[]' ($gw * $gh)
$blobs = @()
for ($i = 0; $i -lt $cnt.Length; $i++) {
  if ($seen[$i] -or $cnt[$i] -eq 0) { continue }
  $stack = New-Object System.Collections.Generic.Stack[int]
  $stack.Push($i); $seen[$i] = $true
  $n = 0; $tx = [long]0; $ty = [long]0
  while ($stack.Count -gt 0) {
    $c = $stack.Pop()
    $n += $cnt[$c]; $tx += $sx[$c]; $ty += $sy[$c]
    $cy = [int]($c / $gw); $cx = $c % $gw
    foreach ($d in @(@(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
      $nx = $cx + $d[0]; $ny = $cy + $d[1]
      if ($nx -lt 0 -or $ny -lt 0 -or $nx -ge $gw -or $ny -ge $gh) { continue }
      $j = $ny * $gw + $nx
      if (-not $seen[$j] -and $cnt[$j] -gt 0) { $seen[$j] = $true; $stack.Push($j) }
    }
  }
  if ($n -ge $MinPix) {
    $blobs += [pscustomobject]@{ N = $n; Px = [int]($tx / $n); Py = [int]($ty / $n) }
  }
}

$cw = $w - 2 * $Border; $ch = $h - $Title - $Border
"사진 ${w}x${h}  ·  맞는 화소 $hit  ·  덩어리 $($blobs.Count)개  (클라이언트 ${cw}x${ch})"
if ($blobs.Count -eq 0) { "덩어리 없음 — 문턱을 낮춰 볼 것"; return }
"{0,8} {1,7} {2,7} {3,9} {4,9} {5,9}" -f '화소수', '창x', '창y', '게임x', '게임y', '중심거리'
foreach ($b in ($blobs | Sort-Object N -Descending)) {
  # 창 픽셀 → 클라이언트 → 1280x960 게임 좌표
  $clx = $b.Px - $Border; $cly = $b.Py - $Title
  $gx = [int]($clx * 1280.0 / $cw); $gy = [int]($cly * 960.0 / $ch)
  $dist = [int][math]::Sqrt([math]::Pow($gx - 640, 2) + [math]::Pow($gy - 480, 2))
  "{0,8} {1,7} {2,7} {3,9} {4,9} {5,9}" -f $b.N, $b.Px, $b.Py, $gx, $gy, $dist
}
