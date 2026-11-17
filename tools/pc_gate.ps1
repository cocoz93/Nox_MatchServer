# PC 검사(게이트) — 측정 직전마다 같은 검사를 돌리고, 못 넘으면 부르는 쪽이 서버를 안 띄운다.
#   ★ 11-17 — load_run.ps1(게임 판)에서 떼어 냈다. 로비 판(lobby_load.ps1)도 같은 검사를 부른다 — 두 벌로 두면 어긋난다
#   (`Docs/MO_LOBBY_MEASURE_PLAN_1117.md` 4절 ①). 본문은 load_run.ps1 의 것을 그대로 옮겼고 화면 줄([게이트] …)도 같다.
#   문턱 기본값은 여기 하나다 — load_report.ps1 이 이 파일의 param 기본값을 읽어 env.txt 「문턱」 줄과 견준다.
#   값의 근거 = Docs/MO_DUMMY_PLAN_1109.md 6절 · Docs/MO_LOAD_VARIABLE_PLAN_1108.md 3절.
#
# 부르는 법(같은 프로세스): $g = & (Join-Path $PSScriptRoot 'pc_gate.ps1') -StampPath <직전 실행이 끝난 시각 파일> [문턱 인자 …]
#   돌려줌 = @{ Notes = 지문 줄(「키: 값」 — 부르는 쪽이 env.txt 에 이 차례 그대로 넣는다); Failed = 못 넘은 게이트 이름들; TopText = 상위 프로세스 CPU }
#   부르는 쪽은 문턱 인자를 자기가 받은 것만 넘긴다($PSBoundParameters) — 기본값을 두 대본에 따로 적지 않는다
param(
  [string]$StampPath = '',               # 직전 실행이 끝난 시각(UTC 틱) 파일 — 잔열 게이트와 지문이 읽는다 · 없으면 잔열 건너뜀
  # 게이트 문턱 — 시험 때 일부러 틀리게 줄 수 있도록 인자로 뺐다. 측정에서는 기본값을 쓴다
  [double]$UptimeMaxHours = 18,          # MMO_Zone 실측 — 0.5~18시간 정상 · 26시간 +16% · 105시간 +70~94%
  [double]$IdleMaxPercent = 1,           # 1초씩 3번 전부 이 값 미만
  [double]$ResidualMaxPercent = 2.5,     # 25초마다 읽어 2연속 이 값 미만
  [int]$ResidualMaxReads = 14,           # 25초 × 14 = 350초 — MMO_Zone 실측 잔열 약 4.5분(270초)에 두 번 연속 읽을 몫 50초와 조금을 더한 값
  [string]$PowerPlanGuid = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c',   # 고성능
  [string]$UpdateServiceStatus = 'Stopped',
  # 유휴 CPU 를 못 넘었을 때 다시 재기 — 문턱이 아니라 기다리는 방법이라 load_report 의 문턱 비교에 안 넣는다
  [int]$IdleRetrySec = 30,               # 다시 재기 전에 쉬는 초
  [int]$IdleRetryMax = 0                 # 다시 재는 최대 횟수 · 0 = 넘을 때까지 끝없이(사용자 11-13 · 가동 시간 문턱 안에서) · 시험 때만 준다 · 업데이트 서비스 기다리기도 같은 두 값
)

# 전체 CPU 사용률 — 성능 카운터 이름은 윈도 언어에 따라 달라 GetSystemTimes 로 직접 읽는다
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NoxLoadRunCpu {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
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

$notes = New-Object System.Collections.ArrayList
function Note([string]$Key, [string]$Value) { [void]$notes.Add("$Key`: $Value") }

$failed = New-Object System.Collections.ArrayList
function Gate([string]$Name, [bool]$Ok, [string]$Detail) {
  $mark = if ($Ok) { '통과' } else { '실패' }
  Write-Host "[게이트] $Name — $mark · $Detail"
  Note "게이트 $Name" "$mark · $Detail"
  if (-not $Ok) { [void]$failed.Add($Name) }
}

# 직전 실행이 끝난 뒤 지난 시간
$sinceLast = $null
if ($StampPath -and (Test-Path -LiteralPath $StampPath)) {
  $sinceLast = [math]::Round(((Get-Date).ToUniversalTime().Ticks - [int64](Get-Content -LiteralPath $StampPath -Raw).Trim()) / 1e7, 0)
  Note '직전 실행이 끝난 뒤' "$sinceLast 초"
} else {
  Note '직전 실행이 끝난 뒤' '기록 없음'
}

# ── 게이트 ────────────────────────────────────────────────────
# 이번 실행의 문턱 — load_report.ps1 이 이 대본의 기본값과 견줘 다르면 그 실행을 버린다(측정 플랜 2-3절 · `final4` 는 유휴 문턱을 풀고 돌렸다)
foreach ($n in @('UptimeMaxHours', 'IdleMaxPercent', 'ResidualMaxPercent', 'ResidualMaxReads', 'PowerPlanGuid', 'UpdateServiceStatus')) {
  Note "문턱 $n" ([string](Get-Variable -Name $n -ValueOnly))
}
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$uptime = [math]::Round(((Get-Date) - $boot).TotalHours, 2)
Gate '가동 시간' ($uptime -le $UptimeMaxHours) "$uptime 시간 · 문턱 $UptimeMaxHours 시간 이하"
Note '가동 시간(시간)' "$uptime"

$active = (& powercfg /getactivescheme | Out-String)
$guid = ''
if ($active -match '([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})') { $guid = $Matches[1].ToLower() }
Gate '전원 계획' ($guid -eq $PowerPlanGuid.ToLower()) "지금 $guid · 기대 $PowerPlanGuid"
Note '전원 계획' $active.Trim()

# 기다리는 동안 가동 시간 문턱을 넘으면 그만 기다린다 — 그 뒤에 돌면 가동 시간 게이트가 막았을 실행이다
function Over-Uptime { return ([math]::Round(((Get-Date) - $boot).TotalHours, 2) -gt $UptimeMaxHours) }

# ★ 11-14 — 기대와 다르면 $IdleRetrySec 초마다 다시 본다(유휴 다시 재기와 같은 값 · 가동 시간 문턱 안에서).
#   업데이트 서비스는 「요청 시 시작」이라 SYSTEM 이 켜고 일이 끝나면 스스로 멈춘다 — 측정 전 PC 정리에서 「사용 안 함」으로 둬도
#   약 24분마다 되돌린다. 11-14 03:02 첫째 묶음이 「지금 Running」으로 멈췄다(02:51 검사 땐 Stopped). 끄려면 관리자 권한이라 대본은 기다리기만 한다
$wu = (Get-Service wuauserv).Status.ToString()
$wuTries = 1
while ($wu -ne $UpdateServiceStatus -and ($IdleRetryMax -le 0 -or $wuTries -le $IdleRetryMax) -and -not (Over-Uptime)) {
  Write-Host "[게이트] 업데이트 서비스 — $wuTries 번째 다름 · 지금 $wu · $IdleRetrySec 초 뒤 다시"
  Note "업데이트 서비스 다름 $wuTries" "$wu · $(Get-Date -Format 'HH:mm:ss')"
  Start-Sleep -Seconds $IdleRetrySec
  $wu = (Get-Service wuauserv).Status.ToString()
  $wuTries++
}
Gate '업데이트 서비스' ($wu -eq $UpdateServiceStatus) "지금 $wu · 기대 $UpdateServiceStatus · 본 횟수 $wuTries"
Note '업데이트 서비스' $wu

try {
  $mp = Get-MpComputerStatus
  Note '백신 실시간 검사' "$($mp.RealTimeProtectionEnabled)"
} catch {
  Note '백신 실시간 검사' "못 읽음 — $($_.Exception.Message)"
}

Start-Sleep -Seconds 3   # 대본이 방금 부른 것(컴파일 · git · WMI 조회)이 식기를 기다린다 — 안 그러면 첫 읽기에 대본 자신의 몫이 섞인다
# ★ 11-14 — 못 넘으면 $IdleRetrySec 초 쉬고 다시 잰다 · $IdleRetryMax 0 이면 넘을 때까지 끝없이(사용자 11-13 「계속 무한으로」).
#   1초 순간값 셋이라 한 번 튄 것에도 실패하고, load_series 는 게이트 실패면 묶음을 멈춘다 — 11-13 23:40 첫째 묶음이 5.2 · 0.31 · 0.54% 로 멈췄다.
#   문턱(1%)은 그대로다 — 계속 무거운 것이 있으면 여기서 기다리고, 그것이 끝나면 이어 간다.
#   못 넘을 때마다 **같은 3초 동안** CPU 를 쓴 상위 프로세스를 적는다(아래 Get-TopCpu 는 유휴를 다 잰 뒤 3.5초를 따로 재서
#   튄 그 1초의 범인을 못 봤다 · 그사이 끝난 프로세스는 여기서도 안 보인다)
$idleTries = 0
while ($true) {
  $idleTries++
  $idleBefore = @{}
  foreach ($p in Get-Process) { if ($p.CPU -ne $null) { $idleBefore[$p.Id] = $p.CPU } }
  $idleReads = @(1..3 | ForEach-Object { Read-CpuPercent 1000 })
  $idleOk = @($idleReads | Where-Object { $_ -ge $IdleMaxPercent }).Count -eq 0
  if ($idleOk) { break }
  $idleTop = @(foreach ($p in Get-Process) {
      if ($p.CPU -eq $null) { continue }
      $old = 0.0
      if ($idleBefore.ContainsKey($p.Id)) { $old = $idleBefore[$p.Id] }
      [pscustomobject]@{ Name = $p.ProcessName; Id = $p.Id; Delta = [math]::Round($p.CPU - $old, 2) }
    }) | Sort-Object Delta -Descending | Select-Object -First 5
  $idleTopText = ($idleTop | ForEach-Object { "$($_.Name)($($_.Id)) $($_.Delta)" }) -join ' ; '
  Write-Host "[게이트] 유휴 CPU — $idleTries 번째 못 넘음 · $($idleReads -join ' · ')% · 같은 3초 상위: $idleTopText"
  Note "유휴 CPU 못 넘음 $idleTries" "$($idleReads -join ' ')% · 같은 3초 상위: $idleTopText"
  if ($IdleRetryMax -gt 0 -and $idleTries -gt $IdleRetryMax) { break }
  if (Over-Uptime) { Write-Host "[게이트] 유휴 CPU — 기다리는 동안 가동 시간 $UptimeMaxHours 시간을 넘어 그만 기다림"; break }
  Start-Sleep -Seconds $IdleRetrySec
}
Gate '유휴 CPU' $idleOk "$($idleReads -join ' · ')% · 문턱 $IdleMaxPercent% 미만(1초씩 3번 전부) · 잰 횟수 $idleTries"
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

return @{ Notes = @($notes); Failed = @($failed); TopText = $topText }
