# pick_row.ps1 — 캐릭터 목록에서 줄 하나를 고르고 픽셀로 확인한다.
#
# ★ 왜 **별도 스크립트**인가 — 같은 클릭(60ms)이 join_my_server **안에서는 다섯 번
#   내리 실패**하고, join 이 끝난 뒤 별도 프로세스에서는 **매번 성공**했다
#   (2026-09-10 pick_probe2: A안 곧장 클릭 = 성공. 화살표 가설은 기각).
#   안팎의 어느 차이가 가르는지 끝내 못 좁혔다 — 그래서 **성공이 재현되는 꼴 그대로**
#   프로세스를 갈라 쓴다. 원인은 관측대기로 남긴다.
#
#   출력: "PICKED=<n>"  (판독된 줄 번호)
param(
  [Parameter(Mandatory)][int]$Pid_,
  [Parameter(Mandatory)][int]$Row,      # 1~5
  [string]$Ctrl = '',
  [int]$Tries = 4
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
if ($Ctrl -eq '') { $Ctrl = Join-Path $repo '_evidence\desk1' }
$di = Join-Path $PSScriptRoot 'di.ps1'

function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $Ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
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

# ★★★ **클릭 전에 큐를 비운다** — 이게 진짜 원인이었다(2026-09-10).
#   join 흐름 안에서는 직전 메뉴 이동 이벤트가 dinput 큐에 남아,
#   다운—업 사이에 잔여 이동이 끼어 **드래그로 읽혔다.**
#   지문 = 실패 캡처마다 커서 불티가 가득(커서가 막 달린 흔적),
#   성공 캡처는 깨끗. 별도 프로세스 실험들이 먹혔던 것도 큐가 비어서다.
function Wait-Drain([int]$maxMs = 5000) {
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalMilliseconds -lt $maxMs) {
    $r = & $di -Target $Pid_ -Cmd @('stat') 2>&1 | Out-String
    if ($r -match '큐 mouse=(\d+)') {
      if ([int]$Matches[1] -le 0) { Start-Sleep -Milliseconds 120; return }
    }
    Start-Sleep -Milliseconds 100
  }
}

$home3 = @('m -3000 -3000','m -3000 -3000','m -3000 -3000','sleep 300')
$rowY = @(112, 128, 144, 160, 176)[$Row - 1]
$picked = 0
foreach ($try in 1..$Tries) {
  & $di -Target $Pid_ -Cmd (@('inject 1') + $home3 + @('m 290 ' + $rowY)) | Out-Null
  Wait-Drain                                     # 이동이 다 먹힐 때까지
  & $di -Target $Pid_ -Cmd @('mb 0 1','sleep 60','mb 0 0','sleep 700') | Out-Null
  Send-Desk "target $Pid_`nshotfull $Ctrl\pickrow.png`n"
  Start-Sleep -Seconds 2
  $picked = Which-Row (Join-Path $Ctrl 'pickrow.png')
  if ($picked -eq $Row) { break }
  Start-Sleep -Milliseconds 800
}
Write-Host ("PICKED={0}" -f $picked)
