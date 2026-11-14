# 측정 플랜 M8 — 함수별 시간(표본 수집 도구 xperf) 1회. 관리자 권한이 있어야 한다(커널 표본 기록).
#   하는 일: ① 둘째 묶음이 끝나기를 기다린다 ② load_run 으로 L방(첫째 묶음 state.json) × 10명 전투를 띄운다
#            ③ 서버가 뜬 뒤 방이 다 차면 SampleSec 초만 xperf 로 표본을 모은다 ④ load_run 이 끝나기를 기다린다
#   판정 값에는 안 쓴다(측정 플랜 `Docs/MO_MEASURE_PLAN_1111.md:137`) — 표본 수집이 부하를 조금 더해서다.
#   왜 이 대본: 클로드 세션은 관리자 권한이 없다 — 사람이 관리자 창에서 한 번 띄워 두면 묶음 끝을 기다렸다가 혼자 돈다(11-14).
#   읽기(함수 표)는 관리자 권한이 필요 없다 — 끝나면 클로드가 `xperf -i <etl> -symbols -a profile -detail` 로 뽑는다.
param(
  [string]$SeriesDir = '_out/load_series/20261113_225933',
  [int]$Rooms = 0,                  # 0 = 묶음 폴더 state.json 의 L
  [int]$SampleSec = 120,            # 표본을 모으는 초 — 게임 스레드 1코어에서 약 12만 표본(1ms 간격 · xperf 기본)이면 2% 몫도 2,400 표본이라 넉넉하다(계산)
  [int]$WaitMaxMin = 300,           # 묶음 끝 · 서버 뜨기를 기다리는 최대 분
  [switch]$DryRun                   # 시험용 — 관리자 확인 · 기다림 · 실행 · xperf 없이 계산한 값만 찍는다
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$xperf = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
$loadRun = Join-Path $PSScriptRoot 'load_run.ps1'
$series = Join-Path $repo $SeriesDir
$seriesTxt = Join-Path $series 'series.txt'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir = Join-Path $repo "_out\m8\$stamp"
if (-not $DryRun) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
$logPath = Join-Path $outDir 'm8.txt'
function Log([string]$s) {
  $ln = "$(Get-Date -Format 'MM-dd HH:mm:ss') $s"
  Write-Host $ln
  if (-not $DryRun) { [IO.File]::AppendAllText($logPath, $ln + "`r`n", (New-Object Text.UTF8Encoding($true))) }
}

# ── 관리자 확인 ──
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $DryRun) { Write-Host '[멈춤] 관리자 권한이 아니다 — 시작 메뉴에서 Windows PowerShell 을 「관리자로 실행」한 창에서 다시 띄울 것'; exit 2 }
if (-not (Test-Path $xperf)) { Log "[멈춤] xperf 가 없다 — $xperf"; exit 2 }

# ── 방 수 · 실행 길이 — 첫째 묶음 M4 와 같은 꼴(load_series.ps1 Cfg · Run-Sec) ──
$state = Get-Content (Join-Path $series 'state.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Rooms -le 0) { $Rooms = [int]$state.L }
if ($Rooms -le 0) { Log '[멈춤] state.json 에 L 이 없다 — -Rooms 로 줄 것'; exit 2 }
$size = 10; $window = 300; $joinSec = 0.2; $tailSec = 60
$sec = [int][math]::Ceiling($Rooms * $size * $joinSec) + $window + $tailSec
# 끈 계측 — 둘째 묶음 기록과 같은 셋(series.txt 「끈 계측」 줄 · 첫째 묶음 M3 결과로 정함)
$extra = '--no-event-log --no-first-seen --no-op-count'
$tag = "M8_r${Rooms}_xperf"
$argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$loadRun`" -Tag $tag -Rooms $Rooms -RoomSize $size -Sec $sec -Behavior fight -Classes 0,1 -FreqMult 1 -CycleSec 0 -ExtraServerArgs `"$extra`""
# 표본 시작 = 서버가 뜬 뒤 입장 몫 + 60초(측정 구간은 마지막 방 + 30초 뒤 다음 10초 경계부터 · load_series.ps1:37)
$sampleAt = [int][math]::Ceiling($Rooms * $size * $joinSec) + 60
Log "[M8] 방 $Rooms × $size · 실행 $sec 초 · 표본 = 서버 뜬 뒤 $sampleAt 초부터 $SampleSec 초 · 폴더 $outDir"
Log "[M8] load_run 인자: $argLine"
# 마지막 「[시작] 묶음」 뒤에 「[끝] 묶음」 이나 「[멈춤]」 이 있고 서버 · 더미가 없으면 끝난 것이다(이미 끝난 뒤 띄워도 바로 넘어간다)
function Batch-Done {
  $lines = @(Get-Content $seriesTxt -Encoding UTF8)
  $start = -1
  for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match '\[시작\] 묶음') { $start = $i; break } }
  $ended = $false
  for ($i = [math]::Max(0, $start); $i -lt $lines.Count; $i++) { if ($lines[$i] -match '\[끝\] 묶음|\[멈춤\]') { $ended = $true } }
  $busy = @(Get-Process nox_server, nox_dummy -ErrorAction SilentlyContinue)
  return ($ended -and $busy.Count -eq 0)
}
if ($DryRun) {
  Log "[시험] 관리자 $isAdmin · 둘째 묶음 끝 $(Batch-Done) · 서버 · 더미 $(@(Get-Process nox_server, nox_dummy -ErrorAction SilentlyContinue).Count)개"
  exit 0
}

# ── ① 둘째 묶음이 끝나기를 기다린다 ──
$deadline = (Get-Date).AddMinutes($WaitMaxMin)
while (-not (Batch-Done)) {
  if ((Get-Date) -gt $deadline) { Log "[멈춤] $WaitMaxMin 분 기다려도 둘째 묶음이 안 끝났다"; exit 3 }
  Start-Sleep -Seconds 30
}
Log '[M8] 둘째 묶음 끝 확인 — 실행을 띄운다(먼저 load_run 의 PC 검사가 돈다)'

# ── ② load_run 을 띄운다 ──
$lr = Start-Process -FilePath 'powershell.exe' -ArgumentList $argLine -WorkingDirectory $repo -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $outDir 'load_run.txt') -RedirectStandardError (Join-Path $outDir 'load_run_err.txt')

# ── ③ 서버가 뜨기를 기다렸다가 방이 다 차면 표본을 모은다 ──
$sv = $null
while (-not $sv) {
  if ($lr.HasExited) { Log "[멈춤] 서버가 뜨기 전에 load_run 이 끝났다(종료 코드 $($lr.ExitCode)) — $outDir\load_run.txt"; exit 4 }
  if ((Get-Date) -gt $deadline.AddMinutes(120)) { Log '[멈춤] 서버가 안 뜬다'; exit 4 }
  $sv = Get-Process nox_server -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $sv) { Start-Sleep -Seconds 5 }
}
$svStart = $sv.StartTime
Log "[M8] 서버 뜸 $($svStart.ToString('HH:mm:ss')) · PID $($sv.Id) — $sampleAt 초 뒤 표본 시작"
$wait = ($svStart.AddSeconds($sampleAt) - (Get-Date)).TotalSeconds
if ($wait -gt 0) { Start-Sleep -Seconds ([int][math]::Ceiling($wait)) }
if ($sv.HasExited -or $lr.HasExited) { Log '[멈춤] 표본 전에 서버나 load_run 이 끝났다'; exit 5 }

$kernelEtl = Join-Path $outDir 'kernel.etl'
$mergedEtl = Join-Path $outDir 'm8.etl'
$started = $false
try {
  # PROC_THREAD+LOADER = 어느 프로세스 · 모듈의 주소인지 · PROFILE = 표본 · 스택도 같이(부른 쪽 포함 몫을 내려고)
  # 원형 4GB 상한 — 표본이 생각보다 커도 디스크를 다 쓰지 않는다
  & $xperf -on PROC_THREAD+LOADER+PROFILE -stackwalk Profile -BufferSize 1024 -MinBuffers 256 -MaxBuffers 1024 -MaxFile 4096 -FileMode Circular -f $kernelEtl
  if ($LASTEXITCODE -ne 0) { Log "[멈춤] xperf 시작 실패(종료 코드 $LASTEXITCODE)"; exit 6 }
  $started = $true
  Log "[M8] 표본 시작 — $SampleSec 초"
  Start-Sleep -Seconds $SampleSec
} finally {
  if ($started) {
    & $xperf -d $mergedEtl
    $dCode = $LASTEXITCODE
    Log "[M8] 표본 끝 · xperf -d 종료 코드 $dCode · $mergedEtl"
    if ($dCode -ne 0) { & $xperf -stop | Out-Null }
  }
}

# ── ④ load_run 이 끝나기를 기다린다 ──
if (-not $lr.WaitForExit(($sec + 900) * 1000)) { Log '[주의] load_run 이 제때 안 끝났다 — 남은 프로세스를 볼 것' }
else { Log "[M8] load_run 끝 · 종료 코드 $($lr.ExitCode)" }
$etlMb = if (Test-Path $mergedEtl) { [math]::Round((Get-Item $mergedEtl).Length / 1MB, 1) } else { 0 }
Log "[끝] M8 · etl $etlMb MB · $mergedEtl"
exit 0
