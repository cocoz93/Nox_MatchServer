# 한계 다시 재기 — 플랜 `Docs/MO_OPTIMIZE_PLAN_1114.md` 3절. 채택이 쌓여 64방 틱합이 7.7% 넘게 줄었을 때 돈다.
#   ① 5분 판 — StartRooms 부터 StepRooms 씩 올리며 넘을 때까지(첫 지점이 넘으면 내려가며 통과할 때까지) · 지점마다 1판
#   ② 30분 확인 — 5분에서 통과한 가장 큰 지점 1판. 넘으면 한 칸 아래를 한 번 더(첫째 묶음 M4C 와 같은 꼴 · EdgeSec 300)
#   판정 = 첫째 묶음과 같은 기준 — load_report 의 넘음(가장 느린 1% 틱 33ms · 끊김 · 넘침 · 사라짐)이 없고 버림이 아님
#   조건 = 첫째 묶음 M4 와 같은 꼴(방 10명 · fight · 직업 0,1 · 빈도 1 · 끈 계측 셋 `state.json` m3Off) · 가동 시간 문턱은 기본(18시간) 그대로 —
#   비교 판과 달리 절대값이라 문턱을 풀지 않는다(플랜 2절 5번)
#   왜 새 대본: load_series 는 첫째 묶음 전체(16방부터 두 배 · 지점마다 3판)라 다시 재기 몫(약 1.5시간)보다 몇 배 길다(11-14)
#
# 사용: powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_limit.ps1 -StartRooms 224
# 종료 코드: 0 = 끝(한계는 limit.txt 마지막 줄) · 1 = PC 검사 실패 · 2 = 인자 · 미커밋 · 4 = 판을 못 얻음
param(
  [int]$StartRooms = 224,
  [int]$StepRooms = 16,          # 첫째 묶음 좁히기 눈금(M4 · 192 → 208)
  [int]$TopRooms = 0,            # 올라가는 상한 · 0 = 더미 스레드 수 × 스레드당 인원 ÷ 방 인원으로 셈(아래)
  [int]$FloorRooms = 160,
  # ★ 11-15 — 상한을 더미 몫에서 셈한다. 옛 상한 320방(3,200명 · 4스레드)은 근거 없이 정한 값이라 7차 뒤 판이 넘지 않고 상한에서 멈췄다.
  #   스레드당 인원 800 = 11-15 320방 30분 판 실측(`_out/load_run/20261115_105107_LIM_p30_r320_t1/dummy.txt` — 스레드당 800명 ·
  #   한 바퀴 일한 시간 평균 7.77 · 최대 11.17ms = 33ms 의 34% · 측정 구간 안 밀림 0). 최대치는 안 쟀다 — 이 몫을 지키면 더미가 서버보다 먼저 막히지 않고,
  #   막히면 load_report 가 측정 구간 안 더미 밀림으로 버린다(넘음과 갈린다)
  [int]$DummyThreads = 6,        # 6 × 800 ÷ 10 = 480방 — 게임 · 통신 바쁨을 방 수에 비례로 늘린 짐작(게임 약 390 · 통신 약 470방에서 100%)을 둘 다 덮는다
  [int]$PerThread = 800,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$utf8Bom = New-Object Text.UTF8Encoding($true)
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$loadReport = Join-Path $PSScriptRoot 'load_report.ps1'
$size = 10; $joinSec = 0.2; $tailSec = 60
$serverExtra = '--no-event-log --no-first-seen --no-op-count'
if ($TopRooms -le 0) { $TopRooms = [int][math]::Floor($DummyThreads * $PerThread / $size) }

$outDir = Join-Path $repo ("_out\load_limit\" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$logPath = Join-Path $outDir 'limit.txt'
function Log([string]$s) {
  $ln = (Get-Date -Format 'MM-dd HH:mm:ss') + ' ' + $s
  Write-Host $ln
  if (-not $DryRun) { [IO.File]::AppendAllText($logPath, $ln + "`r`n", $utf8Bom) }
}
function Q([string]$s) { return "'" + $s.Replace("'", "''") + "'" }

# 미커밋이 있으면 load_report 가 판을 버린다(`load_report.ps1:152-153`) — 띄우기 전에 막는다
$dirtyN = @(& git -C $repo status --porcelain).Count
if ($dirtyN -gt 0 -and -not $DryRun) { Log "[멈춤] 미커밋 $dirtyN 개 — 판이 다 버려진다(git status)"; exit 2 }
$head = (& git -C $repo rev-parse --short HEAD).Trim()
Log "[한계] 커밋 $head · 시작 $StartRooms 방 · 눈금 $StepRooms · 범위 $FloorRooms~$TopRooms · 더미 스레드 $DummyThreads(스레드당 $PerThread 명까지) · 끈 계측 $serverExtra · 가동 시간 문턱 기본"

# 판 하나 → @{ pass; rec; dir }. 버림이면 같은 지점을 3번까지
function Run-Point([string]$tag, [int]$rooms, [int]$window, [int]$edge) {
  $sec = [int][math]::Ceiling($rooms * $size * $joinSec) + $window + $tailSec
  for ($try = 1; $try -le 3; $try++) {
    $t = "LIM_${tag}_r${rooms}_t$try"
    if ($DryRun) { Log "[판] (시험) $t · 방 $rooms × $size · $sec 초 · 측정 구간 $window"; return @{ pass = ($rooms -le 240); rec = $null; dir = '' } }
    Log "[판] $t · 방 $rooms × $size · $sec 초 · 측정 구간 $window 초"
    $cmd = "& $(Q $loadRun) -Tag $(Q $t) -Rooms $rooms -RoomSize $size -Sec $sec -Behavior 'fight' -Classes '0,1' -FreqMult 1 -CycleSec 0 -DummyThreads $DummyThreads -ExtraServerArgs $(Q $serverExtra); exit `$LASTEXITCODE"
    $out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    foreach ($ln in $out) { if ($ln -match '^\[(게이트|멈춤|주의|끝|코어)\]') { Log "  $ln" } }
    $runDir = $null
    foreach ($ln in $out) { $m = [regex]::Match($ln, '^\[끝\] (.+?) · 서버 종료 코드'); if ($m.Success) { $runDir = $m.Groups[1].Value } }
    if ($code -eq 1) { Log "[멈춤] PC 검사 실패 — $t · $runDir\env.txt(가동 시간 문턱이면 재부팅 · 서비스는 사용자)"; exit 1 }
    if ($code -ne 0 -or -not $runDir) { Log "[멈춤] load_run 종료 코드 $code — $t"; exit 4 }
    $rl = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $loadReport -RunDir $runDir -WindowSec $window -EdgeSec $edge 2>&1 | ForEach-Object { "$_" })
    foreach ($ln in $rl) { Log "  $ln" }
    $rp = Join-Path $runDir 'report.json'
    if (-not (Test-Path -LiteralPath $rp)) { Log "[멈춤] report.json 없음 — $runDir"; exit 4 }
    $rec = Get-Content -Raw -Encoding UTF8 -LiteralPath $rp | ConvertFrom-Json
    if (-not $rec.usable) { Log "[버림] $t · $(@($rec.reasons) -join ' · ')"; continue }
    $ok = (@($rec.over).Count -eq 0)
    Log ("[결과] {0} · {1} · 틱 완료 1% {2} · 보내기 1% {3} · 게임 바쁨 {4}% · 틱합 {5}" -f $t, $(if ($ok) { '통과' } else { "넘음 $(@($rec.over) -join ' · ')" }), $rec.completionP99Ms, $rec.sendP99Ms, $rec.gameBusyPct, $rec.partMsPerSec.'틱합')
    return @{ pass = $ok; rec = $rec; dir = $runDir }
  }
  Log "[멈춤] 같은 지점에서 버림 3번 — $tag $rooms 방"; exit 4
}

# ① 5분 판
$best = 0; $firstOver = 0
$cur = $StartRooms
$r = Run-Point 'p5' $cur 300 0
if ($r.pass) {
  $best = $cur
  while ($cur + $StepRooms -le $TopRooms) {
    $cur += $StepRooms
    $r = Run-Point 'p5' $cur 300 0
    if (-not $r.pass) { $firstOver = $cur; break }
    $best = $cur
  }
  if (-not $firstOver) { Log "[주의] 상한 $TopRooms 방까지 넘지 않았다 — 더 위는 더미 몫을 늘려야 한다(이번엔 안 함)" }
} else {
  $firstOver = $cur
  while ($cur - $StepRooms -ge $FloorRooms) {
    $cur -= $StepRooms
    $r = Run-Point 'p5' $cur 300 0
    if ($r.pass) { $best = $cur; break }
    $firstOver = $cur
  }
  if (-not $best) { Log "[멈춤] $FloorRooms 방까지 내려도 5분 판이 넘는다"; exit 4 }
}
Log "[5분] 통과한 가장 큰 지점 $best 방 · 처음 넘은 지점 $(if ($firstOver) { "$firstOver 방" } else { '없음' })"

# ② 30분 확인 — 넘으면 한 칸 아래를 한 번 더
$c = Run-Point 'p30' $best 1800 300
if ($c.pass) { Log "[끝] 한계 = $best 방(30분 통과) · 5분에서 처음 넘은 지점 $firstOver 방"; exit 0 }
$lower = $best - $StepRooms
if ($lower -lt $FloorRooms) { Log "[멈춤] $best 방 30분이 넘었고 아래 지점이 범위 밖"; exit 4 }
$c2 = Run-Point 'p30' $lower 1800 300
if ($c2.pass) { Log "[끝] 한계 = $lower 방(30분 통과) · $best 방은 5분 통과 · 30분 넘음"; exit 0 }
Log "[멈춤] $best · $lower 방 모두 30분이 넘었다 — 더 아래는 사람이 보고 정한다"; exit 4
