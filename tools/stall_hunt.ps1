# M-1 게임 스레드 멈춤 잡기 — 416방 서버 두 대를 동시에 끊지 않고 돌리다가, 한 서버의 [구간] 줄이 10초 끊기면
#   nox_stack 으로 모든 스레드의 호출 사슬 · CPU 몫을 두 번 찍고 덤프를 남긴 뒤 끝낸다(Docs/MO_STALL_HUNT_PLAN_1118.md 3절 ②)
#
#   왜 이 대본이 있나: 멈춤은 서버 한 대 약 1.6시간에 1번(짐작 · 한 번 난 것으로 셈)이라 사람이 지켜보다 찍을 수 없다.
#   멈춘 판과 같은 실행 파일을 build\Release 에서 그대로 두 대 띄워 기다리는 시간을 반으로 줄인다 — 복사해 띄우지 않는 까닭은
#   실행 파일 자리로 길을 정하는 곳이 셋이라서(main.cpp:911 · CrashDump.h:280 · WorldBuild.cpp:62-72).
#   대신 시작과 끝에 해시를 적고 .pdb 를 판 폴더에 복사해 그것으로 이름을 푼다(판 도중 누가 빌드해도 짝이 맞게).
#   성능 판이 아니라 PC 검사는 안 한다 — 절전 · 업데이트 서비스 상태만 적는다.
#
# 사용:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stall_hunt.ps1             # 본 판(상한 4시간)
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stall_hunt.ps1 -SelfTest   # 가짜 서버(nox_stack --victim)로 네 갈래
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stall_hunt.ps1 -OnePair    # 멈춘 판 그대로 쌍 하나(서버 물리 3 · 더미 6스레드 · 입장 0.2초)
#
# 종료 코드: 0 = 끝(어느 갈래로 끝났는지는 hunt.txt) · 2 = 실행 파일 · 맵 없음 · 3 = 코어 지정이 다름 · 4 = 서버가 바로 끝남 · 10 = 자가 시험 틀림
param(
  [int]$Rooms = 416,
  [int]$CapSec = 14400,       # 상한 4시간(플랜 9절 H-1) — 서버 8시간이면 빈도가 짐작대로(서버 한 대 시간당 0.62번)일 때 못 잡을 확률 약 0.7%
  [int]$StallSec = 10,        # 정상 두 판에서 [구간] 줄이 1초 넘게 끊긴 적 0번 · 가장 긴 틱 323ms → 그 30배. 실제 멈춤은 적어도 약 96초
  [int]$EarlySec = 60,        # 더미가 상한보다 이만큼 넘게 일찍 끝나면 「더미 먼저 끝남」
  [int]$JoinMs = 50,          # 입장 간격 — 4,160명 × 0.2초 = 832초를 208초로(측정 판이 아니라 다 찼는지만 본다 · 플랜 4절)
  [int]$DummyThreads = 3,     # 4,160명 ÷ 3 = 1,387명 — 더미 한 스레드 몫 2,100명 안
  [string]$Repo = '',         # 비우면 이 대본의 위 폴더 · 일부러 틀리기 사본이 원래 리포를 가리키게
  [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Nox',
  [switch]$OnePair,           # 본 판만 — 아래 $onePairDef 한 쌍 · 스레드 · 입장 간격을 따로 안 주면 멈춘 판 값
  [switch]$SelfTest
)
if ($OnePair) {
  if (-not $PSBoundParameters.ContainsKey('DummyThreads')) { $DummyThreads = 6 }   # 멈춘 판 --threads 6
  if (-not $PSBoundParameters.ContainsKey('JoinMs')) { $JoinMs = 200 }             # 멈춘 판은 입장 간격을 안 줘 더미 기본값(dummy.cpp:206)
}

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$rel = Join-Path $Repo 'build\Release'
$stackExe = Join-Path $rel 'nox_stack.exe'
$enc = New-Object Text.UTF8Encoding($true)
$sectionRx = [regex]'\[구간\] 초 (\d+)'

# 쌍 — 이 PC(i9-10900)는 물리 코어 k = 논리 2k · 2k+1(tools/load_run.ps1:53). 서버 물리 2(게임 · 통신 스레드에 하나씩) · 더미 물리 3, 두 쌍이 안 겹친다
$pairDefs = @(
  @{ Name = 'A'; SrvMask = [int64]0xF;  DumMask = [int64]0x3F00;  Port = 18590 },
  @{ Name = 'B'; SrvMask = [int64]0xF0; DumMask = [int64]0xFC000; Port = 18591 })
# -OnePair = 멈춘 판(_out/load_run/20261118_032106_LIM_p30_r416_t1 env.txt)의 코어 그대로 — 서버 물리 0~2 · 더미 물리 3~9
#   왜: 11-18 13:35 두 쌍 판(_out/stall_hunt/20261118_133543)에서 서버 물리 2개로는 통신 스레드 바쁨이 100%(멈춘 판 약 78%)에 닿아
#   보낼 요청이 큐에서 버려지고(1초 최대 3만 4천여 건 · 멈춘 판 0) 덜 참 방이 1 → 57로 늘었다 — 멈춘 판과 같은 짐이 아니라 기다림을 반으로 줄이는 쪽을 접는다.
#   서버 한 대 4시간이면 빈도가 짐작대로(시간당 0.62번)일 때 못 잡을 확률 약 8%
$onePairDef = @{   # ⚠ 이름 — PowerShell 변수는 대소문자를 안 가려 $onePair 면 스위치 $OnePair 와 한 변수(11-18 첫 시험)
              Name = 'A'; SrvMask = [int64]0x3F; DumMask = [int64]0xFFFC0; Port = 18590 }

function Get-Hash($p) {
  if (Test-Path -LiteralPath $p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 16) }
  return '없음'
}

function Write-Log($line) {
  Write-Host $line
  [IO.File]::AppendAllText($script:huntTxt, $line + "`r`n", $enc)   # 줄마다 바로 쓴다 — 대본이 도중에 끊겨도 남게
}

function Start-One($exe, $argLine, $dir, $name, [int64]$mask) {
  $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $Repo -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $dir "$name.txt") -RedirectStandardError (Join-Path $dir "$name`_err.txt")
  $null = $p.Handle   # 끝난 뒤 종료 코드를 읽으려면 핸들을 잡아 둬야 한다
  $got = -1
  try { $p.ProcessorAffinity = [IntPtr]$mask; $p.Refresh(); $got = [int64]$p.ProcessorAffinity } catch { }
  return @{ Proc = $p; MaskOk = ($got -eq $mask); MaskText = ('0x{0:X} (기대 0x{1:X})' -f $got, $mask) }
}

function Stop-Server($Proc) {
  # 멈춤 이벤트 먼저(콘솔 프로그램은 창 닫기 신호를 못 받는다) — 강제 종료는 15초 안에 응답이 없을 때만(tools/load_run.ps1:164-179 꼴).
  #   멈춘 서버는 끝내기 신호를 고리 맨 위에서만 읽어(GameThread.cpp:111) 못 듣는다 — 그래서 잡힘 갈래에선 강제 종료가 하나 난다
  if ($Proc.HasExited) { return $true }
  $name = "nox_server_stop_$($Proc.Id)"
  $ev = $null
  for ($i = 0; $i -lt 50 -and $null -eq $ev -and -not $Proc.HasExited; $i++) {
    try { $ev = [System.Threading.EventWaitHandle]::OpenExisting($name) } catch { Start-Sleep -Milliseconds 200 }
  }
  if ($null -ne $ev) { [void]$ev.Set(); $ev.Dispose() } elseif (-not $Proc.HasExited) { Write-Log "[주의] 멈춤 이벤트를 못 찾음: $name" }
  if ($Proc.WaitForExit(15000)) { return $true }
  $Proc.Kill()
  [void]$Proc.WaitForExit(5000)
  return $false
}

# server.txt 에서 지난번 읽은 자리부터 새로 붙은 바이트만 읽어 그 안의 마지막 [구간] 초 값을 잡는다.
#   파일 끝 몇 KB만 읽지 않는 까닭: [구간] 한 줄 약 850바이트 · [판 결과] 한 줄 약 220바이트라 판 끝이 몰리면 끝 몇 KB 안에 [구간] 줄이 없어 헛잡힌다.
#   줄 끝(\n)까지만 읽은 것으로 친다 — 덜 쓴 줄과 한글 바이트가 잘리지 않게 다음에 다시 읽는다
function Read-New($st) {
  if (-not (Test-Path -LiteralPath $st.ServerTxt)) { return }
  $fs = [IO.File]::Open($st.ServerTxt, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  try {
    $len = $fs.Length
    if ($len -le $st.Pos) { return }
    $n = [int][math]::Min($len - $st.Pos, 16MB)
    $buf = New-Object byte[] $n
    [void]$fs.Seek($st.Pos, [IO.SeekOrigin]::Begin)
    $got = $fs.Read($buf, 0, $n)
  } finally { $fs.Dispose() }
  if ($got -le 0) { return }
  $end = [Array]::LastIndexOf($buf, [byte]10, $got - 1)
  if ($end -lt 0) { return }
  $st.Pos += $end + 1
  $st.Bytes += $end + 1
  $ms = $sectionRx.Matches([Text.Encoding]::UTF8.GetString($buf, 0, $end + 1))
  $st.Lines += $ms.Count
  if ($ms.Count -gt 0) {
    $sec = [int]$ms[$ms.Count - 1].Groups[1].Value
    if ($sec -ne $st.LastSec) { $st.LastSec = $sec; $st.ChangedAt = $script:clock.Elapsed.TotalSeconds }
  }
}

# 끝나는 갈래 — 값을 돌려주는 함수 하나(갈래 넷 + 계속). 차례가 뜻이다: 서버가 죽으면 [구간]도 끊기니 죽음을 먼저 본다
function Get-Branch($states, [double]$t, [int]$cap, [int]$early) {
  foreach ($st in $states) { if ($st.Server.HasExited) { return @{ Kind = '서버 죽음'; Pair = $st } } }
  foreach ($st in $states) { if ($st.LastSec -ge 0 -and ($t - $st.ChangedAt) -ge $StallSec) { return @{ Kind = '잡힘'; Pair = $st } } }
  foreach ($st in $states) { if ($st.Dummy.HasExited -and $t -lt $cap - $early) { return @{ Kind = '더미 먼저 끝남'; Pair = $st } } }
  if ($t -ge $cap) { return @{ Kind = '상한'; Pair = $null } }
  return @{ Kind = '계속'; Pair = $null }
}

function Get-Summary($path) {
  if (-not (Test-Path -LiteralPath $path)) { return '못 읽음 — 파일 없음' }
  $l = @([IO.File]::ReadAllLines($path) | Where-Object { $_.StartsWith('[요약]') -or $_.StartsWith('[덤프]') })
  if ($l.Count) { return ($l -join ' / ') }
  return '못 읽음 — [요약] 줄 없음'
}

# 한 판 — 쌍 둘을 띄워 갈래 하나로 끝날 때까지. 돌려주는 값 = 갈래 · 강제 종료 목록 · 쌍별 읽은 [구간] 줄 수
function Invoke-Hunt($dir, $specs, [int]$cap, [int]$early, $pdbSrc) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $script:huntTxt = Join-Path $dir 'hunt.txt'
  $script:clock = [Diagnostics.Stopwatch]::StartNew()
  $kills = New-Object System.Collections.ArrayList
  $pdbCopy = Join-Path $dir (Split-Path -Leaf $pdbSrc)
  Copy-Item -LiteralPath $pdbSrc -Destination $pdbCopy
  $exes = @($specs[0].ServerExe, $pdbSrc, $specs[0].DummyExe, $stackExe)
  $hash0 = ($exes | ForEach-Object { '{0} {1}' -f (Split-Path -Leaf $_), (Get-Hash $_) }) -join ' · '
  Write-Log ('[시작] {0:yyyy-MM-dd HH:mm:ss} · 상한 {1}초 · 멈춤 문턱 {2}초 · 더미 일찍 끝남 {3}초' -f (Get-Date), $cap, $StallSec, $early)
  Write-Log "[해시] $hash0 · 판 폴더 .pdb $(Get-Hash $pdbCopy)"
  $standby = @(powercfg /query SCHEME_CURRENT SUB_SLEEP STANDBYIDLE | Where-Object { $_ -match '0x[0-9a-fA-F]{8}\s*$' } | ForEach-Object { $_.Trim() })
  Write-Log "[절전] $($standby -join ' · ') · [업데이트 서비스] $((Get-Service wuauserv).Status)"

  $states = @()
  foreach ($s in $specs) {
    $pd = Join-Path $dir $s.Name
    New-Item -ItemType Directory -Force -Path $pd | Out-Null
    $states += @{ Name = $s.Name; Dir = $pd; Spec = $s; ServerTxt = (Join-Path $pd 'server.txt'); Pos = [int64]0; Bytes = [int64]0; Lines = 0
                  LastSec = -1; ChangedAt = 0.0; Server = $null; Dummy = $null }
  }
  $fail = $null
  foreach ($st in $states) {
    $r = Start-One $st.Spec.ServerExe $st.Spec.ServerArgs $st.Dir 'server' $st.Spec.SrvMask
    $st.Server = $r.Proc
    Write-Log "[쌍 $($st.Name)] 서버 pid $($r.Proc.Id) · 코어 $($r.MaskText) · $($st.Spec.ServerExe) $($st.Spec.ServerArgs)"
    if (-not $r.MaskOk) { $fail = 3 }
  }
  Start-Sleep -Seconds 2
  foreach ($st in $states) { if ($st.Server.HasExited) { Write-Log "[멈춤] 서버 $($st.Name)가 바로 끝남(코드 $($st.Server.ExitCode)) — $($st.ServerTxt)"; $fail = 4 } }
  if (-not $fail) {
    foreach ($st in $states) {
      $r = Start-One $st.Spec.DummyExe $st.Spec.DummyArgs $st.Dir 'dummy' $st.Spec.DumMask
      $st.Dummy = $r.Proc
      Write-Log "[쌍 $($st.Name)] 더미 pid $($r.Proc.Id) · 코어 $($r.MaskText) · $($st.Spec.DummyExe) $($st.Spec.DummyArgs)"
      if (-not $r.MaskOk) { $fail = 3 }
    }
  }
  if ($fail) {
    foreach ($st in $states) {
      if ($st.Dummy -and -not $st.Dummy.HasExited) { $st.Dummy.Kill(); [void]$kills.Add("더미 $($st.Name)") }
      if ($st.Server -and -not (Stop-Server $st.Server)) { [void]$kills.Add("서버 $($st.Name)") }
    }
    Write-Log "[끝] 준비 실패(코드 $fail) · 강제 종료 $($kills.Count)$(if ($kills.Count) { ' — ' + ($kills -join ' · ') })"
    return @{ Kind = '준비 실패'; Code = $fail; Kills = $kills; States = $states }
  }

  # 감시 — 2초마다 새로 붙은 줄을 읽고 갈래를 본다 · 1분마다 watch.txt 에 한 줄(읽은 [구간] 줄 수 = 「0 = 못 읽음」을 가르는 지나간 양)
  $watchTxt = Join-Path $dir 'watch.txt'
  $nextWatch = 60
  while ($true) {
    foreach ($st in $states) { Read-New $st }
    $t = $script:clock.Elapsed.TotalSeconds
    $b = Get-Branch $states $t $cap $early
    if ($t -ge $nextWatch -or $b.Kind -ne '계속') {
      $w = ('{0:F0}초 · ' -f $t) + (($states | ForEach-Object { '{0} 초 {1} · 줄 {2} · {3:F0}KB' -f $_.Name, $_.LastSec, $_.Lines, ($_.Bytes / 1KB) }) -join ' · ')
      [IO.File]::AppendAllText($watchTxt, $w + "`r`n", $enc)
      $nextWatch += 60
    }
    if ($b.Kind -ne '계속') { break }
    Start-Sleep -Seconds 2
  }

  $p = $b.Pair
  $others = @($states | Where-Object { $_ -ne $p })
  switch ($b.Kind) {
    '잡힘' {
      Write-Log ('[갈래] 잡힘 · 쌍 {0} · 마지막 [구간] 초 {1} · 바뀐 뒤 {2:F0}초 · 대본 시계 {3:F0}초' -f $p.Name, $p.LastSec, ($t - $p.ChangedAt), $t)
      $sym = "$dir;$(Split-Path -Parent $p.Spec.ServerExe)"   # 판 폴더에 복사한 .pdb 먼저
      $s1 = Join-Path $p.Dir 'stack1.txt'; $s2 = Join-Path $p.Dir 'stack2.txt'
      & $stackExe --pid $p.Server.Id --samples 5 --gap-ms 200 --out $s1 --sym $sym | Out-Null
      $rc1 = $LASTEXITCODE
      Start-Sleep -Seconds 10   # 둘째 묶음도 같은 함수면 「오래 걸리는 일」이 아니라 「갇힘」
      & $stackExe --pid $p.Server.Id --samples 5 --gap-ms 200 --out $s2 --dump (Join-Path $p.Dir 'server.dmp') --sym $sym | Out-Null
      $rc2 = $LASTEXITCODE
      Write-Log "[요약 1] 코드 $rc1 · $(Get-Summary $s1)"
      Write-Log "[요약 2] 코드 $rc2 · $(Get-Summary $s2)"
      if (-not (Stop-Server $p.Server)) { [void]$kills.Add("서버 $($p.Name)") }
      foreach ($o in $others) { if (-not (Stop-Server $o.Server)) { [void]$kills.Add("서버 $($o.Name)") } }
      foreach ($st in $states) { if (-not $st.Dummy.HasExited) { $st.Dummy.Kill(); [void]$kills.Add("더미 $($st.Name)") } }
    }
    '서버 죽음' {
      $crashDir = Join-Path (Split-Path -Parent $p.Spec.ServerExe) 'CrashDump'
      $crash = @(Get-ChildItem -LiteralPath $crashDir -Filter '*.dmp' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge (Get-Date).AddSeconds(-$t) } |
                 Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { $_.FullName })
      Write-Log ('[갈래] 서버 죽음 · 쌍 {0} · 종료 코드 {1} · 마지막 [구간] 초 {2} · 대본 시계 {3:F0}초 · 크래시 덤프 {4}' -f $p.Name, $p.Server.ExitCode, $p.LastSec, $t,
                 $(if ($crash.Count) { $crash[0] } else { '이 판에 새로 생긴 것 없음' }))
      foreach ($o in $others) { if (-not (Stop-Server $o.Server)) { [void]$kills.Add("서버 $($o.Name)") } }
      foreach ($st in $states) { if (-not $st.Dummy.HasExited) { $st.Dummy.Kill(); [void]$kills.Add("더미 $($st.Name)") } }
    }
    '더미 먼저 끝남' {
      $dl = @(Get-Content -LiteralPath (Join-Path $p.Dir 'dummy.txt') -Encoding UTF8 -Tail 1 -ErrorAction SilentlyContinue)
      Write-Log ('[갈래] 더미 먼저 끝남 · 쌍 {0} · 종료 코드 {1} · 대본 시계 {2:F0}초 · 마지막 줄 {3}' -f $p.Name, $p.Dummy.ExitCode, $t, ($dl -join ''))
      foreach ($st in $states) { if (-not (Stop-Server $st.Server)) { [void]$kills.Add("서버 $($st.Name)") } }
      foreach ($st in $states) { if (-not $st.Dummy.HasExited) { $st.Dummy.Kill(); [void]$kills.Add("더미 $($st.Name)") } }
    }
    '상한' {
      Write-Log ('[갈래] 상한 · 대본 시계 {0:F0}초 · 멈춤 0번 — 더미가 스스로 끝나기를 2분까지 기다린 뒤 서버 끝내기' -f $t)
      foreach ($st in $states) { [void]$st.Dummy.WaitForExit(120000) }   # 더미는 --duration-sec 상한으로 띄워 스스로 끝난다
      foreach ($st in $states) { if (-not $st.Dummy.HasExited) { $st.Dummy.Kill(); [void]$kills.Add("더미 $($st.Name)") } }
      foreach ($st in $states) { if (-not (Stop-Server $st.Server)) { [void]$kills.Add("서버 $($st.Name)") } }
    }
  }
  $hash1 = ($exes | ForEach-Object { '{0} {1}' -f (Split-Path -Leaf $_), (Get-Hash $_) }) -join ' · '
  Write-Log "[끝 해시] $(if ($hash1 -eq $hash0) { '시작과 같음' } else { '시작과 다름 — ' + $hash1 })"
  Write-Log ('[끝] {0:yyyy-MM-dd HH:mm:ss} · 갈래 {1} · 강제 종료 {2}{3} · 읽은 [구간] 줄 {4}' -f (Get-Date), $b.Kind, $kills.Count,
             $(if ($kills.Count) { ' — ' + ($kills -join ' · ') } else { '' }), (($states | ForEach-Object { "$($_.Name) $($_.Lines)" }) -join ' · '))
  return @{ Kind = $b.Kind; Code = 0; Kills = $kills; States = $states }
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
if (-not (Test-Path -LiteralPath $stackExe)) { Write-Host "[멈춤] 없음: $stackExe"; exit 2 }

if ($SelfTest) {
  # 가짜 서버 = nox_stack --victim fakeserver(1초마다 [구간] 줄 · 사이사이 [판 결과] 줄 · 끝내기 이벤트를 듣는다) · 가짜 더미 = --victim sleep.
  #   갈래마다 강제 종료 수를 센다(잡힘 = 멈춘 서버 + 더미 둘 · 서버 죽음 = 더미 둘 · 더미 먼저 끝남 = 남은 더미 · 상한 = 0).
  #   상한 판의 가짜 더미는 진짜처럼 상한 초만큼 자다 스스로 끝난다 · 다른 판은 상한보다 길게 자 강제 종료가 세어지게
  $cases = @(
    @{ Name = '잡힘';           A = '--stall-at 5';                 DA = 120; DB = 120; Early = 60; Kills = 3; Want = 'VictimSpin' },
    @{ Name = '서버 죽음';      A = '--die-at 5 --die-code 3';      DA = 120; DB = 120; Early = 60; Kills = 2; Want = '' },
    @{ Name = '더미 먼저 끝남'; A = '';                             DA = 5;   DB = 120; Early = 20; Kills = 1; Want = '' },
    @{ Name = '상한';           A = '';                             DA = 60;  DB = 60;  Early = 60; Kills = 0; Want = '' },
    @{ Name = '잡힘';  One = $true; A = '--stall-at 5';            DA = 120; DB = 120; Early = 60; Kills = 2; Want = 'VictimSpin' })   # 쌍 하나(-OnePair 코어) = 멈춘 서버 + 더미 하나
  $base = Join-Path $Repo "_out\stall_hunt\$stamp`_selftest"
  $pass = 0; $bad = 0
  $i = 0
  foreach ($c in $cases) {
    $i++
    $specs = @()
    foreach ($d in $(if ($c.One) { @($onePairDef) } else { $pairDefs })) {
      $isA = $d.Name -eq 'A'
      $specs += @{ Name = $d.Name; SrvMask = $d.SrvMask; DumMask = $d.DumMask; ServerExe = $stackExe; DummyExe = $stackExe
                   ServerArgs = ('--victim fakeserver ' + $(if ($isA) { $c.A } else { '' })).Trim()
                   DummyArgs = "--victim sleep --sec $(if ($isA) { $c.DA } else { $c.DB })" }
    }
    $r = Invoke-Hunt (Join-Path $base "$i") $specs 60 $c.Early ([IO.Path]::ChangeExtension($stackExe, '.pdb'))
    $checks = @(
      @("갈래 = $($c.Name)", ($r.Kind -eq $c.Name), $r.Kind),
      @("강제 종료 수 = $($c.Kills)", ($r.Kills.Count -eq $c.Kills), "$($r.Kills.Count) — $($r.Kills -join ' · ')"),
      @('쌍마다 읽은 [구간] 줄 > 0', (@($r.States | Where-Object { $_.Lines -le 0 }).Count -eq 0), (($r.States | ForEach-Object { "$($_.Name) $($_.Lines)" }) -join ' · ')))
    if ($c.Want) {
      $ht = [IO.File]::ReadAllText((Join-Path (Join-Path $base "$i") 'hunt.txt'))
      $l1 = @($ht -split "`r`n" | Where-Object { $_.StartsWith('[요약 1]') })
      $l2 = @($ht -split "`r`n" | Where-Object { $_.StartsWith('[요약 2]') })
      $dmp = Join-Path (Join-Path $base "$i") 'A\server.dmp'
      $checks += , @("hunt.txt 요약 둘에 $($c.Want) · 덤프", ($l1.Count -eq 1 -and $l1[0].Contains($c.Want) -and $l2.Count -eq 1 -and $l2[0].Contains($c.Want) -and
                     (Test-Path -LiteralPath $dmp) -and (Get-Item -LiteralPath $dmp).Length -gt 0), (($l1 + $l2) -join ' / '))   # ⚠ 괄호 — 쉼표가 -join 보다 먼저 묶여 배열 전체가 한 글줄이 된다(11-18 첫 시험)
    }
    foreach ($k in $checks) {
      Write-Host ('{0} 시험 {1} {2} — {3}' -f $(if ($k[1]) { '[통과]' } else { '[틀림]' }), $i, $k[0], $k[2])
      if ($k[1]) { $pass++ } else { $bad++ }
    }
  }
  Write-Host "시험 $pass/$($pass + $bad) 통과 — $base"
  if ($bad) { exit 10 } else { exit 0 }
}

# ── 본 판 ──────────────────────────────────────────────────────
$srvExe = Join-Path $rel 'nox_server.exe'
$dumExe = Join-Path $rel 'nox_dummy.exe'
$map = Join-Path $GameDir 'maps\So_LOD\So_LOD.map'
foreach ($x in @($srvExe, $dumExe, [IO.Path]::ChangeExtension($srvExe, '.pdb'), $map)) {
  if (-not (Test-Path -LiteralPath $x)) { Write-Host "[멈춤] 없음: $x"; exit 2 }
}
$runDir = Join-Path $Repo "_out\stall_hunt\$stamp"
$specs = @()
foreach ($d in $(if ($OnePair) { @($onePairDef) } else { $pairDefs })) {
  $results = Join-Path (Join-Path $runDir $d.Name) 'matches.jsonl'
  # 서버 인자 = 멈춘 판(_out/load_run/20261118_032106_LIM_p30_r416_t1 env.txt 「서버 명령줄」)과 같음 + --port ·
  #   더미 인자 = 같음(스레드만 6 → 3) + 입장 간격 · 상한(더미가 스스로 끝난다) · -OnePair 면 스레드 6 · 입장 0.2초 그대로
  $specs += @{ Name = $d.Name; SrvMask = $d.SrvMask; DumMask = $d.DumMask; ServerExe = $srvExe; DummyExe = $dumExe
               ServerArgs = "--quiet --aoi --friends --battle-maps Estate --no-join-token --standalone --map `"$map`" --load-rooms $Rooms --load-room-size 10 --stat-mult 3 --tick-sections --results `"$results`" --no-event-log --no-first-seen --no-op-count --port $($d.Port)"
               DummyArgs = "--server 127.0.0.1:$($d.Port) --count $($Rooms * 10) --room-size 10 --classes 0,1 --behavior fight --stat-mult 3 --freq-mult 1 --duration-sec $CapSec --threads $DummyThreads --join-interval-ms $JoinMs" }
}
$r = Invoke-Hunt $runDir $specs $CapSec $EarlySec ([IO.Path]::ChangeExtension($srvExe, '.pdb'))
if ($r.Code) { exit $r.Code }
exit 0
