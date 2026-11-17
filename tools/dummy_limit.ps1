# 더미 한계 재기 — 플랜 `Docs/MO_DUMMY_LIMIT_PLAN_1117.md`(git 밖). 더미 스레드 하나가 측정 구간 5분 동안 한 번도 밀리지 않는
#   (구간 안 lateTicks 0) 가장 많은 인원을 잰다. 그 × 80% 가 앞으로 「더미 스레드 하나에 몇 명을 싣나」(load_limit.ps1 PerThread)와
#   코어 나누기의 더미 쪽 입력이 된다 — 「서버가 몇 명을 받나」가 아니라 「서버를 재는 도구가 어디까지 믿을 만한가」를 정한다.
#   ① 5분 판 — StartPeople 부터 남은 여유만큼 올린다(5절 2 · 11-17 사용자 「많이 남으면 많이 올리고 적게 남으면 적당히」)
#   ② 몫 = 한계 × ShareRatio · 100명 내림(5절 3 · D-1)
#   ③ 30분 확인 1판 — 넘으면 몫 −200 한 번 더(5절 4 · D-2)
#   판정 네 갈래(3절) = Get-Verdict · 다음 인원(5절 2) = Get-Next — 둘 다 값을 돌려주는 함수다(-SelfTest 가 갈래를 전수로 본다)
#   조건 = 측정 순서 1번(load_limit.ps1)과 같다 — 방 10명 · fight · 직업 0,1 · 빈도 1 · 끈 계측 셋 · 서버 게임 스레드 1 · 서버 코어 0x3F.
#   바꾸는 것 = 더미 --threads 1 · 더미 코어 0xC0(물리 코어 하나 · 4절) · 인원
#
# 사용: powershell -NoProfile -ExecutionPolicy Bypass -File tools\dummy_limit.ps1 -ServerLimitRooms 368
#       powershell -NoProfile -ExecutionPolicy Bypass -File tools\dummy_limit.ps1 -SelfTest      (판 없이 두 함수만 시험)
#       powershell -NoProfile -ExecutionPolicy Bypass -File tools\dummy_limit.ps1 -ServerLimitRooms 368 -DryRun   (가짜 곡선으로 차례만)
# 종료 코드: 0 = 끝(limit.txt 마지막 줄) · 1 = PC 검사 실패 · 2 = 인자 · 미커밋 · 4 = 판을 못 얻음 ·
#            5 = 플랜이 멈추라는 자리(파일럿 어긋남 · 가를 수 없음 · 서버 넘음 · 바닥에서도 넘음 · 30분 두 번 넘음) · 9 = 자체 시험 실패
param(
  [int]$ServerLimitRooms = 0,    # 측정 순서 1번 한계 방 수(load_limit limit.txt 마지막 줄) — 상한 = × 0.9 × 방 인원(5절 2 · 그 위는 서버가 먼저 찬다)
  [int]$StartPeople = 1200,      # 8절 — 800명은 최대 11.17ms(33ms 의 34%)라 넘을 일이 없어 판 하나를 아낀다
  [double]$ShareRatio = 0.8,     # D-1(11-17 사용자 「권장대로」) — 실제 판은 스레드 여섯이 캐시 · 메모리를 나눠 쓰는 몫을 이 판이 못 본다
  [int64]$DummyMask = 0xC0,      # 4절 — 물리 코어 하나(논리 6 · 7). 실제 판의 「스레드마다 제 물리 코어」 꼴
  [switch]$SelfTest,
  [switch]$DryRun,
  # -DryRun 가짜 곡선 — 본 흐름의 갈래(멈춤 ① · ② · 상한 · 바닥 · 30분 넘음)를 판 없이 지나가 보는 손잡이. 측정엔 안 쓴다
  [double]$DrySlope = 0.0117,    # 명당 여유 ms(11-15 800명 최대 11.17 → 비례)
  [int]$DryOverAt = 2500,        # 이 인원을 넘으면 더미 넘음
  [int]$DryP30Over = 0           # 30분 판 중 앞에서 몇 판을 더미 넘음으로(0 · 1 · 2)
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$utf8Bom = New-Object Text.UTF8Encoding($true)
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$loadReport = Join-Path $PSScriptRoot 'load_report.ps1'
$size = 10; $joinSec = 0.2; $tailSec = 60                       # load_limit.ps1 과 같은 값
$serverExtra = '--no-event-log --no-first-seen --no-op-count'   # load_limit.ps1 과 같은 값(끈 계측 셋)
$targetMs = 29.7     # 33ms 의 90% — 여유 신호(상위 5%)가 여기 닿으면 멈춤 ①. 몫이 한계의 80% 라 한계를 10% 안쪽으로만 알면 된다(8절)
$stepMin = 200       # 한 번에 적어도 +200명 · 넘은 판과 마지막 통과가 이만큼 안이면 멈춤 ②(8절 — 몫을 100명 단위로 정하기에 충분)
$grain = 100         # 인원 단위(내림)
$floorPeople = 400   # 첫 판부터 넘으면 절반씩 내리는 바닥(5절 2)

function Floor100([double]$x) { return [int]([math]::Floor($x / $grain) * $grain) }

# ── 판정 네 갈래(3절) ── 갈래 = 통과 · 더미 넘음 · 버림 · 가를 수 없음. 버림은 retry 로 둘을 가른다:
#   load_report 의 다른 버림 까닭 → 같은 지점 3번까지(retry $true) · 서버 넘음 → 서버가 먼저 찼다, 더미 판정을 못 함(retry $false)
#   load_report 는 구간 안 lateTicks > 0 을 버림 까닭으로 둔다(`load_report.ps1:383` · 서버 측정엔 맞다) — 이 대본에선 그 까닭 하나만이면 더미 넘음으로 읽는다
function Get-Verdict($rec) {
  $other = @(@($rec.reasons) | Where-Object { $_ -and $_ -notmatch '^측정 구간 안 더미 lateTicks \d+$' })
  if ($other.Count -gt 0) { return @{ kind = '버림'; retry = $true; why = ($other -join ' · ') } }
  $srv = New-Object System.Collections.ArrayList
  foreach ($x in @($rec.over)) { if ($x -and $x -ne '사라짐') { [void]$srv.Add($x) } }   # 틱 완료 · 보내는 쪽 지연 · 끊김 · 넘침
  if ($rec.queueDrops -gt 0) { [void]$srv.Add("서버 큐 버림 $($rec.queueDrops)") }      # 사라짐 넷 중 서버 쪽 둘
  if ($rec.sendFail -gt 0) { [void]$srv.Add("보내기 실패 $($rec.sendFail)") }
  if ($srv.Count -gt 0) { return @{ kind = '버림'; retry = $false; why = '서버 넘음 ' + ($srv -join ' · ') } }
  if ($rec.dummyLateTicksInWindow -gt 0) { return @{ kind = '더미 넘음'; retry = $false; why = "구간 안 lateTicks $($rec.dummyLateTicksInWindow)" } }
  $loss = $rec.params.lossPercent   # load_report 의 문턱을 그대로(두 대본에 따로 적지 않는다)
  $lost = @()
  if ($rec.inputDiffPct -gt $loss) { $lost += "입력 차이 $($rec.inputDiffPct)%" }
  if ($rec.pieceDiffPct -gt $loss) { $lost += "조각 차이 $($rec.pieceDiffPct)%" }
  if ($lost.Count -gt 0) { return @{ kind = '가를 수 없음'; retry = $false; why = ($lost -join ' · ') + " > $loss% — 더미 받기 버퍼 넘침인지 서버 쪽인지 못 가른다" } }
  return @{ kind = '통과'; retry = $false; why = "구간 안 lateTicks 0 · 여유(초마다 최대의 상위 5%) $($rec.dummyBusyMaxP95MsInWindow)ms" }
}

# ── 다음 인원(5절 2) ── 돌려주는 kind = next(people 로 다음 판) · stop1 · stop2 · cap(한계 = people) · floor · error(멈춤)
#   통과: 여유 ≥ 29.7ms → stop1 · 위에 넘은 판이 있으면 그와 200명 안 → stop2, 아니면 가운데 · 없으면 비례(× 29.7 ÷ 여유 · 2배까지 · 적어도 +200 · 상한까지)
#   더미 넘음: 통과가 없으면 절반(바닥 400 · 바닥에서 넘으면 floor) · 마지막 통과와 200명 안 → stop2 · 아니면 가운데
#   비례가 모자라게 짚는 쪽인 까닭 = 인원이 늘수록 명당 일이 줄었다(2절) · 늘어나는 쪽으로 꺾이면 넘음 → 가운데가 받는다
function Get-Next([int]$cur, [string]$kind, $p95, [int]$lastPass, [int]$overMin, [int]$cap) {
  switch ($kind) {
    '통과' {
      if ($null -eq $p95 -or [double]$p95 -le 0) { return @{ kind = 'error'; people = 0; why = '여유 값(dummyBusyMaxP95MsInWindow)이 없다 — 새 1초 줄이 안 찍혔다' } }
      if ([double]$p95 -ge $targetMs) { return @{ kind = 'stop1'; people = $cur; why = "여유 $p95 ms ≥ $targetMs ms — 실제 한계는 이 위 약 10% 안쪽" } }
      if ($overMin -gt 0) {
        if ($overMin - $cur -le $stepMin) { return @{ kind = 'stop2'; people = $cur; why = "위에 넘은 $overMin 명과 $($overMin - $cur) 명 차이" } }
        return @{ kind = 'next'; people = (Floor100 (($cur + $overMin) / 2.0)); why = "넘은 $overMin 명과의 가운데" }
      }
      if ($cur -ge $cap) { return @{ kind = 'cap'; people = $cur; why = "상한 $cap 명까지 안 넘음 — 더미 한 스레드 ≥ $cap 명(아래쪽 값)" } }
      $raw = Floor100 ($cur * $targetMs / [double]$p95)
      $n = [math]::Max([math]::Min($raw, 2 * $cur), $cur + $stepMin)
      $why = "비례 $raw 명(× $targetMs ÷ $p95)"
      if ($n -ne $raw) { $why += $(if ($n -eq 2 * $cur) { ' → 2배 상한' } else { ' → 적어도 +200' }) }
      if ($n -ge $cap) { $n = $cap; $why += " → 상한 $cap" }
      return @{ kind = 'next'; people = [int]$n; why = $why }
    }
    '더미 넘음' {
      if ($lastPass -le 0) {
        if ($cur -le $floorPeople) { return @{ kind = 'floor'; people = 0; why = "바닥 $floorPeople 명도 넘음" } }
        return @{ kind = 'next'; people = [int][math]::Max($floorPeople, (Floor100 ($cur / 2.0))); why = '통과 없이 넘음 — 절반' }
      }
      if ($cur - $lastPass -le $stepMin) { return @{ kind = 'stop2'; people = $lastPass; why = "마지막 통과 $lastPass 명과 $($cur - $lastPass) 명 차이" } }
      return @{ kind = 'next'; people = (Floor100 (($lastPass + $cur) / 2.0)); why = "마지막 통과 $lastPass 명과의 가운데" }
    }
    default { return @{ kind = 'error'; people = 0; why = "다음 인원을 못 정하는 갈래 $kind" } }
  }
}

# ── 자체 시험 ── 갈래마다 가짜 report 하나 · 다음 인원 경계 값. 값이 아니라 개수를 본다(갈래 넷이 다 나왔나 · 맞은 수)
if ($SelfTest) {
  $bad = 0; $n = 0
  function Check([string]$name, $got, $want) { $script:n++; if ("$got" -ne "$want") { $script:bad++; Write-Host "[틀림] $name — 받음 $got · 기대 $want" } else { Write-Host "[맞음] $name — $got" } }
  $p = [pscustomobject]@{ lossPercent = 0.1 }
  function FakeRep($reasons, $over, $late, $qd, $sf, $inp, $pc) { return [pscustomobject]@{ reasons = $reasons; over = $over; dummyLateTicksInWindow = $late; queueDrops = $qd; sendFail = $sf; inputDiffPct = $inp; pieceDiffPct = $pc; params = $p; dummyBusyMaxP95MsInWindow = 12.3 } }
  $cases = @(
    @('통과', (FakeRep @() @() 0 0 0 0.003 0), '통과|False'),
    @('더미 넘음', (FakeRep @('측정 구간 안 더미 lateTicks 3') @() 3 0 0 0.01 0), '더미 넘음|False'),
    @('서버 넘음(틱 완료 + lateTicks)', (FakeRep @('측정 구간 안 더미 lateTicks 2') @('틱 완료') 2 0 0 0 0), '버림|False'),
    @('서버 넘음(큐 버림 · 사라짐)', (FakeRep @() @('사라짐') 0 5 0 0 0), '버림|False'),
    @('다른 버림 까닭', (FakeRep @('입장 1100/1200', '측정 구간 안 더미 lateTicks 1') @() 1 0 0 0 0), '버림|True'),
    @('가를 수 없음', (FakeRep @() @('사라짐') 0 0 0 0.2 0.05), '가를 수 없음|False')
  )
  $kinds = @{}
  foreach ($c in $cases) { $v = Get-Verdict $c[1]; $kinds[$v.kind] = 1; Check "갈래 $($c[0])" "$($v.kind)|$($v.retry)" $c[2] }
  Check '갈래 종류 수' $kinds.Count 4
  # Get-Next(cur, kind, p95, lastPass, overMin, cap)
  $nx = @(
    @('2배 상한', @(1200, '통과', 10.0, 0, 0, 3100), 'next 2400'),
    @('+200 하한', @(1200, '통과', 29.0, 0, 0, 3100), 'next 1400'),
    @('비례 그대로', @(1200, '통과', 20.0, 0, 0, 3100), 'next 1700'),
    @('29.7ms 딱', @(1200, '통과', 29.7, 0, 0, 3100), 'stop1 1200'),
    @('29.69ms', @(1200, '통과', 29.69, 0, 0, 3100), 'next 1400'),
    @('넘음 차이 200 딱', @(2600, '더미 넘음', $null, 2400, 0, 3100), 'stop2 2400'),
    @('넘음 차이 201', @(2601, '더미 넘음', $null, 2400, 0, 3100), 'next 2500'),
    @('첫 판 넘음', @(1200, '더미 넘음', $null, 0, 0, 3100), 'next 600'),
    @('바닥 넘음', @(400, '더미 넘음', $null, 0, 0, 3100), 'floor 0'),
    @('넘은 위로 가운데', @(1800, '통과', 20.0, 1800, 2400, 3100), 'next 2100'),
    @('넘은 위와 200 안', @(2200, '통과', 20.0, 2200, 2400, 3100), 'stop2 2200'),
    @('상한으로 깎임', @(2800, '통과', 20.0, 2800, 0, 3100), 'next 3100'),
    @('상한 닿음', @(3100, '통과', 25.0, 3100, 0, 3100), 'cap 3100'),
    @('여유 값 없음', @(1200, '통과', $null, 0, 0, 3100), 'error 0')
  )
  foreach ($c in $nx) { $a = $c[1]; $r = Get-Next $a[0] $a[1] $a[2] $a[3] $a[4] $a[5]; Check "다음 $($c[0])" "$($r.kind) $($r.people)" $c[2] }
  Write-Host "[자체 시험] $($n - $bad)/$n 맞음"
  if ($bad -gt 0) { exit 9 } else { exit 0 }
}

if ($ServerLimitRooms -le 0) { Write-Host '[멈춤] -ServerLimitRooms 가 없다(측정 순서 1번 한계 방 수)'; exit 2 }
$cap = Floor100 ($ServerLimitRooms * 0.9 * $size)
if ($StartPeople -gt $cap) { $StartPeople = $cap }

$outDir = Join-Path $repo ("_out\dummy_limit\" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$logPath = Join-Path $outDir 'limit.txt'
function Log([string]$s) {
  $ln = (Get-Date -Format 'MM-dd HH:mm:ss') + ' ' + $s
  Write-Host $ln
  if (-not $DryRun) { [IO.File]::AppendAllText($logPath, $ln + "`r`n", $utf8Bom) }
}
function Q([string]$s) { return "'" + $s.Replace("'", "''") + "'" }

# 미커밋이 있으면 load_report 가 판을 버린다 — 띄우기 전에 막는다(load_limit.ps1 과 같음)
$dirtyN = @(& git -C $repo status --porcelain).Count
if ($dirtyN -gt 0 -and -not $DryRun) { Log "[멈춤] 미커밋 $dirtyN 개 — 판이 다 버려진다(git status)"; exit 2 }
$head = (& git -C $repo rev-parse --short HEAD).Trim()
Log ("[더미 한계] 커밋 $head · 시작 $StartPeople 명 · 상한 $cap 명(서버 한계 $ServerLimitRooms 방 × 0.9 × $size) · 더미 --threads 1 · 코어 0x{0:X} · 몫 비율 $ShareRatio · 끈 계측 $serverExtra" -f $DummyMask)

# 판 하나 → @{ v = 갈래; rec; dir; min }. 다른 버림 까닭이면 같은 지점을 3번까지
function Run-Point([string]$tag, [int]$people, [int]$window, [int]$edge) {
  $rooms = [int]($people / $size)
  $sec = [int][math]::Ceiling($people * $joinSec) + $window + $tailSec
  for ($try = 1; $try -le 3; $try++) {
    $t = "DLIM_${tag}_n${people}_t$try"
    if ($DryRun) {   # 가짜 곡선(위 Dry* 손잡이) — 차례 확인용일 뿐이다
      $late = if ($people -gt $DryOverAt) { 4 } else { 0 }
      if ($window -ge 1800 -and $script:dryP30Seen++ -lt $DryP30Over) { $late = 2 }
      $fake = [pscustomobject]@{ reasons = @(); over = @(); dummyLateTicksInWindow = $late; queueDrops = 0; sendFail = 0
        inputDiffPct = 0; pieceDiffPct = 0; params = [pscustomobject]@{ lossPercent = 0.1 }; dummyBusyMaxP95MsInWindow = [math]::Round($people * $DrySlope, 2) }
      $v = Get-Verdict $fake
      Log "[판] (시험) $t · 방 $rooms × $size · $sec 초 · 측정 구간 $window · $($v.kind) · $($v.why)"
      return @{ v = $v; rec = $fake; dir = ''; min = [math]::Round(($sec + 90) / 60.0, 1) }
    }
    Log "[판] $t · 방 $rooms × $size · $sec 초 · 측정 구간 $window 초"
    $t0 = Get-Date
    $cmd = "& $(Q $loadRun) -Tag $(Q $t) -Rooms $rooms -RoomSize $size -Sec $sec -Behavior 'fight' -Classes '0,1' -FreqMult 1 -CycleSec 0 -DummyThreads 1 -DummyMask $DummyMask -ExtraServerArgs $(Q $serverExtra); exit `$LASTEXITCODE"
    $out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    foreach ($ln in $out) { if ($ln -match '^\[(게이트|멈춤|주의|끝|코어)\]') { Log "  $ln" } }
    $runDir = $null
    foreach ($ln in $out) { $m = [regex]::Match($ln, '^\[끝\] (.+?) · 서버 종료 코드'); if ($m.Success) { $runDir = $m.Groups[1].Value } }
    if ($code -eq 1) { Log "[멈춤] PC 검사 실패 — $t · $runDir\env.txt"; exit 1 }
    if ($code -ne 0 -or -not $runDir) { Log "[멈춤] load_run 종료 코드 $code — $t"; exit 4 }
    $rl = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $loadReport -RunDir $runDir -WindowSec $window -EdgeSec $edge 2>&1 | ForEach-Object { "$_" })
    foreach ($ln in $rl) { Log "  $ln" }
    $rp = Join-Path $runDir 'report.json'
    if (-not (Test-Path -LiteralPath $rp)) { Log "[멈춤] report.json 없음 — $runDir"; exit 4 }
    $rec = Get-Content -Raw -Encoding UTF8 -LiteralPath $rp | ConvertFrom-Json
    $v = Get-Verdict $rec
    $min = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    Log ("[결과] {0} · {1} · {2} · 여유 상위 5% {3}ms · 구간 최대 {4}ms · 평균 {5}ms · 구간 초 {6} · 틱 완료 1% {7} · 게임 바쁨 {8}% · {9}분" -f $t, $v.kind, $v.why,
      $rec.dummyBusyMaxP95MsInWindow, $rec.dummyBusyMaxMsInWindow, $rec.dummyBusyAvgMsInWindow, $rec.dummyBusySecondsInWindow, $rec.completionP99Ms, $rec.gameBusyPct, $min)
    if ($v.kind -eq '버림' -and $v.retry) { continue }
    return @{ v = $v; rec = $rec; dir = $runDir; min = $min }
  }
  Log "[멈춤] 같은 지점에서 버림 3번 — $tag $people 명"; exit 4
}

# 판정이 「이 갈래면 멈춘다」인 둘(서버 넘음 · 가를 수 없음) — 5분 · 30분 판 공통
function Stop-IfUnsplittable($r, [string]$where) {
  switch ($r.v.kind) {
    '가를 수 없음' { Log "[멈춤] $where 가를 수 없음 — $($r.v.why) · $($r.dir)"; exit 5 }
    '버림' { Log "[멈춤] $where 서버 넘음 — 더미 판정을 못 한다(상한 $cap 명 아래에서 서버가 먼저 참 · 1번 한계와 어긋남) · $($r.v.why) · $($r.dir)"; exit 5 }
  }
}

# ① 5분 판 — 여유만큼 올리기
$cur = $StartPeople; $lastPass = 0; $overMin = 0; $first = $true; $limit = 0; $lower = $false; $dryP30Seen = 0
while ($true) {
  $r = Run-Point 'p5' $cur 300 0
  Stop-IfUnsplittable $r '5분 판'
  if ($first) {
    # 파일럿 = 첫 판(5절 1) — 새 1초 줄이 측정 구간 초만큼 찍혔나. 갈래 · 다음 인원은 아래 줄로 사람이 대조한다
    $first = $false
    if (-not $DryRun -and ($null -eq $r.rec.dummyBusyMaxP95MsInWindow -or $r.rec.dummyBusySecondsInWindow -lt 290)) {
      Log "[멈춤] 파일럿 — 새 1초 줄이 구간 초만큼 없다(구간 초 $($r.rec.dummyBusySecondsInWindow) · 여유 $($r.rec.dummyBusyMaxP95MsInWindow)) · $($r.dir)"; exit 5
    }
    Log "[파일럿] 새 1초 줄 구간 초 $($r.rec.dummyBusySecondsInWindow) · 갈래 $($r.v.kind) · 판 하나 $($r.min) 분"
  }
  if ($r.v.kind -eq '통과') { $lastPass = [math]::Max($lastPass, $cur) } else { if ($overMin -eq 0 -or $cur -lt $overMin) { $overMin = $cur } }
  $nx = Get-Next $cur $r.v.kind $r.rec.dummyBusyMaxP95MsInWindow $lastPass $overMin $cap
  Log "[다음] $($nx.kind) $($nx.people) 명 · $($nx.why) · 지금 마지막 통과 $lastPass · 넘은 가장 작은 $overMin"
  # ⚠ switch 안의 continue · break 는 바깥 while 이 아니라 switch 에 걸린다 — 표지로 나간다
  $again = $false
  switch ($nx.kind) {
    'next' { $cur = $nx.people; $again = $true }
    'stop1' { $limit = $nx.people }
    'stop2' { $limit = $nx.people }
    'cap' { $limit = $nx.people; $lower = $true }
    'floor' { Log "[멈춤] 바닥 $floorPeople 명에서도 더미가 밀린다 — 더미 쪽을 사람이 본다"; exit 5 }
    default { Log "[멈춤] 다음 인원을 못 정함 — $($nx.why)"; exit 5 }
  }
  if (-not $again) { break }
}
Log ("[5분] 더미 한 스레드 한계 {0} 명{1}" -f $limit, $(if ($lower) { '(상한까지 안 넘음 — 아래쪽 값)' } else { '' }))

# ② 몫 → ③ 30분 확인 — 넘으면 몫 −200 한 번 더
$share = Floor100 ($limit * $ShareRatio)
foreach ($s in @($share, ($share - $stepMin))) {
  if ($s -lt $floorPeople) { Log "[멈춤] 30분 확인 인원 $s 명이 바닥 밑"; exit 5 }
  $c = Run-Point 'p30' $s 1800 300
  Stop-IfUnsplittable $c '30분 판'
  if ($c.v.kind -eq '통과') {
    Log ("[끝] 더미 한 스레드 한계 = {0} 명{1} · 몫 = {2} 명(30분 통과 · 몫 비율 {3}{4}) · PerThread 에 넣을 값 = {2}" -f $limit, $(if ($lower) { '(아래쪽 값)' } else { '' }), $s, $ShareRatio, $(if ($s -ne $share) { " · 몫 $share 명은 30분 넘음" } else { '' }))
    exit 0
  }
}
Log "[멈춤] 몫 $share · $($share - $stepMin) 명 모두 30분이 넘었다 — 5분 판과 30분 판이 이만큼 어긋나는 까닭을 사람이 본다"; exit 5
