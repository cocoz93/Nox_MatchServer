# 부하 실험 한 회 실행 — PC 검사(게이트) · 환경 지문(env.txt) · 코어 지정 · 서버 + 더미 실행 · 서버 멈춤 이벤트로 끝내기
#
#   왜 이 대본이 있나: 측정값은 PC 상태에 따라 흔들린다(가동 시간 · 다른 프로그램 · 전원 계획 · 직전 실행의 잔열).
#   그래서 측정 직전마다 같은 검사를 돌리고, 못 넘으면 서버를 안 띄운다. 지문이 없는 실행의 절대값은 인용하지 않는다.
#   값의 근거 = Docs/MO_DUMMY_PLAN_1109.md 6절 · Docs/MO_LOAD_VARIABLE_PLAN_1108.md 3절.
#
#   무거운 프로그램은 이 대본이 끄지 않는다 — 사용자 작업 중일 수 있어 측정 직전에 사용자가 끈다.
#
# 사용:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_run.ps1 -CheckOnly
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_run.ps1 -Tag r10x4 -Rooms 4 -RoomSize 10 -Sec 300
#
# 종료 코드: 0 = 통과(또는 실행 끝) · 1 = 게이트 실패 · 2 = 실행 파일 · 맵 없음 · 3 = 코어 지정이 다름 · 4 = 서버가 안 뜸
param(
  [string]$Tag = 'run',
  [int]$Rooms = 1,
  [int]$RoomSize = 10,
  [int]$Sec = 300,
  [int]$Port = 18590,
  [string]$Classes = '0,1',
  [string]$Behavior = 'fight',
  [int]$StatMult = 5,
  [double]$FreqMult = 1.0,
  [int]$DummyThreads = 4,                # 더미 내부 스레드(측정 플랜 4절 — 1,280명이면 한 바퀴 최대 약 94ms 라 3스레드가 필요하고 하나 더)
  [int]$CycleSec = 0,                    # 방 여닫기(측정 플랜 M7) — 더미 --cycle-sec 로 넘긴다. 마지막 방이 이 초마다 닫히고 새로 열린다 · 0 = 끔
  [string]$ExtraServerArgs = '',
  [string]$ExtraDummyArgs = '',
  [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Nox',
  [switch]$CheckOnly,
  # 게이트 문턱 — 시험 때 일부러 틀리게 줄 수 있도록 인자로 뺐다. 측정에서는 기본값을 쓴다
  [double]$UptimeMaxHours = 18,          # MMO_Zone 실측 — 0.5~18시간 정상 · 26시간 +16% · 105시간 +70~94%
  [double]$IdleMaxPercent = 1,           # 1초씩 3번 전부 이 값 미만
  [double]$ResidualMaxPercent = 2.5,     # 25초마다 읽어 2연속 이 값 미만
  [int]$ResidualMaxReads = 14,           # 25초 × 14 = 350초 — MMO_Zone 실측 잔열 약 4.5분(270초)에 두 번 연속 읽을 몫 50초와 조금을 더한 값
  [string]$PowerPlanGuid = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c',   # 고성능
  [string]$UpdateServiceStatus = 'Stopped'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$serverExe = Join-Path $repo 'build\Release\nox_server.exe'
$dummyExe = Join-Path $repo 'build\Release\nox_dummy.exe'
$map = Join-Path $GameDir 'maps\So_LOD\So_LOD.map'
$runRoot = Join-Path $repo '_out\load_run'
$stamp = Join-Path $runRoot 'last_end.txt'   # 직전 실행이 끝난 시각(UTC 틱) — 잔열 게이트와 지문이 읽는다

# 코어 — 이 PC(i9-10900)는 물리 코어 k 가 논리 2k · 2k+1 이다. 서버 = 물리 0~2 · 더미 = 물리 3~9
$serverMask = 0x3F
$dummyMask = 0xFFFC0

foreach ($p in @($serverExe, $dummyExe, $map)) {
  if (-not (Test-Path -LiteralPath $p)) { Write-Host "[멈춤] 없음: $p"; exit 2 }
}

New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$runDir = Join-Path $runRoot ((Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + $Tag)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

# 전체 CPU 사용률 — 성능 카운터 이름은 윈도 언어에 따라 달라 GetSystemTimes 로 직접 읽는다
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NoxLoadRunCpu {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
}
'@

# CPU 속도(측정 플랜 3절) — 성능 카운터를 영어 이름으로 연다(PdhAddEnglishCounter 는 윈도 언어와 상관없다).
#   실제 MHz = 기준 MHz(Processor Frequency) × 성능 %(% Processor Performance · 터보면 100 을 넘어 NOCAP100 으로 읽는다) ÷ 100.
#   값은 지난번 읽은 때부터 이번까지의 평균이다. 대본 안에서 읽어 따로 프로세스를 안 띄운다(WMI 는 WmiPrvSE 가 서버 코어에서 돌 수 있다)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NoxLoadRunPdh {
    [StructLayout(LayoutKind.Explicit)]
    struct FmtValue { [FieldOffset(0)] public uint CStatus; [FieldOffset(8)] public double Value; }
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] static extern uint PdhOpenQueryW(string src, IntPtr user, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr user, out IntPtr counter);
    [DllImport("pdh.dll")] static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll")] static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, IntPtr type, out FmtValue value);
    const uint FmtDoubleNoCap = 0x200 | 0x8000;   // PDH_FMT_DOUBLE | PDH_FMT_NOCAP100
    static IntPtr q, perf, freq;
    public static string Open() {
        uint r = PdhOpenQueryW(null, IntPtr.Zero, out q);
        if (r != 0) return "PdhOpenQuery 0x" + r.ToString("X");
        r = PdhAddEnglishCounterW(q, @"\Processor Information(_Total)\% Processor Performance", IntPtr.Zero, out perf);
        if (r != 0) return "% Processor Performance 0x" + r.ToString("X");
        r = PdhAddEnglishCounterW(q, @"\Processor Information(_Total)\Processor Frequency", IntPtr.Zero, out freq);
        if (r != 0) return "Processor Frequency 0x" + r.ToString("X");
        r = PdhCollectQueryData(q);
        if (r != 0) return "PdhCollectQueryData 0x" + r.ToString("X");
        return "";
    }
    public static bool Read(out double perfPct, out double baseMhz) {
        perfPct = 0; baseMhz = 0;
        if (PdhCollectQueryData(q) != 0) return false;
        FmtValue v;
        if (PdhGetFormattedCounterValue(perf, FmtDoubleNoCap, IntPtr.Zero, out v) != 0) return false;
        perfPct = v.Value;
        if (PdhGetFormattedCounterValue(freq, FmtDoubleNoCap, IntPtr.Zero, out v) != 0) return false;
        baseMhz = v.Value;
        return true;
    }
}
'@

function Read-CpuPercent([int]$Ms) {
  $i0 = 0L; $k0 = 0L; $u0 = 0L; $i1 = 0L; $k1 = 0L; $u1 = 0L
  [void][NoxLoadRunCpu]::GetSystemTimes([ref]$i0, [ref]$k0, [ref]$u0)
  Start-Sleep -Milliseconds $Ms
  [void][NoxLoadRunCpu]::GetSystemTimes([ref]$i1, [ref]$k1, [ref]$u1)
  $total = ($k1 - $k0) + ($u1 - $u0)     # 커널 시간에 유휴 시간이 들어 있다
  if ($total -le 0) { return 0.0 }
  return [math]::Round(100.0 * ($total - ($i1 - $i0)) / $total, 2)
}

# 프로세스별 CPU 시간이 이 동안 얼마나 늘었나 — 게이트를 못 넘었을 때 범인을 보여 주고 지문에도 남긴다
function Get-TopCpu([int]$Ms, [int]$Top) {
  $before = @{}
  foreach ($p in Get-Process) { if ($p.CPU -ne $null) { $before[$p.Id] = $p.CPU } }
  Start-Sleep -Milliseconds $Ms
  $rows = foreach ($p in Get-Process) {
    if ($p.CPU -eq $null) { continue }
    $old = 0.0
    if ($before.ContainsKey($p.Id)) { $old = $before[$p.Id] }
    [pscustomobject]@{ Name = $p.ProcessName; Id = $p.Id; Delta = [math]::Round($p.CPU - $old, 2) }
  }
  return $rows | Sort-Object Delta -Descending | Select-Object -First $Top
}

$env_lines = New-Object System.Collections.ArrayList
function Note([string]$Key, [string]$Value) { [void]$env_lines.Add("$Key`: $Value") }
function Save-Env { [IO.File]::WriteAllLines((Join-Path $runDir 'env.txt'), [string[]]$env_lines, (New-Object Text.UTF8Encoding($true))) }

$failed = New-Object System.Collections.ArrayList
function Gate([string]$Name, [bool]$Ok, [string]$Detail) {
  $mark = if ($Ok) { '통과' } else { '실패' }
  Write-Host "[게이트] $Name — $mark · $Detail"
  Note "게이트 $Name" "$mark · $Detail"
  if (-not $Ok) { [void]$failed.Add($Name) }
}

# 환경 변수 — 서버 코드가 읽는 셋을 비운다(이 프로세스와 자식에게만)
$cleared = @()
foreach ($n in @('NOX_TRACE_OUT', 'NOX_NO_DOOR_WALLS', 'NOX_DUMP_SOUND_FLAGS')) {
  if ([Environment]::GetEnvironmentVariable($n, 'Process')) { $cleared += $n }
  [Environment]::SetEnvironmentVariable($n, $null, 'Process')
}

# ── 지문 앞부분 ──────────────────────────────────────────────
$commit = (& git -C $repo rev-parse HEAD).Trim()
$dirty = @(& git -C $repo status --porcelain).Count
Note '시각' (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
Note '커밋' $commit
Note '미커밋 파일 수' "$dirty"
$noxVars = @(Get-ChildItem Env: | Where-Object { $_.Name -like 'NOX_*' } | ForEach-Object { "$($_.Name)=$($_.Value)" })
Note 'NOX_ 환경 변수' $(if ($noxVars.Count) { $noxVars -join ' ; ' } else { '없음' })
Note '비운 환경 변수' $(if ($cleared.Count) { $cleared -join ' ; ' } else { '없음' })

# 직전 실행이 끝난 뒤 지난 시간
$sinceLast = $null
if (Test-Path -LiteralPath $stamp) {
  $sinceLast = [math]::Round(((Get-Date).ToUniversalTime().Ticks - [int64](Get-Content -LiteralPath $stamp -Raw).Trim()) / 1e7, 0)
  Note '직전 실행이 끝난 뒤' "$sinceLast 초"
} else {
  Note '직전 실행이 끝난 뒤' '기록 없음'
}

# ── 게이트 ────────────────────────────────────────────────────
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$uptime = [math]::Round(((Get-Date) - $boot).TotalHours, 2)
Gate '가동 시간' ($uptime -le $UptimeMaxHours) "$uptime 시간 · 문턱 $UptimeMaxHours 시간 이하"
Note '가동 시간(시간)' "$uptime"

$active = (& powercfg /getactivescheme | Out-String)
$guid = ''
if ($active -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') { $guid = $Matches[1].ToLower() }
Gate '전원 계획' ($guid -eq $PowerPlanGuid.ToLower()) "지금 $guid · 기대 $PowerPlanGuid"
Note '전원 계획' $active.Trim()

$wu = (Get-Service wuauserv).Status.ToString()
Gate '업데이트 서비스' ($wu -eq $UpdateServiceStatus) "지금 $wu · 기대 $UpdateServiceStatus"
Note '업데이트 서비스' $wu

try {
  $mp = Get-MpComputerStatus
  Note '백신 실시간 검사' "$($mp.RealTimeProtectionEnabled)"
} catch {
  Note '백신 실시간 검사' "못 읽음 — $($_.Exception.Message)"
}

Start-Sleep -Seconds 3   # 대본이 방금 부른 것(컴파일 · git · WMI 조회)이 식기를 기다린다 — 안 그러면 첫 읽기에 대본 자신의 몫이 섞인다
$idleReads = @(1..3 | ForEach-Object { Read-CpuPercent 1000 })
$idleOk = @($idleReads | Where-Object { $_ -ge $IdleMaxPercent }).Count -eq 0
Gate '유휴 CPU' $idleOk "$($idleReads -join ' · ')% · 문턱 $IdleMaxPercent% 미만(1초씩 3번 전부)"
Note '유휴 CPU(%)' ($idleReads -join ' ')

# 잔열 — 직전 실행이 없으면 볼 것이 없고, 유휴가 이미 못 넘었으면 읽어도 못 넘으니 기다리지 않는다
if ($null -eq $sinceLast) {
  Write-Host '[게이트] 잔열 — 건너뜀 · 직전 실행 기록 없음'
  Note '게이트 잔열' '건너뜀 · 직전 실행 기록 없음'
} elseif (-not $idleOk) {
  Write-Host '[게이트] 잔열 — 건너뜀 · 유휴 CPU 를 못 넘어 기다려도 소용없음'
  Note '게이트 잔열' '건너뜀 · 유휴 CPU 를 못 넘음'
} else {
  $reads = @(); $streak = 0; $residualOk = $false
  for ($i = 0; $i -lt $ResidualMaxReads; $i++) {
    $v = Read-CpuPercent 25000
    $reads += $v
    if ($v -lt $ResidualMaxPercent) { $streak++ } else { $streak = 0 }
    if ($streak -ge 2) { $residualOk = $true; break }
  }
  Gate '잔열' $residualOk "$($reads -join ' · ')% · 문턱 $ResidualMaxPercent% 미만 2연속 · 25초마다"
}

$top = @(Get-TopCpu 3500 10)
$topText = ($top | ForEach-Object { "$($_.Name)($($_.Id)) $($_.Delta)" }) -join ' ; '
Note '상위 프로세스 CPU 증가(초, 3.5초 동안)' $topText

if ($failed.Count -gt 0) {
  Write-Host "[멈춤] 게이트 실패: $($failed -join ', ') — 서버를 안 띄운다"
  Write-Host "CPU 상위(3.5초 동안 늘어난 초): $topText"
  Write-Host "지문: $runDir\env.txt"
  Save-Env
  exit 1
}

if ($CheckOnly) {
  Write-Host '[통과] 게이트 전부 통과 — -CheckOnly 라 띄우지 않는다'
  Write-Host "지문: $runDir\env.txt"
  Save-Env
  exit 0
}

# ── 실행 ──────────────────────────────────────────────────────
# 인자는 운영 인자(tools/mo_host.ps1)에서 로비를 빼고 실험 옵션을 붙인 것 — 플랜 6절
$results = Join-Path $runDir 'matches.jsonl'
$serverArgs = "--quiet --aoi --friends --battle-maps Estate --no-join-token --standalone --map `"$map`" --load-rooms $Rooms --load-room-size $RoomSize --stat-mult $StatMult --tick-sections --results `"$results`""
if ($Port -ne 18590) { $serverArgs += " --port $Port" }
if ($ExtraServerArgs) { $serverArgs += " $ExtraServerArgs" }
$dummyArgs = "--server 127.0.0.1:$Port --count $($Rooms * $RoomSize) --room-size $RoomSize --classes $Classes --behavior $Behavior --stat-mult $StatMult --freq-mult $FreqMult --duration-sec $Sec --threads $DummyThreads"
if ($CycleSec -gt 0) { $dummyArgs += " --cycle-sec $CycleSec" }
if ($ExtraDummyArgs) { $dummyArgs += " $ExtraDummyArgs" }
Note '서버 명령줄' "$serverExe $serverArgs"
Note '더미 명령줄' "$dummyExe $dummyArgs"

function Stop-Server($Proc) {
  # 멈춤 이벤트 먼저(콘솔 프로그램은 창 닫기 신호를 못 받는다) — 강제 종료는 응답이 없을 때만
  $name = "nox_server_stop_$($Proc.Id)"
  # 띄운 직후면 서버가 아직 이벤트를 안 만들었다 — 0.2초마다 10초까지 다시 연다(코어가 달라 곧바로 끝낼 때 15초를 헛기다리다 강제 종료한 일)
  $ev = $null
  for ($i = 0; $i -lt 50 -and $null -eq $ev -and -not $Proc.HasExited; $i++) {
    try { $ev = [System.Threading.EventWaitHandle]::OpenExisting($name) } catch { Start-Sleep -Milliseconds 200 }
  }
  if ($null -ne $ev) { [void]$ev.Set() } else { Write-Host "[주의] 멈춤 이벤트를 못 찾음: $name" }
  if (-not $Proc.WaitForExit(15000)) {
    Write-Host '[주의] 서버가 15초 안에 안 끝나 강제 종료'
    $Proc.Kill()
    return $false
  }
  return $true
}

function Set-Affinity($Proc, [int64]$Mask, [string]$Who) {
  $Proc.ProcessorAffinity = [IntPtr]$Mask
  $Proc.Refresh()
  $got = [int64]$Proc.ProcessorAffinity
  $text = '0x{0:X} (기대 0x{1:X})' -f $got, $Mask
  Write-Host "[코어] $Who $text"
  Note "코어 지정 $Who" $text
  return ($got -eq $Mask)
}

# 시작 때 메모리 여유(측정 플랜 2-2절) — 서버 메모리가 늘 때 PC 쪽 여유가 얼마였나를 같이 적는다
$os = Get-CimInstance Win32_OperatingSystem
Note '메모리 여유 — 시작 때(MB)' ('{0:F0} / 전체 {1:F0}' -f ($os.FreePhysicalMemory / 1024.0), ($os.TotalVisibleMemorySize / 1024.0))

$sv = Start-Process -FilePath $serverExe -ArgumentList $serverArgs -WorkingDirectory $repo -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $runDir 'server.txt') -RedirectStandardError (Join-Path $runDir 'server_err.txt')
$null = $sv.Handle   # 끝난 뒤 종료 코드를 읽으려면 핸들을 잡아 둬야 한다
if (-not (Set-Affinity $sv $serverMask '서버')) {
  [void](Stop-Server $sv)
  Write-Host '[멈춤] 서버 코어 지정이 다르다'
  Save-Env
  exit 3
}
Start-Sleep -Seconds 2
if ($sv.HasExited) {
  Write-Host "[멈춤] 서버가 바로 끝났다 — $runDir\server.txt"
  Save-Env
  exit 4
}

$du = Start-Process -FilePath $dummyExe -ArgumentList $dummyArgs -WorkingDirectory $repo -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $runDir 'dummy.txt') -RedirectStandardError (Join-Path $runDir 'dummy_err.txt')
$null = $du.Handle
if (-not (Set-Affinity $du $dummyMask '더미')) {
  $du.Kill()
  [void](Stop-Server $sv)
  Write-Host '[멈춤] 더미 코어 지정이 다르다'
  Save-Env
  exit 3
}
Save-Env   # 실행 중 끊겨도 지문은 남긴다

# 더미가 끝나기를 기다리며 CPU 속도를 10초마다 적는다(측정 플랜 3절 — 검사 문턱이 아니라 결과 옆에 적는 값)
#   cpu_speed.txt 한 줄 = 더미를 띄운 뒤 지난 초 · 실제 MHz · 성능 % · 기준 MHz (지난 줄부터 이 줄까지의 평균)
$cpuLines = New-Object System.Collections.ArrayList
$cpuMhz = New-Object System.Collections.ArrayList
$pdhErr = [NoxLoadRunPdh]::Open()
if ($pdhErr) { [void]$cpuLines.Add("못 읽음 — $pdhErr") }
$duStart = Get-Date
$deadline = $duStart.AddSeconds($Sec + 30)
while (-not $du.HasExited) {
  $left = ($deadline - (Get-Date)).TotalMilliseconds
  if ($left -le 0) { break }
  [void]$du.WaitForExit([int][math]::Min(10000, $left))
  if ($pdhErr) { continue }
  $perf = 0.0; $base = 0.0
  if ([NoxLoadRunPdh]::Read([ref]$perf, [ref]$base)) {
    $mhz = $base * $perf / 100.0
    [void]$cpuMhz.Add($mhz)
    [void]$cpuLines.Add(('{0:F0} · {1:F0} · {2:F1} · {3:F0}' -f ((Get-Date) - $duStart).TotalSeconds, $mhz, $perf, $base))
  } else {
    [void]$cpuLines.Add(('{0:F0} · 못 읽음' -f ((Get-Date) - $duStart).TotalSeconds))
  }
}
if (-not $du.HasExited) {
  Write-Host '[주의] 더미가 시간 안에 안 끝나 강제 종료'
  $du.Kill()
}
[IO.File]::WriteAllLines((Join-Path $runDir 'cpu_speed.txt'), [string[]]$cpuLines, (New-Object Text.UTF8Encoding($true)))
if ($cpuMhz.Count -gt 0) {
  $m = $cpuMhz | Measure-Object -Minimum -Maximum -Average
  Note 'CPU 속도(MHz) — 실행 중 10초 평균의' ('최소 {0:F0} · 평균 {1:F0} · 최대 {2:F0} · {3}줄(cpu_speed.txt)' -f $m.Minimum, $m.Average, $m.Maximum, $cpuMhz.Count)
} else {
  Note 'CPU 속도(MHz) — 실행 중 10초 평균의' $(if ($pdhErr) { "못 읽음 — $pdhErr" } else { '줄 없음' })
}
Start-Sleep -Seconds 1   # 더미가 끝난 뒤 서버가 마지막 입력을 다 받도록
$clean = Stop-Server $sv
[IO.File]::WriteAllText($stamp, ((Get-Date).ToUniversalTime().Ticks).ToString())

Note '서버 종료 코드' "$($sv.ExitCode)"
Note '더미 종료 코드' "$($du.ExitCode)"
Note '서버가 멈춤 이벤트로 끝남' "$clean"
Save-Env
Write-Host "[끝] $runDir · 서버 종료 코드 $($sv.ExitCode) · 더미 종료 코드 $($du.ExitCode)"
exit 0
