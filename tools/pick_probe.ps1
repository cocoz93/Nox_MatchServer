# pick_probe.ps1 — 캐릭터 목록에서 **선택을 바꾸는 방법**을 실측한다.
#
# 왜 — 줄 클릭이 선택을 안 바꾼다(2026-09-10 실측: 커서는 위저드 줄 위인데
#   하이라이트는 1번 그대로 → 위저드 조합이 컨저러로 뛰었다. 주문 58·128 이 증거).
#   그래서 지금까지의 「직업별」은 전부 1번 캐릭터였을 공산이 크다.
#
# 방법 후보를 한 세션에서 차례로 재고, 하이라이트 줄을 **픽셀로 판독**해 판정한다.
#   .\tools\pick_probe.ps1            (미리 join_my_server -StopAtChars 로 목록까지 몰아 둘 것)
param([int]$Pid_ = 0)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$ctrl = Join-Path $repo '_evidence\desk1'
$di   = Join-Path $PSScriptRoot 'di.ps1'

if ($Pid_ -eq 0) {
  $f = Join-Path $ctrl 'lastpid.txt'
  $Pid_ = [int]([IO.File]::ReadAllText($f).Trim())
}

function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
function Shot([string]$n) {
  Send-Desk "target $Pid_`nshotfull $ctrl\$n.png`n"
  Start-Sleep -Seconds 2
}

Add-Type -AssemblyName System.Drawing
# 하이라이트 판독 — 줄 y 대역에서 밝은 회색 픽셀을 센다.
# 줄 중심(1296x999 캡처): 260 · 292 · 325 · 357 · 389 (2026-09-09 실측 목록 좌표)
function Which-Row([string]$png) {
  $img = [Drawing.Bitmap]::FromFile($png)
  $rows = @(260, 292, 325, 357, 389)
  $best = 0; $bestN = -1
  for ($r = 0; $r -lt $rows.Count; $r++) {
    $n = 0
    for ($y = $rows[$r] - 8; $y -le $rows[$r] + 8; $y += 2) {
      for ($x = 240; $x -lt 1020; $x += 6) {
        $c = $img.GetPixel($x, $y)
        # 하이라이트 바 = 회색 (R≈G≈B, 밝기 120~200)
        $mx = [math]::Max($c.R, [math]::Max($c.G, $c.B))
        $mn = [math]::Min($c.R, [math]::Min($c.G, $c.B))
        if ($mx - $mn -lt 25 -and $mx -ge 110 -and $mx -le 210) { $n++ }
      }
    }
    if ($n -gt $bestN) { $bestN = $n; $best = $r + 1 }
  }
  $img.Dispose()
  return @{ row = $best; n = $bestN }
}

$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')

Write-Host "--- 시작 상태 ---"
Shot 'pk0'
$w = Which-Row (Join-Path $ctrl 'pk0.png')
Write-Host ("  하이라이트 = {0}번 (회색점 {1})" -f $w.row, $w.n)

Write-Host "--- 방법 B: 아래 화살표 ---"
& $di -Target $Pid_ -Cmd @('inject 1','k 0xD0 1','sleep 120','k 0xD0 0','sleep 500') | Out-Null
Shot 'pkB1'
$w = Which-Row (Join-Path $ctrl 'pkB1.png')
Write-Host ("  아래 1번 뒤 = {0}번 (회색점 {1})" -f $w.row, $w.n)
& $di -Target $Pid_ -Cmd @('k 0xD0 1','sleep 120','k 0xD0 0','sleep 500') | Out-Null
Shot 'pkB2'
$w = Which-Row (Join-Path $ctrl 'pkB2.png')
Write-Host ("  아래 2번 뒤 = {0}번 (회색점 {1})" -f $w.row, $w.n)

Write-Host "--- 방법 A: 빠른 클릭 (3번 줄, 다운 60ms) ---"
& $di -Target $Pid_ -Cmd ($home3 + @('m 290 144','sleep 500','mb 0 1','sleep 60','mb 0 0','sleep 700')) | Out-Null
Shot 'pkA'
$w = Which-Row (Join-Path $ctrl 'pkA.png')
Write-Host ("  클릭 뒤 = {0}번 (회색점 {1})" -f $w.row, $w.n)

Write-Host "--- 방법 C: 클릭 두 번 (4번 줄) ---"
& $di -Target $Pid_ -Cmd ($home3 + @('m 290 160','sleep 500','mb 0 1','sleep 60','mb 0 0','sleep 350','mb 0 1','sleep 60','mb 0 0','sleep 700')) | Out-Null
Shot 'pkC'
$w = Which-Row (Join-Path $ctrl 'pkC.png')
Write-Host ("  더블 뒤 = {0}번 (회색점 {1})" -f $w.row, $w.n)
