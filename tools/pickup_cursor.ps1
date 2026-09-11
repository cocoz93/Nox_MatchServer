# pickup_cursor.ps1 — **커서를 물건 위에 올리고** 눌러 `0x73 TRY_GET` 을 끌어낸다.
#
# 왜 새로 만드나 (2026-09-11) —
#   `pickup_sweep.ps1` 이 **일곱 번(sw1~sw7) 실패**했다. 기준선(왼버튼 클릭 +1)은 움직였으니
#   계측은 멀쩡했다 — **누른 조작 중에 답이 없었다.** 정답지를 뜨니 까닭이 나왔다:
#
#   ⭕ `opennox/src/input_c.go:356-368` —
#       if v54 >= 75*75 { CursorPickupFar }              // 멀면 **커서만 바꾸고 안 보낸다**
#       else { … if 주버튼 눌림 { clientPickup(sprite) } }  // → `0x73`
#     여기서 **`sprite` 는 「커서 아래 오브젝트」**다.
#
#   → **커서를 물건 위에 올려야** 한다. 앞선 스윕은 커서를 안 옮기고 클릭만 했다.
#
# ⚠ `nav.ps1` 은 같은 일을 하지만 **윈도 `mouse_event`** 를 쓴다 — 숨은 데스크톱에서는 안 먹는다.
#   그래서 `di.ps1` 의 `m <dx> <dy>`(DirectInput 상대 축)로 옮겼다.
#   ★ 상대 이동이라 **절대 좌표를 모른다** → 좌상단으로 크게 민 뒤 목표만큼 되돌린다.
#   ★★ 감도(화면픽셀 ÷ 보낸값)를 모르므로 **격자로 훑는다.** 한 점이라도 맞으면 요청이 오른다.
#
#   .\tools\pickup_cursor.ps1 -Tag pc1
param(
  [string]$Tag = 'pc1',
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map',
  # 격자 — 화면 가운데를 중심으로 ±Span 을 Step 칸으로 훑는다(보낸값 기준).
  [int]$Span = 120,
  [int]$Step = 40,
  # 한 점마다 기다리는 초. ⚠ `[물건]` 줄은 **5초에 한 번** 찍히므로 그보다 길어야 한다.
  [int]$WaitPer = 6
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$slog = Join-Path $root ("_evidence\srv_{0}.log" -f $Tag)
$di   = Join-Path $PSScriptRoot 'di.ps1'

Write-Host ("[조건] 격자 ±{0} 간격 {1} · 점마다 {2}초 · 꼬리표={3}" -f $Span, $Step, $WaitPer, $Tag) -ForegroundColor Cyan

function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try { return (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
}
# `[물건] 줍기요청 N(성공 M·멂 K)` 에서 셋을 읽는다. 못 읽으면 -1.
#   ★ 이 줄은 이제 **늘 찍힌다**(2026-09-11 고침). 전에는 셋 다 0 이면 줄이 없어
#     「0 이었나」와 「안 쟀나」를 못 갈랐다.
function PickCounts {
  $l = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[물건\]' } | Select-Object -Last 1)
  if ($l.Count -eq 0) { return @(-1, -1, -1) }
  if ($l[0] -match '줍기요청 (\d+)\(성공 (\d+)·멂 (\d+)\)') {
    return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
  }
  return @(-1, -1, -1)
}
function Cleanup {
  Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
}
Cleanup
Start-Sleep -Seconds 3
if (Test-Path $slog) { [IO.File]::Delete($slog) }

$exe = Join-Path $root 'build\Release\nox_server.exe'
$cmd = 'cmd /c ""' + $exe + '" --port 18590 --standalone --map "' + $Map +
       '" --mode arena --item-at-spawn > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $cmd) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3

# ★ 손잡이가 먹었는지 **먼저 확인한다** — 안 먹었으면 스윕 전체가 무효다.
$boot = ReadShared $slog
$moved = @(($boot -split "`r?`n") | Where-Object { $_ -match '\[시험\].*아이템 자리' })
if ($moved.Count -eq 0) {
  Write-Host '[멈춤] --item-at-spawn 이 안 먹었다 (「[시험] 스폰 자리를…」 줄이 없다)' -ForegroundColor Red
  ($boot -split "`r?`n") | Where-Object { $_ -match '\[시험\]' } | Select-Object -First 2
  Cleanup; exit 6
}
Write-Host ("  " + $moved[0].Trim()) -ForegroundColor Green

& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag $Tag -CharRow 1 |
  Select-String -Pattern '픽 확인' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
$p = @(Get-Process -Name Game3 -ErrorAction SilentlyContinue)
if ($p.Count -eq 0) { Write-Host '[멈춤] 클라가 안 떴다' -ForegroundColor Red; Cleanup; exit 6 }
$cpid = $p[0].Id
& $di -Target $cpid -Cmd @('inject 1') | Out-Null
Start-Sleep -Seconds 4

# ★ 사람이 실제로 물건 위에 있나 — `[자리]` 줄로 확인한다(손잡이와 실제를 가른다).
$posLine = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[자리\].*물건' } | Select-Object -Last 1)
if ($posLine.Count -gt 0) {
  Write-Host ("  " + $posLine[0].Trim())
  if ($posLine[0] -match '사람 \d+=(\d+),(\d+)' ) {
    $px = [int]$Matches[1]; $py = [int]$Matches[2]
    if ($posLine[0] -match '물건 \d+=(\d+),(\d+)') {
      $ix = [int]$Matches[1]; $iy = [int]$Matches[2]
      $d = [Math]::Sqrt([Math]::Pow($ix-$px,2) + [Math]::Pow($iy-$py,2))
      Write-Host ("  사람↔가장 가까운 물건 거리 {0:N1} (클라 문턱 75)" -f $d)
      if ($d -ge 75) { Write-Host '  ⚠ 75 이상이다 — 클라가 아예 안 보낸다. 이 회차는 무효다' -ForegroundColor Yellow }
    }
  }
}

$start = PickCounts
Write-Host ''
Write-Host ("--- 커서 격자 스윕 --- (시작 요청 {0} · 성공 {1} · 멂 {2})" -f $start[0], $start[1], $start[2])

$prev = $start
$hit = @()
foreach ($dy in -$Span..$Span) {
  if (($dy + $Span) % $Step -ne 0) { continue }
  foreach ($dx in -$Span..$Span) {
    if (($dx + $Span) % $Step -ne 0) { continue }
    # ★ 좌상단으로 크게 밀어 **기준점**을 만든 뒤(상대 이동이라 절대 좌표를 모른다)
    #   화면 가운데(대략 640,480 보낸값)로 가고, 거기서 격자만큼 더 움직인다.
    $cmds = @('m -3000 -3000','m -3000 -3000','sleep 60',
              ('m {0} {1}' -f (640 + $dx), (480 + $dy)), 'sleep 150',
              'mb 0 1','sleep 150','mb 0 0')
    & $di -Target $cpid -Cmd $cmds | Out-Null
    Start-Sleep -Seconds $WaitPer
    $now = PickCounts
    $d0 = $now[0] - $prev[0]
    if ($d0 -gt 0) {
      $mark = '★'
      $hit += ("({0},{1}) 요청+{2}·성공+{3}·멂+{4}" -f $dx, $dy, $d0, ($now[1]-$prev[1]), ($now[2]-$prev[2]))
    } else { $mark = ' ' }
    Write-Host ("  {0} ({1,4},{2,4})  요청 +{3} · 성공 +{4} · 멂 +{5}" -f
                $mark, $dx, $dy, $d0, ($now[1]-$prev[1]), ($now[2]-$prev[2]))
    $prev = $now
  }
}

Write-Host ''
Write-Host '=== 결과 ==='
if ($hit.Count -gt 0) {
  Write-Host ('  ★ 줍기 요청이 난 자리 — ' + ($hit -join ' · ')) -ForegroundColor Green
} else {
  Write-Host '  ➖ 격자 어디서도 줍기 요청이 안 났다' -ForegroundColor Yellow
  Write-Host '     → 커서가 물건에 안 닿았거나(격자·감도), 클라가 그 물건을 못 집는 것으로 본다'
}
$fin = PickCounts
Write-Host ("  마지막 — 요청 {0} · 성공 {1} · 멂 {2}" -f $fin[0], $fin[1], $fin[2])
Cleanup
exit $(if ($hit.Count -gt 0) { 0 } else { 4 })
