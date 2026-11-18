# 부하 실험 한 회 실행 — PC 검사(게이트) · 환경 지문(env.txt) · 코어 지정 · 서버 + 더미 실행 · 서버 멈춤 이벤트로 끝내기
#   · ★ 11-18 게임 스레드 멈춤 감시(-StallSec · 잡히면 판 폴더에 stack1.txt · stack2.txt · server.dmp · env.txt 「멈춤 감시」)
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
# 종료 코드: 0 = 통과(또는 실행 끝) · 1 = 게이트 실패 · 2 = 실행 파일 · 맵 없음 · 서버 · 더미 코어 인자 · 3 = 코어 지정이 다름 · 4 = 서버가 안 뜸
param(
  [string]$Tag = 'run',
  [int]$Rooms = 1,
  [int]$RoomSize = 10,
  [int]$Sec = 300,
  [int]$Port = 18590,
  [string]$Classes = '0,1',
  [string]$Behavior = 'fight',
  [int]$StatMult = 3,                    # 체력 · 마나 배수 — 11-12 사용자 ×5 → ×3(전사 450 이라 체력 알림 41 의 510 막힘이 안 걸림)
  [double]$FreqMult = 1.0,
  [int]$DummyThreads = 4,                # 더미 내부 스레드(측정 플랜 4절 — 1,280명이면 한 바퀴 최대 약 94ms 라 3스레드가 필요하고 하나 더)
  # ★ 11-17 — 더미 코어(논리 코어 비트). 기본 = 물리 3~9. 더미 한계(`Docs/MO_DUMMY_LIMIT_PLAN_1117.md` 4절)는 스레드 하나를
  #   물리 코어 하나(논리 6 · 7 = 0xC0)에 묶어 잰다. ⚠ 아래 코어 줄에서 같은 이름을 지역 변수로 다시 넣지 말 것(대소문자 안 가림 — 인자가 지워진다)
  [int64]$DummyMask = 0xFFFC0,
  # ★ 11-18 — 서버 코어(논리 코어 비트). 기본 = 물리 0~2(옛 고정값 그대로라 안 주는 대본은 바뀌지 않음). 게임 스레드 3개 판은
  #   무거운 스레드가 넷(게임 셋 · 통신)이라 물리 0~3(0xFF)을 준다 — 값은 tools/load_limit.ps1 Core-Split. ⚠ 아래에서 같은 이름을 지역 변수로 다시 넣지 말 것
  [int64]$ServerMask = 0x3F,
  [int]$CycleSec = 0,                   # 방 여닫기(측정 플랜 M7) — 더미 --cycle-sec 로 넘긴다. 마지막 방이 이 초마다 닫히고 새로 열린다 · 0 = 끔
  [switch]$NoDummy,                      # 빈 서버(측정 플랜 M2) — 더미를 안 띄우고 서버만 Sec 초 돌린다. 방은 더미가 들어와야 생겨 0개다
  [string]$ExtraServerArgs = '',
  [string]$ExtraDummyArgs = '',
  [string]$ServerExe = '',               # 비우면 build\Release\nox_server.exe · 최적화 바퀴의 「고치기 전 서버」 사본을 띄울 때(tools/load_compare.ps1 · 11-14)
  # ★ 11-18 멈춤 감시(M-1 · tools/stall_watch.ps1) — 한 게임 스레드의 [구간] 초가 이 초 동안 안 바뀌면 nox_stack 두 묶음 + 덤프를 판 폴더에 남기고
  #   판을 일찍 끝낸다(종료 코드는 그대로 0 — 부르는 대본은 그 판을 「버림」으로 보고 다시 잰다). 값 = stall_hunt.ps1 $StallSec 과 같음 · 0 = 끔
  [int]$StallSec = 10,
  [string]$FakeServerArgs = '',          # 시험용(tools/stall_watch_test.ps1) — 주면 서버 = nox_stack --victim fakeserver 이것 · 더미 = nox_stack --victim sleep · 측정에서는 안 준다
  [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Nox',
  [switch]$CheckOnly,
  # 게이트 문턱 · 다시 재기 — ★ 11-17 기본값과 뜻은 tools/pc_gate.ps1 하나(로비 판과 같이 쓰려고 떼어 냄). 여기선 받은 것만 넘긴다
  #   (시험 때 일부러 틀리게 · load_compare 가 가동 시간 문턱을 풂). 측정에서는 안 준다
  [double]$UptimeMaxHours, [double]$IdleMaxPercent, [double]$ResidualMaxPercent, [int]$ResidualMaxReads,
  [string]$PowerPlanGuid, [string]$UpdateServiceStatus, [int]$IdleRetrySec, [int]$IdleRetryMax
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# ⚠ PowerShell 변수 이름은 대소문자를 안 가린다 — 인자 `$ServerExe` 와 지역 변수 `$serverExe` 는 **한 변수**라, 기본 경로를 먼저 넣으면
#   인자가 지워진다(11-14 바퀴 1 「고치기 전」 판이 실제로는 build\Release 를 띄웠다 · 그 판 env.txt 「서버 명령줄」). 지역 이름을 따로 둔다
$srvExePath = if (-not $ServerExe) { Join-Path $repo 'build\Release\nox_server.exe' }
              elseif (Test-Path -LiteralPath $ServerExe) { (Resolve-Path -LiteralPath $ServerExe).Path }
              else { $ServerExe }   # 없으면 아래 검사가 「[멈춤] 없음」으로 멈춘다
$dummyExe = Join-Path $repo 'build\Release\nox_dummy.exe'
$stackExe = Join-Path $repo 'build\Release\nox_stack.exe'   # ★ 11-18 멈춤 감시 — 없으면 감시만 끄고 판은 돈다
if ($FakeServerArgs) { $srvExePath = $stackExe; $dummyExe = $stackExe }
. (Join-Path $PSScriptRoot 'stall_watch.ps1')
$map = Join-Path $GameDir 'maps\So_LOD\So_LOD.map'
$runRoot = Join-Path $repo '_out\load_run'
$stamp = Join-Path $runRoot 'last_end.txt'   # 직전 실행이 끝난 시각(UTC 틱) — 잔열 게이트와 지문이 읽는다

# 코어 — 이 PC(i9-10900)는 물리 코어 k 가 논리 2k · 2k+1 이다. 서버 = 물리 0~2 · 더미 = 물리 3~9(인자 -ServerMask · -DummyMask 기본값)
#   ★ 11-18 옛 `$serverMask = 0x3F` 줄을 뺐다 — 인자 -ServerMask 와 한 변수라 남기면 인자가 지워진다
if ($ServerMask -le 0 -or ($ServerMask -band -bnot [int64]0xFFFFF) -ne 0) {
  Write-Host ('[멈춤] -ServerMask 0x{0:X} — 논리 0~19 밖' -f $ServerMask); exit 2
}
if (($DummyMask -band $ServerMask) -ne 0 -or $DummyMask -le 0 -or ($DummyMask -band -bnot [int64]0xFFFFF) -ne 0) {
  Write-Host ('[멈춤] -DummyMask 0x{0:X} — 서버 코어(0x{1:X})와 겹치거나 논리 0~19 밖' -f $DummyMask, $ServerMask); exit 2
}

foreach ($p in @($srvExePath, $dummyExe, $map)) {
  if (-not (Test-Path -LiteralPath $p)) { Write-Host "[멈춤] 없음: $p"; exit 2 }
}

New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$runDir = Join-Path $runRoot ((Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + $Tag)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

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

$env_lines = New-Object System.Collections.ArrayList
function Note([string]$Key, [string]$Value) { [void]$env_lines.Add("$Key`: $Value") }
function Save-Env { [IO.File]::WriteAllLines((Join-Path $runDir 'env.txt'), [string[]]$env_lines, (New-Object Text.UTF8Encoding($true))) }

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

# ── 게이트 ── ★ 11-17 tools/pc_gate.ps1 로 떼어 냄(로비 판도 같은 검사 · `Docs/MO_LOBBY_MEASURE_PLAN_1117.md` 4절 ①).
#   본문 · 화면 줄은 옛 그대로. 문턱 인자는 받은 것만 넘기고, 지문 줄은 돌려받아 이 차례 그대로 env.txt 에 넣는다
$gateArgs = @{ StampPath = $stamp }
foreach ($n in @('UptimeMaxHours', 'IdleMaxPercent', 'ResidualMaxPercent', 'ResidualMaxReads', 'PowerPlanGuid', 'UpdateServiceStatus', 'IdleRetrySec', 'IdleRetryMax')) {
  if ($PSBoundParameters.ContainsKey($n)) { $gateArgs[$n] = $PSBoundParameters[$n] }
}
$gate = & (Join-Path $PSScriptRoot 'pc_gate.ps1') @gateArgs
foreach ($ln in @($gate.Notes)) { [void]$env_lines.Add($ln) }
$failed = @($gate.Failed); $topText = $gate.TopText

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
if ($FakeServerArgs) { $serverArgs = "--victim fakeserver $FakeServerArgs"; $dummyArgs = "--victim sleep --sec $Sec" }
Note '서버 명령줄' "$srvExePath $serverArgs"
Note '더미 명령줄' $(if ($NoDummy) { '없음(-NoDummy)' } else { "$dummyExe $dummyArgs" })
# ★ 11-18 멈춤 감시 — 잡히면 .pdb 를 판 폴더에 복사해 이름을 푼다. 시작 때 해시와 견줘 판 도중 다시 빌드했는지 가른다
$srvPdb = [IO.Path]::ChangeExtension($srvExePath, '.pdb')
$srvPdbHash = Get-ShortHash $srvPdb
Note '서버 실행 파일 해시' "$(Get-ShortHash $srvExePath) · .pdb $srvPdbHash"

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

# ★ 11-15 — 판 도중 업데이트. 일시 중지(11-04~12-09)를 걸어도 백신 정의 · Store 앱 업데이트가 업데이트 서비스를 부른다
#   (11-15 10:05:50 Store 내려받기 둘이 288방 판 도중). 위 업데이트 서비스 게이트는 판 시작 때만 봐서 판 도중을 못 본다 —
#   판 동안 서비스 상태를 10초마다 적고, 끝난 뒤 그 시간의 업데이트 기록(WindowsUpdateClient/Operational)을 센다.
#   기록(찾기 · 받기 · 설치)이 있으면 load_report 가 버린다. 서비스가 켜져 있던 것만으로는 안 버리고 적기만 한다 —
#   11-15 판 넷 중 둘(288 · 304방)이 서비스가 켜진 채 끝났는데 둘 다 가장 느린 1% 1ms 통과였고, 켜짐만으로 버리면 load_limit 이 같은 지점 버림 3번에 멈춘다
Add-Type -AssemblyName System.ServiceProcess
$runWallStart = Get-Date
$wuSc = New-Object System.ServiceProcess.ServiceController 'wuauserv'
$wuSeen = 0; $wuOn = 0; $wuErr = 0; $wuOnAt = New-Object System.Collections.ArrayList

$sv = Start-Process -FilePath $srvExePath -ArgumentList $serverArgs -WorkingDirectory $repo -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $runDir 'server.txt') -RedirectStandardError (Join-Path $runDir 'server_err.txt')
$null = $sv.Handle   # 끝난 뒤 종료 코드를 읽으려면 핸들을 잡아 둬야 한다
if (-not (Set-Affinity $sv $ServerMask '서버')) {
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

$du = $null
if (-not $NoDummy) {
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
}
Save-Env   # 실행 중 끊겨도 지문은 남긴다

# 더미가 끝나기를 기다리며 CPU 속도를 10초마다 적는다(측정 플랜 3절 — 검사 문턱이 아니라 결과 옆에 적는 값)
#   cpu_speed.txt 한 줄 = 더미를 띄운 뒤 지난 초 · 실제 MHz · 성능 % · 기준 MHz (지난 줄부터 이 줄까지의 평균)
#   -NoDummy 면 Sec 초를 채우거나 서버가 먼저 끝날 때까지
$cpuLines = New-Object System.Collections.ArrayList
$cpuMhz = New-Object System.Collections.ArrayList
$pdhErr = [NoxLoadRunPdh]::Open()
if ($pdhErr) { [void]$cpuLines.Add("못 읽음 — $pdhErr") }
$duStart = Get-Date
$deadline = $duStart.AddSeconds($(if ($NoDummy) { $Sec } else { $Sec + 30 }))
# ★ 11-18 멈춤 감시 — 10초 기다릴 때마다 server.txt 새 줄을 읽는다. 기다림이 10초라 실제로는 [구간] 줄이 한 번 기다리는 동안 하나도 안 와야
#   잡힌다(문턱과 기다림 중 큰 쪽). 정상 판에서 [구간] 줄이 1초 넘게 끊긴 적 0번 · 11-18 멈춘 판은 96초 넘게 멈춤(stall_hunt.ps1 $StallSec 주석)
$watch = $null; $caught = $null
if ($StallSec -gt 0) {
  if (Test-Path -LiteralPath $stackExe) { $watch = New-StallWatch (Join-Path $runDir 'server.txt') $StallSec }
  else { Write-Host "[주의] 멈춤 감시 끔 — 없음: $stackExe" }
}
$watchClock = [Diagnostics.Stopwatch]::StartNew()
while ($(if ($NoDummy) { -not $sv.HasExited } else { -not $du.HasExited })) {
  $left = ($deadline - (Get-Date)).TotalMilliseconds
  if ($left -le 0) { break }
  $waitMs = [int][math]::Min(10000, $left)
  if ($NoDummy) { [void]$sv.WaitForExit($waitMs) } else { [void]$du.WaitForExit($waitMs) }
  if ($watch -and -not $sv.HasExited) {   # 서버가 죽어도 [구간] 이 끊긴다 — 그건 멈춤이 아니다(종료 코드가 따로 남는다)
    Update-StallWatch $watch $watchClock.Elapsed.TotalSeconds
    $caught = Get-StallVerdict $watch $watchClock.Elapsed.TotalSeconds
    if ($caught) { break }
  }
  try {
    $wuSc.Refresh(); $wuSeen++
    if ($wuSc.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) { $wuOn++; [void]$wuOnAt.Add([int]((Get-Date) - $duStart).TotalSeconds) }
  } catch { $wuErr++ }
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
if ($caught) {
  $atSec = ((Get-Date) - $duStart).TotalSeconds
  Write-Host ('[멈춤 잡힘] 게임 스레드 {0} · 마지막 [구간] 초 {1} · 바뀐 뒤 {2:F0}초 — 호출 사슬 두 묶음 + 덤프를 남긴다' -f $caught.Thread, $caught.Sec, $caught.Since)
  $pdbNow = '없음'
  if (Test-Path -LiteralPath $srvPdb) { Copy-Item -LiteralPath $srvPdb -Destination $runDir; $pdbNow = Get-ShortHash (Join-Path $runDir (Split-Path -Leaf $srvPdb)) }
  $snap = Invoke-StallSnap $stackExe $sv.Id $runDir "$runDir;$(Split-Path -Parent $srvExePath)"   # 판 폴더에 복사한 .pdb 먼저
  Note '멈춤 감시' ('잡힘 · 게임 스레드 {0} · 마지막 [구간] 초 {1} · 바뀐 뒤 {2:F0}초 · 더미 띄운 뒤 {3:F0}초 · 문턱 {4}초 · {5} · 판 폴더 .pdb {6}(시작 때 {7})' -f
                   $caught.Thread, $caught.Sec, $caught.Since, $atSec, $StallSec, (Get-StallLines $watch), $pdbNow, $srvPdbHash)
  Note '멈춤 호출 사슬 1' $snap[0]
  Note '멈춤 호출 사슬 2' $snap[1]
  Write-Host "[멈춤 잡힘] $($snap[0])"
  if ($du -and -not $du.HasExited) { $du.Kill() }   # 아래 「시간 안에 안 끝나」 줄과 가르려고 여기서
  Save-Env
} elseif ($watch) {
  Note '멈춤 감시' "안 잡힘 · 문턱 ${StallSec}초 · $(Get-StallLines $watch)"
} else {
  Note '멈춤 감시' $(if ($StallSec -le 0) { '끔(-StallSec 0)' } else { "끔 — 없음: $stackExe" })
}
if ($du -and -not $du.HasExited) {
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

# 판 도중 업데이트 기록 — 서버를 띄우기 직전부터 멈춘 뒤까지. 「0건」과 「못 읽음」을 가른다(기록이 없으면 Get-WinEvent 가 오류를 낸다)
$runWallEnd = Get-Date
$wuEvText = $null
try {
  $wuEv = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-WindowsUpdateClient/Operational'; StartTime = $runWallStart; EndTime = $runWallEnd } -ErrorAction Stop)
} catch {
  if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') { $wuEv = @() } else { $wuEv = $null; $wuEvText = "못 읽음 — $($_.Exception.Message)" }
}
if ($null -ne $wuEv) {
  $wuItems = @($wuEv | Sort-Object TimeCreated | ForEach-Object { '{0:HH:mm:ss} id={1} {2}' -f $_.TimeCreated, $_.Id, [string]$_.Properties[0].Value })
  $wuEvText = "$($wuEv.Count)건 · 본 시간 $($runWallStart.ToString('HH:mm:ss'))~$($runWallEnd.ToString('HH:mm:ss'))" + $(if ($wuItems.Count) { ' · ' + ($wuItems -join ' ; ') } else { '' })
}
Note '업데이트 기록 — 판 도중' $wuEvText
Note '업데이트 서비스 — 판 도중 10초 표본' ("켜짐 $wuOn / 본 횟수 $wuSeen · 못 읽음 $wuErr" + $(if ($wuOnAt.Count) { ' · 켜진 때(더미 띄운 뒤 초) ' + (@($wuOnAt | Select-Object -First 30) -join ' ') } else { '' }))
if (-not $wuEvText.StartsWith('0건')) { Write-Host "[주의] 판 도중 업데이트 기록 — $wuEvText" }
if ($wuOn -gt 0) { Write-Host "[주의] 판 도중 업데이트 서비스 켜짐 — 10초 표본 $wuSeen 번 중 $wuOn 번(버림은 아님)" }

Note '서버 종료 코드' "$($sv.ExitCode)"
$duCode = if ($du) { "$($du.ExitCode)" } else { '없음' }
Note '더미 종료 코드' $duCode
Note '서버가 멈춤 이벤트로 끝남' "$clean"
Save-Env
Write-Host "[끝] $runDir · 서버 종료 코드 $($sv.ExitCode) · 더미 종료 코드 $duCode"
exit 0
