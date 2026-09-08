# onox_join.ps1 — 떠 있는 오픈녹스 클라를 메뉴부터 몰아 18590 서버에 접속시킨다.
#
# 왜 만드나 — 서버를 고치고 화면으로 확인하는 왕복을 하루에 수십 번 한다.
# 매번 스크린샷을 보며 손으로 좌표를 찍으면 한 번에 대여섯 번 왕복이 든다.
#
# 좌표는 **1920x1080 전체화면 기준**이다(2026-09-08 실측). 창 크기가 바뀌면 다시 떠야 한다.
#
#   .\tools\onox_join.ps1 -Target <클라pid> [-Shot <png>] [-Wake]
#
# 화면이 어디서 시작하든 되도록 닿게, 앞쪽 단계는 **없으면 그냥 지나간다**.
param(
  [Parameter(Mandatory=$true)][int]$Target,
  [string]$Shot = '',
  [switch]$Wake,
  # ★ 단계마다 찍는다. 좌표를 감으로 조절하지 말고 **어디서 어긋났는지 보고** 고칠 것.
  #   `_evidence\trace\NN_<이름>.png` 로 쌓인다.
  [switch]$Trace
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$w    = Join-Path $PSScriptRoot 'winctl.ps1'
$traceDir = Join-Path $root '_evidence\trace'
$step = 0
if ($Trace) {
  if (Test-Path $traceDir) { Get-ChildItem $traceDir -Filter '*.png' | Remove-Item -Force }
  else { New-Item -ItemType Directory -Path $traceDir | Out-Null }
}

function Tap([int]$x, [int]$y, [double]$wait = 1.0, [string]$name = '') {
  & $w -Target $Target -Front -ClickX $x -ClickY $y | Out-Null
  Start-Sleep -Milliseconds ([int]($wait * 1000))
  if ($Trace) {
    $script:step++
    $f = Join-Path $traceDir ("{0:d2}_{1}.png" -f $script:step, ($name -replace '\W','_'))
    & $w -Target $Target -Shot $f | Out-Null
  }
}

Add-Type -AssemblyName System.Drawing
# ★ 한 점의 밝기를 읽는다. **화면 전체 지문은 못 쓴다** — 메뉴에도 불꽃·눈동자
#   애니메이션이 돌아서 아무것도 안 눌러도 매번 「바뀜」이 나온다(2026-09-08).
#   대신 **그 화면에만 있는 버튼 자리**를 본다.
function Bright([int]$x, [int]$y) {
  $tmp = Join-Path $env:TEMP ('onox_pix_{0}.png' -f $PID)
  & $w -Target $Target -Front -Shot $tmp | Out-Null
  if (-not (Test-Path $tmp)) { return -1 }
  $bmp = [System.Drawing.Bitmap]::FromFile($tmp)
  $p = $bmp.GetPixel($x, $y)
  $bmp.Dispose()
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
  return ($p.R + $p.G + $p.B)
}

# ⚠ **한 번 눌러서 안 넘어가면 다시 누른다.** 갓 띄운 클라의 메인 메뉴는 첫 클릭이
#   커서를 옮기는 데만 쓰여 반응하지 않는다(2026-09-08).
#   `$markX/$markY` 가 밝아지면(임계 넘으면) 다음 화면이 뜬 것으로 본다.
function TapUntil([int]$x, [int]$y, [double]$wait, [string]$name,
                  [int]$markX, [int]$markY, [int]$thr, [int]$tries = 5) {
  for ($k = 1; $k -le $tries; $k++) {
    Tap $x $y $wait ("{0}_t{1}" -f $name, $k)
    if ((Bright $markX $markY) -gt $thr) { Write-Host ("  {0}: {1}번째에 넘어감" -f $name, $k); return $true }
  }
  Write-Host ("  {0}: {1}번 눌러도 안 넘어감" -f $name, $tries)
  return $false
}

# ── 좌표표 (1920x1080) ────────────────────────────────────────────
#   게임 안 「응답 없음」 대화상자 Exit ...... 1004, 608
#   서버목록 알림 OK ........................  886, 668
#   Refresh List ............................  497, 684
#   서버 첫 줄 ..............................  835, 132
#   Join ....................................  866, 361
#   Load(캐릭터) ............................  957, 853
Tap 1004  608 2.0 'exit'      # 게임 중이었다면 나가기 (아니면 헛클릭)
Tap  886  668 1.5 'ok'        # 알림 닫기 (없으면 헛클릭)
# ⚠⚠ **메인 메뉴는 클릭을 여러 번 먹여야 반응한다.** 커서를 옮기는 데 한 번이 쓰이고
#   그 다음이 실제 클릭이 되는데, 갓 띄운 클라는 두 번으로도 모자랐다(2026-09-08).
#   `SetCursorPos`·절대 이동·열 걸음 나눠 옮기기 셋 다 소용없었다.
#   → 좌표를 감으로 만지는 대신 **화면이 바뀔 때까지** 누른다.
#   지표 = 서버목록 화면의 「Host Game」 버튼 자리(497,146)가 붉게 밝아진다
TapUntil  869  378 1.5 'multiplay'  497 146 100 | Out-Null
# ⚠ **새로 받기는 한 번만, 대신 넉넉히 기다린다.** 짧게 기다리면 목록이 비어 헛클릭이 되고,
#   그 다음 Join 이 **옛 대상**으로 가 「Connection timed out」이 뜬다. 서버는 멀쩡한데
#   접속이 안 되는 것처럼 보여 한참 헤맸다. 두 번 누르는 것도 답이 아니다 —
#   두 번째가 목록을 **다시 비우는 중**에 줄을 누르게 된다(2026-09-08 둘 다 실측).
Tap  497  684 9.0 'refresh'   # 목록 새로 받기
# 서버 줄 → 팝업. 지표 = Join 버튼 글자(866,361)가 금색으로 밝아진다
TapUntil  835  132 3.0 'row'   866 361 300 | Out-Null
# Join → 캐릭터 선택. 지표 = Load 버튼 글자(957,853)
TapUntil  866  361 4.0 'join'  957 853 200 | Out-Null
Tap  957  853 14.0 'load'     # 캐릭터 Load — 맵 읽기까지 기다린다

if ($Wake) {
  # ★ 접속 직후는 **관전 상태**다 — HUD 가 없고 입력이 안 먹는다.
  #   왼클릭·SPACE 로 깨운다. ENTER 는 쓰지 말 것(채팅 입력줄이 열려 키를 다 먹는다).
  Tap 960 500 2.0
  & $w -Target $Target -Key 0x20 | Out-Null
  Start-Sleep -Seconds 3
}

if ($Shot -ne '') {
  if (-not [IO.Path]::IsPathRooted($Shot)) { $Shot = Join-Path $root $Shot }
  & $w -Target $Target -Shot $Shot | Out-Null
  Write-Host ("찍음 {0}" -f $Shot)
}
Write-Host "접속 순서 끝"
