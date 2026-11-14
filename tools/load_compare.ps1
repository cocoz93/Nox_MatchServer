# 최적화 개선 비교 — 플랜 `Docs/MO_OPTIMIZE_PLAN_1114.md` 2절 4 · 5 · 6번을 한 번에 돈다.
#   ① 같은 결과 확인 — wirecmp 로 고치기 전 서버 두 번 · 고친 서버 한 번 녹음(같은 자동 클라 · 120초씩)해
#      서버→클라 메시지 종류별 수가 고치기 전 두 번의 차이 폭 안인지 본다
#   ② PC 확인 — 고치기 전 서버 64방 × 5분 1판. 틱합이 기준 중앙 ± DriftMs 밖이면 PC 가 느려진 것이라 멈춘다
#   ③ 재기 — 고친 서버(build\Release) 64방 × 5분 × Runs 판 → 채택 · 기각
#   왜 이 대본: 판 넷 · 녹음 셋을 차례로 돌고 판마다 버림 까닭을 가르는 일을 손으로 하면 틀어진다(11-14).
#   load_report 는 문턱이 기본과 다르거나(가동 시간 문턱을 풀면 걸림) 미커밋 파일이 있으면(고친 서버는 커밋 전) 판을 버린다 —
#   이 대본은 버림 까닭이 **그 두 가지뿐일 때만** 받아들인다. 고친 내용은 판 폴더에 `fix.diff` 로 남긴다.
#
# 사용(고치기 전 서버는 고치기 전에 build\Release 를 따로 복사해 둔다 — exe · pdb · dll):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_compare.ps1 -Label r1_aoistatics -BaseExe _out\base_f3cdafc\nox_server.exe
#   ⚠ 사본은 리포 아래 **두 단**에 둔다(아래 접속 덤프 확인 주석)
#
# 종료 코드: 0 = 끝(채택 · 기각은 compare.txt 마지막 줄) · 1 = 같은 결과 확인 어긋남 · 2 = 인자 · 파일 · 3 = PC 느려짐 · 4 = 판을 못 얻음
param(
  [Parameter(Mandatory = $true)][string]$Label,
  [Parameter(Mandatory = $true)][string]$BaseExe,
  [int]$Runs = 3,
  [int]$Rooms = 64,
  # 기준 세 판 틱합 — 첫째 묶음 M4 64방(`_out/load_series/20261113_225933/series.jsonl` · 4844f00). 채택하면 다음 개선은 그 세 판을 준다(플랜 2절 8번)
  [string[]]$BaseTicks = @('288.39', '287.08', '288.68'),   # 글자로 받아 아래에서 쉼표로 나눠 수로 바꾼다(`-File` 은 a,b,c 를 문자열 하나로 넘긴다)
  [double]$AdoptMs = 4.8,           # 채택 = 세 판 모두 기준 최솟값보다 낮고 중앙값이 기준 중앙보다 이만큼 이상 낮음(판끼리 흔들림 1.6 의 세 배)
  [double]$DriftMs = 4.8,           # PC 확인 폭 — 11-14 사용자 「적당히 넓게」(처음 287.1~288.7 은 한 판이 들 확률이 절반이라 넓힘)
  [double]$UptimeMaxHours = 1000,   # 개선 비교 판은 가동 시간 문턱을 푼다(11-14 사용자 「좋아 그렇게가자」 · 대신 ② PC 확인)
  # ★ 11-15 6차 개선 — 판정 값. tick = 틱합(ms/초 · 기본) · busy = 게임 바쁨(게임 스레드가 안 기다린 시간 · % × 10 = ms/초).
  #   틱 시계 밖(`main.cpp` 의 `BeginTick` · 사건 처리)을 고치면 틱합이 못 본다 — 그때 busy. busy 는 판끼리 흔들림이 틱합과 달라
  #   -AdoptMs · -DriftMs · -BaseTicks(기준 세 판의 busy ms/초)를 같이 준다(안 주면 멈춤)
  [ValidateSet('tick', 'busy')][string]$Metric = 'tick',
  [int]$WireSecs = 120,
  [switch]$SkipWire,                # ① 을 건너뜀(시험용)
  [string[]]$WireCaps = @(),        # 시험용 — 녹음 셋(기준1 · 기준2 · 고친)을 새로 뜨지 않고 이 파일들로 ① 판정만 하고 끝낸다
  [switch]$DryRun                   # 아무것도 안 띄우고 계산한 값 · 명령만 찍는다
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$utf8Bom = New-Object Text.UTF8Encoding($true)
$inv = [Globalization.CultureInfo]::InvariantCulture
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$loadReport = Join-Path $PSScriptRoot 'load_report.ps1'
$wirecmp = Join-Path $PSScriptRoot 'wirecmp.ps1'
$opcount = Join-Path $repo 'build\Release\nox_opcount.exe'
$fixExe = Join-Path $repo 'build\Release\nox_server.exe'
if (-not (Test-Path -LiteralPath $BaseExe)) { Write-Host "[멈춤] 고치기 전 서버 없음: $BaseExe"; exit 2 }
$BaseExe = (Resolve-Path -LiteralPath $BaseExe).Path
# 서버는 접속 덤프를 「실행 파일 두 단 위 = 리포」의 `_evidence\CAP_arena_0907_ok.txt` 에서 찾는다(`WorldBuild.cpp` DefaultDumpPath).
#   사본을 리포 아래 두 단(`_out\base_<해시>\`)이 아닌 곳에 두면 클라가 접속만 되고 게임에 못 들어간다 — 11-14 첫 비교가
#   `_out\opt\base_f3cdafc\` 사본으로 녹음 45KB · 종류 6개를 받고 멈췄다. 띄우기 전에 막는다
$baseDump = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $BaseExe))) '_evidence\CAP_arena_0907_ok.txt'
if (-not (Test-Path -LiteralPath $baseDump)) { Write-Host "[멈춤] 고치기 전 서버 자리에서 접속 덤프가 안 보인다 — $baseDump (사본은 리포 아래 두 단 · 예: _out\base_<해시>\)"; exit 2 }
# `-File` 로 부르면 `-WireCaps a,b,c` 가 배열이 아니라 문자열 하나로 온다 — 쉼표로 나눈다(11-14 · 안 나눠 시험 대신 진짜 녹음 · 판이 돌았다)
if ($WireCaps.Count -eq 1 -and $WireCaps[0] -match ',') { $WireCaps = @($WireCaps[0] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
if ($WireCaps.Count -ne 0 -and $WireCaps.Count -ne 3) { Write-Host "[멈춤] -WireCaps 는 셋(기준1 · 기준2 · 고친)이어야 한다 — 지금 $($WireCaps.Count)개"; exit 2 }

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = Join-Path $repo "_out\opt\${stamp}_$Label"
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$logPath = Join-Path $outDir 'compare.txt'
function Log([string]$s) {
  $ln = (Get-Date -Format 'MM-dd HH:mm:ss') + ' ' + $s
  Write-Host $ln
  if (-not $DryRun) { [IO.File]::AppendAllText($logPath, $ln + "`r`n", $utf8Bom) }
}
function Median([double[]]$xs) {
  $s = @($xs | Sort-Object)
  if ($s.Count -eq 0) { return $null }
  if ($s.Count % 2) { return $s[($s.Count - 1) / 2] }
  return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2.0
}
function F([double]$v) { return $v.ToString('0.00', $inv) }

# 첫째 묶음 M4 64방과 같은 꼴(load_series.ps1 Cfg · Run-Sec · 끈 계측 셋)
$size = 10; $window = 300
$sec = [int][math]::Ceiling($Rooms * $size * 0.2) + $window + 60
$extra = '--no-event-log --no-first-seen --no-op-count'
# `-File` 로 부르면 `-BaseTicks a,b,c` 가 문자열 하나로 온다 — [double[]] 로 받으면 변환 오류로 대본이 아예 안 뜬다(11-14 2차 개선).
#   매개 변수는 [string[]] 꼴이 붙어 있어 수를 다시 넣으면 글자로 돌아간다 — 수는 따로 담는다
$baseTicksV = @(($BaseTicks -join ',') -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, $inv) })
if ($baseTicksV.Count -lt 1) { Write-Host "[멈춤] -BaseTicks 가 비었다"; exit 2 }
if ($Metric -eq 'busy') {
  foreach ($need in @('AdoptMs', 'DriftMs', 'BaseTicks')) {
    if (-not $PSBoundParameters.ContainsKey($need)) { Write-Host "[멈춤] -Metric busy 는 -$need 를 같이 준다(기본값은 틱합 몫)"; exit 2 }
  }
}
$mName = if ($Metric -eq 'busy') { '게임 바쁨(ms/초)' } else { '틱합' }
$baseMed = Median $baseTicksV
$baseMin = ($baseTicksV | Measure-Object -Minimum).Minimum
$driftLo = $baseMed - $DriftMs; $driftHi = $baseMed + $DriftMs

$head = (& git -C $repo rev-parse --short HEAD).Trim()
Log "[비교] $Label · 커밋 $head + 미커밋 고침 · 고치기 전 $BaseExe · 고친 $fixExe"
Log "[비교] 방 $Rooms × $size · 실행 $sec 초 · 측정 구간 $window 초 · 끈 계측 $extra · 가동 시간 문턱 $UptimeMaxHours"
Log "[비교] 기준 $mName $(($baseTicksV | ForEach-Object { F $_ }) -join ' · ') · 중앙 $(F $baseMed) · 채택 = 세 판 모두 $(F $baseMin) 미만 · 중앙 $(F ($baseMed - $AdoptMs)) 이하 · PC 확인 폭 $(F $driftLo)~$(F $driftHi)"
if (-not $DryRun) {
  [IO.File]::WriteAllText((Join-Path $outDir 'fix.diff'), ((& git -C $repo diff HEAD) -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
}

# ── ① 같은 결과 확인 — 녹음 셋 ──
#   서버 인자에 `--aoi` 를 준다 — wirecmp 기본 인자에는 시야 반경이 없어 정적 소개 길(AppendAoiStatics · CollectAoiExits)을 안 지난다
function Wire-Counts([string]$cap) {
  # ★ 11-14 2차 개선 — **클라가 마지막으로 보낸 순간까지만 센다.** 자동 클라를 닫은 뒤에도 서버는 끊김을 알 때까지
  #   그 사람을 마지막 입력 그대로 굴린다 — 닫는 순간 달리던 중이면 그 꼬리에 발소리(`0xA7` WalkOnDirt · RunOnDirt ·
  #   HumanMaleExertionLight)가 더 붙는다. 닫는 순간의 자세는 자동 클라 대본의 박자 탓이지 서버 고침 탓이 아니다:
  #   2차 개선 첫 판정이 `0xA7` 기준 61 · 59 · 고친 69 로 멈췄는데 고친 쪽의 8건이 모두 클라 마지막 줄 뒤였고,
  #   장애물 고침이 없는 1차 개선 녹음(`opt_t_same_fix`)도 67건 중 8건이 그 뒤였다. 클라 마지막 줄까지는 61 · 59 · 61
  $lastCs = $null; $lines = 0
  foreach ($ln in [IO.File]::ReadLines($cap)) { $lines++; if ($ln -match '^(\d\d:\d\d:\d\d\.\d{3}) C->S ') { $lastCs = $Matches[1] } }
  if (-not $lastCs) { Log "[멈춤] 녹음 $cap 에 클라→서버 줄이 없다(읽은 줄 $lines)"; exit 4 }
  # nox_opcount 는 UTF-8 로 찍는다 — PowerShell 5.1 은 바깥 프로그램 출력을 콘솔 인코딩(CP949)으로 읽어 한글 머리줄이 깨지고 0종류가 된다(11-14 시험)
  $prevEnc = [Console]::OutputEncoding
  [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
  try { $out = @(& $opcount $cap '--to' $lastCs 2>&1 | ForEach-Object { "$_" }) } finally { [Console]::OutputEncoding = $prevEnc }
  $tail = 0
  foreach ($ln in [IO.File]::ReadLines($cap)) { if ($ln -match '^(\d\d:\d\d:\d\d\.\d{3}) S->C ' -and $Matches[1] -gt $lastCs) { $tail++ } }
  Log "[녹음 자르기] $(Split-Path -Leaf $cap) · 클라 마지막 $lastCs · 그 뒤 서버→클라 $tail 통 뺌(읽은 줄 $lines)"
  $h = @{}; $in = $false
  foreach ($ln in $out) {
    if ($ln -match '^-- 서버→클라 상위') { $in = $true; continue }
    if ($in -and $ln -match '^--') { break }
    if ($in) { $m = [regex]::Match($ln, '^\s+0x([0-9a-f]{2})\s+\S+\s+x (\d+)'); if ($m.Success) { $h[$m.Groups[1].Value] = [int64]$m.Groups[2].Value } }
  }
  return $h
}
function Wire-Run([string]$tag, [string]$exe) {
  if ($DryRun) { Log "[녹음] (시험) $tag · $exe"; return @{} }
  $o = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wirecmp -Tag $tag -Secs $WireSecs -SrvExtra '--aoi' -ServerExe $exe 2>&1 | ForEach-Object { "$_" })
  foreach ($ln in $o) { Log "  $ln" }
  $cap = Join-Path $repo "_evidence\_runs\$tag.cap.txt"
  if (-not (Test-Path -LiteralPath $cap)) { Log "[멈춤] 녹음 파일 없음 — $cap"; exit 4 }
  if (-not ($o -match "$WireSecs 초 동안 셋 다 살아 있었다")) { Log "[멈춤] 녹음 중 클라 · 서버 · 중계기 중 하나가 끝났다 — $tag"; exit 4 }
  return (Wire-Counts $cap)
}
if ($WireCaps.Count -eq 3) {
  $wb1 = Wire-Counts $WireCaps[0]; $wb2 = Wire-Counts $WireCaps[1]; $wf = Wire-Counts $WireCaps[2]
} elseif (-not $SkipWire) {
  $wb1 = Wire-Run "opt_${Label}_base1" $BaseExe
  $wb2 = Wire-Run "opt_${Label}_base2" $BaseExe
  $wf = Wire-Run "opt_${Label}_fix" $fixExe
}
if ($WireCaps.Count -eq 3 -or -not $SkipWire) {
  # 못 읽은 것과 없는 것을 가른다 — 셋 중 하나라도 종류가 10개 미만이면 읽기가 깨진 것(정상 120초 녹음은 40종 안팎 · alias1113d 45종)
  foreach ($pair in @(@('기준1', $wb1), @('기준2', $wb2), @('고친', $wf))) {
    if ($pair[1].Count -lt 10) { Log "[멈춤] 녹음 $($pair[0]) 의 서버→클라 종류가 $($pair[1].Count)개 — nox_opcount 출력을 못 읽었거나 클라가 게임에 못 들어갔다(녹음 크기 · 그 서버 출력의 [경고] 를 볼 것)"; exit 4 }
  }
  # 판정 — 종류마다 d = max(|기준1-기준2|, 평균의 5%, 3). 고친 쪽이 [작은 쪽 - d, 큰 쪽 + d] 안이면 같음.
  #   왜 폭을 넓히나: 같은 서버 두 번도 시각에 따라 수가 다르고, 셋째 판이 앞 두 판 사이에 들 확률은 1/3 뿐이다(계산).
  #   잡으려는 것은 종류가 새로 생기거나 사라지거나 수가 몇 배로 바뀌는 것 — 5% · 3 은 그보다 훨씬 작다(내가 정함 · 11-14)
  $keys = @(@($wb1.Keys) + @($wb2.Keys) + @($wf.Keys) | Sort-Object -Unique)
  $bad = New-Object System.Collections.ArrayList
  foreach ($k in $keys) {
    $b1 = [double]$(if ($wb1.ContainsKey($k)) { $wb1[$k] } else { 0 }); $b2 = [double]$(if ($wb2.ContainsKey($k)) { $wb2[$k] } else { 0 })
    $f = [double]$(if ($wf.ContainsKey($k)) { $wf[$k] } else { 0 })
    $d = [math]::Max([math]::Max([math]::Abs($b1 - $b2), 0.05 * ($b1 + $b2) / 2.0), 3.0)
    $lo = [math]::Min($b1, $b2) - $d; $hi = [math]::Max($b1, $b2) + $d
    $ok = ($f -ge $lo -and $f -le $hi)
    Log ("[녹음 대조] 0x{0} 기준 {1} · {2} · 고친 {3} · 폭 {4}~{5} · {6}" -f $k, $b1, $b2, $f, [math]::Round($lo, 0), [math]::Round($hi, 0), $(if ($ok) { '같음' } else { '★다름' }))
    if (-not $ok) { [void]$bad.Add("0x$k") }
  }
  if ($bad.Count) { Log "[멈춤] 같은 결과 확인 어긋남 — $($bad -join ' · ') (플랜 4절: 되돌리고 그 후보는 미결)"; exit 1 }
  Log "[녹음 대조] 종류 $($keys.Count)개 전부 같은 폭 안"
  if ($WireCaps.Count -eq 3) { Log '[시험] 녹음 판정만 하고 끝'; exit 0 }
}

# ── 판 하나 — load_run → load_report. 받아들일 버림 까닭은 가동 시간 문턱 · 미커밋 둘뿐 ──
function Run-One([string]$tag, [string]$exe) {
  for ($try = 1; $try -le 3; $try++) {
    if ($DryRun) { Log "[판] (시험) $tag · $exe"; return $null }
    Log "[판] $tag · $try 번째 · $exe"
    $o = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $loadRun -Tag $tag -Rooms $Rooms -RoomSize $size -Sec $sec -Behavior fight -Classes '0,1' `
          -FreqMult 1 -CycleSec 0 -ExtraServerArgs $extra -ServerExe $exe -UptimeMaxHours $UptimeMaxHours 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    foreach ($ln in $o) { if ($ln -match '^\[(게이트|멈춤|주의|끝|코어)\]') { Log "  $ln" } }
    $runDir = $null
    foreach ($ln in $o) {
      $m = [regex]::Match($ln, '^\[끝\] (.+?) · 서버 종료 코드'); if ($m.Success) { $runDir = $m.Groups[1].Value }
      $m = [regex]::Match($ln, '^지문: (.+)\\env\.txt$'); if ($m.Success -and -not $runDir) { $runDir = $m.Groups[1].Value }
    }
    if ($code -ne 0 -or -not $runDir) { Log "  load_run 종료 코드 $code — 다시"; continue }
    # 띄운 서버가 맞나 — 판 지문의 「서버 명령줄」 첫머리가 넘긴 실행 파일이어야 한다. 11-14 1차 개선은 load_run 이 `-ServerExe` 를
    #   지역 변수로 덮어(PowerShell 은 변수 이름 대소문자를 안 가림) 「고치기 전」 판이 build\Release 를 띄웠는데 아무 데서도 안 걸렸다
    $envTxt = Join-Path $runDir 'env.txt'
    $cmdLine = @(Get-Content -Encoding UTF8 -LiteralPath $envTxt | Where-Object { $_ -match '^서버 명령줄: ' }) | Select-Object -First 1
    if (-not $cmdLine -or -not $cmdLine.Substring(8).StartsWith($exe, [StringComparison]::OrdinalIgnoreCase)) {
      Log "[멈춤] 띄운 서버가 넘긴 것과 다르다 — 넘김 $exe · 지문 「$cmdLine」"; exit 4
    }
    $rl = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $loadReport -RunDir $runDir -WindowSec $window -EdgeSec 0 2>&1 | ForEach-Object { "$_" })
    foreach ($ln in $rl) { Log "  $ln" }
    $rp = Join-Path $runDir 'report.json'
    if (-not (Test-Path -LiteralPath $rp)) { Log "  report.json 없음 — 다시"; continue }
    $r = Get-Content -Raw -Encoding UTF8 -LiteralPath $rp | ConvertFrom-Json
    $other = @(@($r.reasons) | Where-Object { $_ -notmatch '^문턱 UptimeMaxHours = ' -and $_ -notmatch '^미커밋 파일 \d+개$' })
    if ($other.Count) { Log "  버림 — $($other -join ' · ') — 다시"; continue }
    return $r
  }
  Log "[멈춤] $tag — 3번 해도 쓸 판을 못 얻음"; exit 4
}
function Part($r, [string]$n) { $v = $r.partMsPerSec.$n; if ($null -eq $v) { return 0.0 } else { return [double]$v } }
# 판정 값 — `-Metric`(머리 주석). busy 는 report.json `gameBusyPct`(%) × 10 = ms/초
function Judge($r) { if ($Metric -eq 'busy') { return 10.0 * [double]$r.gameBusyPct } else { return Part $r '틱합' } }
function Run-Line([string]$who, $r) {
  return ("[결과] {0} · 틱합 {1} · 정적 {2} · 걸음 {3} · 사람 {4} · 게임 바쁨 {5}% · 고리 나머지 {14} · 틱 완료 1% {6} · 받은 조각 {7} · 끊김 {8} · 넘침 {9} · 입력 차이 {10}% · 조각 차이 {11}% · 메모리 {12}MB · {13}" -f `
    $who, (F (Part $r '틱합')), (F (Part $r '정적')), (F (Part $r '걸음')), (F (Part $r '사람')), $r.gameBusyPct, $r.completionP99Ms, $r.recvPieces,
    $r.timeouts, $r.overflow, $r.inputDiffPct, $r.pieceDiffPct, $r.memPrivMaxMb, $r.dir, $r.loopRestMsPerSec)
}

# ── ② PC 확인 — 고치기 전 서버 1판 ──
$rb = Run-One "OPT_${Label}_base" $BaseExe
if ($DryRun) { Log '[시험] 끝'; exit 0 }
Log (Run-Line '고치기 전' $rb)
$bt = Judge $rb
if ($bt -lt $driftLo -or $bt -gt $driftHi) {
  $dir = if ($bt -gt $driftHi) { '느려짐' } else { '빨라짐 — 기준이 지금 PC 와 안 맞는다' }
  Log "[멈춤] PC 상태가 기준과 다르다($dir) — 고치기 전 서버 $mName $(F $bt) 이 $(F $driftLo)~$(F $driftHi) 밖(플랜 4절: 측정을 멈추고 고치기 · 빌드 · 시험만)"; exit 3
}
Log "[PC 확인] 고치기 전 $(F $bt) — 폭 안"

# ── ③ 재기 — 고친 서버 Runs 판 ──
$fix = @()
for ($i = 1; $i -le $Runs; $i++) {
  $r = Run-One "OPT_${Label}_fix_p$i" $fixExe
  Log (Run-Line "고친 $i" $r)
  $fix += $r
}
$ft = @($fix | ForEach-Object { Judge $_ })
$fmed = Median $ft
$allBelow = @($ft | Where-Object { $_ -ge $baseMin }).Count -eq 0
$adopt = $allBelow -and ($fmed -le $baseMed - $AdoptMs)
$loss = @($fix + @($rb) | Where-Object { $_.timeouts -ne 0 -or $_.overflow -ne 0 -or $_.inputDiffPct -ne 0 -or $_.pieceDiffPct -ne 0 }).Count
$rooms1 = $fmed / $Rooms
Log ("[판정] 고친 $mName {0} · 중앙 {1}(방 하나 {2}ms) · 기준 중앙 {3} → 차 {4} · 고치기 전 판 {5} 대비 {6} · 손실 있는 판 {7}" -f `
  (($ft | ForEach-Object { F $_ }) -join ' · '), (F $fmed), $rooms1.ToString('0.000', $inv), (F $baseMed), (F ($fmed - $baseMed)), (F $bt), (F ($fmed - $bt)), $loss)
if ($loss -gt 0) { Log '[끝] 기각 — 손실(끊김 · 넘침 · 입력 · 조각 차이)이 0 이 아닌 판이 있다'; exit 0 }
Log $(if ($adopt) { '[끝] 채택' } else { "[끝] 기각 — 세 판 모두 기준 최솟값 미만 $allBelow · 중앙 차 $(F ($fmed - $baseMed)) (기준 -$(F $AdoptMs))" })
exit 0
