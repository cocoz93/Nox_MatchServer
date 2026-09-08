# onox_drive.ps1 — 오픈녹스 클라 하나를 메뉴부터 인게임까지 몬다. **지표 픽셀로 확인하며.**
#
# 왜 이 방식인가 — 화면 전체 지문 비교는 못 쓴다(메뉴에도 불꽃·눈동자 애니메이션이 돌아
# 아무것도 안 눌러도 매번 「바뀜」이 나온다). 그 화면에만 있는 **버튼 자리의 밝기**를 본다.
#
#   .\tools\onox_drive.ps1 -Target <pid> [-Shot <png>]
param(
  [Parameter(Mandatory=$true)][int]$Target,
  [string]$Shot = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$w    = Join-Path $PSScriptRoot 'winctl.ps1'
Add-Type -AssemblyName System.Drawing

function Bright([int]$x, [int]$y) {
  $t = Join-Path $env:TEMP ('onox_drive_{0}.png' -f $Target)
  & $w -Target $Target -Front -Shot $t | Out-Null
  if (-not (Test-Path $t)) { return -1 }
  $b = [System.Drawing.Bitmap]::FromFile($t)
  $p = $b.GetPixel($x, $y); $b.Dispose()
  Remove-Item $t -Force -ErrorAction SilentlyContinue
  return ($p.R + $p.G + $p.B)
}
function TapUntil([int]$x, [int]$y, [int]$wait, [int]$mx, [int]$my, [int]$thr, [string]$name) {
  for ($k = 1; $k -le 5; $k++) {
    & $w -Target $Target -Front -ClickX $x -ClickY $y | Out-Null
    Start-Sleep -Seconds $wait
    if ((Bright $mx $my) -gt $thr) { Write-Host ("  {0} ({1}번째)" -f $name, $k); return $true }
  }
  Write-Host ("  {0} 실패" -f $name); return $false
}

# 인트로 넘기기
foreach ($i in 1..4) { & $w -Target $Target -Front -Key 0x1B | Out-Null; Start-Sleep -Seconds 2 }

# ── 지표 픽셀 (1920x1080) ─────────────────────────
#   서버목록  Host Game (497,146) > 100
#   팝업      Join      (866,361) > 300
#   캐릭터    Load      (957,853) > 200
TapUntil  869 378 2  497 146 100 '멀티플레이' | Out-Null
& $w -Target $Target -Front -ClickX 497 -ClickY 684 | Out-Null   # Refresh
Start-Sleep -Seconds 9
TapUntil  835 132 3  866 361 300 '서버 줄'     | Out-Null
TapUntil  866 361 4  957 853 200 'Join'        | Out-Null
& $w -Target $Target -Front -ClickX 957 -ClickY 853 | Out-Null   # Load
Start-Sleep -Seconds 22

# ★ 접속 직후는 **관전 상태**다 — 왼클릭·SPACE 로 깨운다.
#   ENTER 는 쓰지 말 것(채팅 입력줄이 열려 키를 다 먹는다).
& $w -Target $Target -Front -ClickX 960 -ClickY 500 | Out-Null
Start-Sleep -Seconds 2
& $w -Target $Target -Key 0x20 | Out-Null
Start-Sleep -Seconds 2
& $w -Target $Target -ClickX 900 -ClickY 450 | Out-Null
Start-Sleep -Seconds 4

if ($Shot -ne '') {
  if (-not [IO.Path]::IsPathRooted($Shot)) { $Shot = Join-Path $root $Shot }
  & $w -Target $Target -Shot $Shot | Out-Null
  Write-Host ("찍음 {0}" -f $Shot)
}
