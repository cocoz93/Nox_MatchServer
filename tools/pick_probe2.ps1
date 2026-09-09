# pick_probe2.ps1 — 픽 실패 원인 2차 실험.
#
# 1차(pick_probe)에서 「빠른 클릭이 먹혔다」고 결론냈는데, 같은 60ms 클릭이
# 러너(v3·v4)에서는 **연속 실패**했다. 1차와 러너의 남은 차이 = 1차에서는 클릭 전에
# **화살표 키를 눌렀다**(안 먹혔지만 입력은 갔다). 그 가설을 여기서 가른다 —
#   A안: 진입 → 곧장 60ms 클릭 (러너 재현 — 실패해야 가설이 산다)
#   B안: 진입 → 아래 화살표 1회 → 60ms 클릭 (1차 재현 — 성공해야 가설이 산다)
# 한 번의 몰기에서 A→판독→B→판독 순서로 잰다.
param([int]$Pid_ = 0)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$ctrl = Join-Path $repo '_evidence\desk1'
$di   = Join-Path $PSScriptRoot 'di.ps1'
if ($Pid_ -eq 0) { $Pid_ = [int]([IO.File]::ReadAllText((Join-Path $ctrl 'lastpid.txt')).Trim()) }

function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
function Shot([string]$n) { Send-Desk "target $Pid_`nshotfull $ctrl\$n.png`n"; Start-Sleep -Seconds 2 }

Add-Type -AssemblyName System.Drawing
function Which-Row([string]$png) {
  $img = [Drawing.Bitmap]::FromFile($png)
  $rows = @(260, 292, 325, 357, 389)
  $best = 0; $bestN = -1
  for ($r = 0; $r -lt $rows.Count; $r++) {
    $n = 0
    for ($y = $rows[$r] - 8; $y -le $rows[$r] + 8; $y += 2) {
      for ($x = 240; $x -lt 1020; $x += 6) {
        $c = $img.GetPixel($x, $y)
        $mx = [math]::Max($c.R, [math]::Max($c.G, $c.B)); $mn = [math]::Min($c.R, [math]::Min($c.G, $c.B))
        if ($mx - $mn -lt 25 -and $mx -ge 110 -and $mx -le 210) { $n++ }
      }
    }
    if ($n -gt $bestN) { $bestN = $n; $best = $r + 1 }
  }
  $img.Dispose(); return $best
}

$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')

Write-Host "--- A안: 곧장 60ms 클릭 (3번 줄) ---"
& $di -Target $Pid_ -Cmd (@('inject 1') + $home3 + @('m 290 144','sleep 500','mb 0 1','sleep 60','mb 0 0','sleep 700')) | Out-Null
Shot 'p2A'
Write-Host ("  판독 = {0}번 (바람 3)" -f (Which-Row (Join-Path $ctrl 'p2A.png')))

Write-Host "--- B안: 아래 화살표 1회 → 60ms 클릭 (4번 줄) ---"
& $di -Target $Pid_ -Cmd @('k 0xD0 1','sleep 120','k 0xD0 0','sleep 400') | Out-Null
& $di -Target $Pid_ -Cmd ($home3 + @('m 290 160','sleep 500','mb 0 1','sleep 60','mb 0 0','sleep 700')) | Out-Null
Shot 'p2B'
Write-Host ("  판독 = {0}번 (바람 4)" -f (Which-Row (Join-Path $ctrl 'p2B.png')))

Write-Host "--- C안: 클릭 두 번 연달아 (2번 줄) ---"
& $di -Target $Pid_ -Cmd ($home3 + @('m 290 128','sleep 500','mb 0 1','sleep 60','mb 0 0','sleep 300','mb 0 1','sleep 60','mb 0 0','sleep 700')) | Out-Null
Shot 'p2C'
Write-Host ("  판독 = {0}번 (바람 2)" -f (Which-Row (Join-Path $ctrl 'p2C.png')))
