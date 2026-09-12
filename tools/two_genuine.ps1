# two_genuine.ps1 — 정품 클라 **둘**을 내 서버에 붙이는 한 바퀴. 숨은 데스크톱이라 화면을 안 뺏는다.
#
# 왜 만드나 — 「클라가 접속 직후 죽는다」는 09-08 저녁에 **오픈녹스 클라로만** 잰 값이다.
#   정품 클라 둘은 커밋 8b78b89(09-08 02:52)에 동시 인게임이 됐고, 4164afb(05:42)에는
#   아레나 한 판까지 돌았다. 그 뒤 서버가 여덟 번 바뀌었으니 지금도 그런지 다시 잰다.
#   이 시험이 가르는 것 = 죽음이 **내 서버 탓인가, 오픈녹스 클라 고유의 것인가.**
#
#   .\tools\two_genuine.ps1 -Tag g1 [-Genuine] [-ServerArgs '--raw-snapshot']
#
# 산출물 : _evidence\CAP_<Tag>.txt (와이어) · srv_<Tag>.log (서버 틱) ·
#          _evidence\desk1\<Tag>A*.png / <Tag>B*.png / <Tag>_A2.png (B 접속 뒤 A 화면)
param(
  [Parameter(Mandatory=$true)][string]$Tag,
  [switch]$Genuine,            # 내 서버 대신 정품(opennox-server)을 뒤에 둔다 — 대조군
  [string]$ServerArgs = '',
  # ★ 서두를 어느 녹음에서 뽑을지. **이것이 클라 종류와 짝이다.**
  #   onox    = SNAP_onox_ref.txt  (오픈녹스 클라로 뜬 것 — 09-08 저녁 작업이 여기 맞춰져 있다)
  #   default = --dump 를 안 준다  → main.cpp DefaultDumpPath() = CAP_arena_0907_ok.txt
  #                                  (**정품 클라**로 뜬 09-07 아레나 녹음)
  #   그 밖   = 준 경로를 그대로 쓴다
  [string]$Prelude = 'onox',
  [int]$WaitMenu = 40,
  # 둘 다 들어온 뒤 몇 초를 더 지켜보나. 늦게 죽는 것을 잡는 자리다.
  [int]$WatchSeconds = 40,
  # ★ 싸움 — 둘 다 **왼버튼을 누른 채로** 둔다. 스폰이 40px 떨어져 있고
  #   근접 사거리가 45 라 바로 닿는다(Server.h 의 kMeleeRange).
  #   한 대 12 · 최대 체력 75 · 쉬는 시간 0.5초 → 한 사람 죽이는 데 최소 3.5초,
  #   판끝(10킬)까지 최소 35초. 넉넉히 주려면 180 쯤.
  #   ⚠ 첫 클릭은 **관전 풀기**라 공격이 아니다 — 접속 몰기에서 이미 한 번 썼다.
  [int]$FightSeconds = 0,
  # `-Genuine` 일 때 정품 서버에 덧붙일 인자. 맵은 여기서 고른다:
  #   -GenuineArgs '-autoexec "load bludeath"'
  [string]$GenuineArgs = '',
  # ★ 키 탐침 — 어떤 키가 걸음이고 어떤 키가 무슨 주문인지 잰다 (4.6 절 참조)
  [switch]$Probe,
  # ★ 걷기 시험 — A 를 지금 자리에서 이만큼 떨어진 곳까지 걸려 보낸다. "250,0" 꼴.
  #   화면각↔월드각 차(Δ)를 배우는 자리라, 붙기 전에 여기서 먼저 맞춰 둔다.
  # ★★ **계측을 시험하는 스위치** — 이 초가 지나면 클라 B 를 일부러 죽인다.
  #   왜 있나: 생존 감시가 **있었는데 판정으로 안 이어져** 죽은 런이 통과했다(2026-09-11).
  #   고친 뒤에는 「무효를 내는가」를 **직접 확인해야** 한다. 안 내면 계측이 아직 거짓말한다.
  #   기대 결과 — 종료 코드 3 + 「⛔ 판정 무효」.
  [int]$SelfTestKill = 0,
  [string]$WalkTest = '',
  # ★ 판을 실제로 뛴다 — A 가 깃발을 가지러 가거나 몬스터를 찾아간다.
  #   **정상 배치**(멀리 떨어진 자리)에서도 판이 도는지 보는 자리다.
  [int]$PlaySeconds = 0,
  # ★ 직업 — **캐릭터 목록에서 어느 줄을 고르나**로 갈린다.
  #   2026-09-09 실측 목록 = **1 컨저러 · 2 위저드 · 3~5 워리어** (새로 만든 것이 맨 위로 온다).
  #   워리어는 근접(왼버튼), 위저드는 주문키(F 파이어볼 · D 연쇄번개)로 싸운다.
  #   ⚠ 컨저러는 목록에 없어서 `tools\newchar.ps1` 로 새로 만들었다 —
  #     위저드 주문칸 다섯(4 점멸 · 51 보호막 · 43 연쇄번개 · 27 파이어볼 · 41 약한치유)에
  #     소환(75~86)이 하나도 없어, 직업을 바꾸지 않고는 소환을 시킬 길이 없었다.
  [ValidateSet('warrior','wizard','conjurer')][string]$Class = 'warrior',
  # ★ 모드 — 서버에 그대로 넘긴다. `ctf`·`quest` 는 둘 다 들어온 뒤에 차려진다.
  [ValidateSet('arena','ctf','quest')][string]$Mode = 'arena'
)
# ★ Stop 으로 두면 네이티브 도구가 stderr 에 한 줄만 뱉어도 한 바퀴가 통째로 끊긴다
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$ev   = Join-Path $root '_evidence'
$ctrl = Join-Path $ev 'desk1'
$dump = Join-Path $ev ("CAP_{0}.txt" -f $Tag)
$slog = Join-Path $ev ("srv_{0}.log" -f $Tag)

function Spawn([string]$cmd, [string]$cwd) {
  # ★ 툴 세션이 끝나면 자식이 같이 죽는다 → WMI 로 띄운다
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $cmd; CurrentDirectory = $cwd }
  if ($r.ReturnValue -ne 0) { Write-Host "[FAIL] 못 띄움($($r.ReturnValue)): $cmd"; exit 1 }
  return $r.ProcessId
}
function Send-Desk([string]$lines) {
  [IO.File]::WriteAllText((Join-Path $ctrl 'cmd.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}
function Shot([int]$target, [string]$name) {
  # ★ target 을 빼면 shotfull 이 「대상 없음」으로 조용히 실패한다
  if ($target -le 0) { Write-Host "  (샷 건너뜀 — pid 를 못 잡았다: $name)"; return }
  Send-Desk "target $target`nshotfull $ctrl\$name.png`n"
  Start-Sleep -Seconds 2
}
# ★★ pid 0 을 걸러야 한다 — `Get-Process -Id 0` 은 **Idle 프로세스라 성공한다.**
#   pid 를 못 잡은 회차가 통째로 「살아있음」으로 찍혀 판정이 뒤집혔다(2026-09-09).
function Alive([int]$p) {
  if ($p -le 4) { return $false }
  return ($null -ne (Get-Process -Id $p -ErrorAction SilentlyContinue))
}
# 클라가 태어난 자리에서 적어 둔 pid 를 읽는다. 이름으로 다시 찾으면 이미 죽었을 때 0 이 된다.
# ★ 공유 읽기 — 서버·프록시가 **아직 쓰고 있는** 파일이다. 그냥 열면 튕긴다.
function ReadShared([string]$p) {
  if (-not (Test-Path $p)) { return '' }
  $f = New-Object IO.FileStream($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
  $r = New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)
  $t = $r.ReadToEnd(); $r.Close(); $f.Close(); return $t
}
function LastPid() {
  $f = Join-Path $ctrl 'lastpid.txt'
  if (-not (Test-Path $f)) { return 0 }
  $v = 0; [void][int]::TryParse(([IO.File]::ReadAllText($f)).Trim(), [ref]$v); return $v
}

# 0. 자리 비우기. ★ 서버도 매 바퀴 새로 띄운다 (죽은 세션이 남으면 새 접속이 목록으로 되돌아간다)
Get-Process nox_server, nox_proxy, opennox, opennox-server, deskhost -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
# 프로세스를 죽여도 UDP 소켓이 잠깐 남는다. 안 기다리면 서버가 bind 10048 로 조용히 죽고
# 화면만 보면 「접속이 안 된다」로 보인다.
foreach ($try in 1..12) {
  $busy = @(netstat -ano -p UDP | Select-String ':1859[01]\s')
  if ($busy.Count -eq 0) { break }
  Start-Sleep -Seconds 1
}
foreach ($f in @($dump, $slog)) { if (Test-Path $f) { [IO.File]::Delete($f) } }

# ★★★ **이 태그로 전에 찍은 캡처를 지운다** (2026-09-11 — 미결 「캡처 재사용 즉사」의 정체).
#   Windows 는 파일 이름의 **대소문자를 안 가린다.** 낮에 `GEN2` 로 돌린 캡처가 남아 있는데
#   밤에 `gen2` 로 돌리면 **같은 파일**이라, 이번 런이 못 찍은 자리에 **옛 그림이 그대로** 있다.
#   하네스는 그것을 「이번 런의 화면」으로 읽고 **엉뚱한 판정**을 한다.
#   실측 — `gen2` 회차에서 `A1_list` 는 23:29 인데 `A2_chars`·`A3_ingame` 은 **13:10**(낮)이었다.
$staleDir = Join-Path $ev 'desk1'
$stale = @(Get-ChildItem $staleDir -Filter ($Tag + '*.png') -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
  Write-Host ("[정리] 이 태그로 전에 찍은 캡처 {0}장을 지운다 — 안 지우면 옛 그림을 새 것으로 읽는다" -f $stale.Count) -ForegroundColor Yellow
  foreach ($s in $stale) { try { [IO.File]::Delete($s.FullName) } catch {} }
}
if (-not (Test-Path $ctrl)) { New-Item -ItemType Directory $ctrl | Out-Null }

# ★★ 런 전 문지기 — 2026-09-10 에 이것이 없어서 **실클라 네 판을 날렸다.**
#   증상은 「접속 요청 158바이트에 답이 0개」였고 로그 어디에도 까닭이 없었다.
#   진짜 원인은 -ServerArgs 에 --map 을 안 준 것 하나였다. 맵이 없으면
#   SetStandalone 이 안 불려 CLIENT_ACCEPT 가 조용히 아무 답도 안 보낸다.
if (-not $Genuine -and $ServerArgs -match '--standalone' -and $ServerArgs -notmatch '--map') {
  Write-Host '[멈춤] --standalone 인데 -ServerArgs 에 --map 이 없다.' -ForegroundColor Red
  Write-Host '       하네스는 --map 을 자기가 안 붙인다. 이대로 돌리면 아무도 못 들어온다.'
  Write-Host '  보기: -ServerArgs ''--standalone --map "<게임폴더>\maps\estate\Estate.map"'''
  exit 2
}

# 1. 서버
if ($Prelude -eq 'onox')      { $dumpArg = ' --dump "' + (Join-Path $ev 'SNAP_onox_ref.txt') + '"' }
elseif ($Prelude -eq 'default') { $dumpArg = '' }
else                          { $dumpArg = ' --dump "' + $Prelude + '"' }
if ($Genuine) {
  $g = Join-Path $root 'third_party\opennox\opennox-server.exe'
  # ★ `-autoexec` 로 맵을 고른다(기본은 `load estate`). estate 는 넓어서 둘이 서로
  #   시야 밖에 스폰한다 — **정품에서도 서로 안 보인다**. 「상대가 그려진 상태」의
  #   표본을 뜨려면 BluDeath 같은 작은 데스매치 맵이라야 한다(2026-09-09).
  $spid = Spawn ('"' + $g + '" -serveronly -noDraw -port 18591 ' + $GenuineArgs) (Split-Path -Parent $g)
  Write-Host ("정품 서버 pid={0} {1}" -f $spid, $GenuineArgs)
} else {
  # ★ 로그를 파일로 받는다 — WMI 로 띄우면 콘솔이 없어 stdout 이 사라진다.
  #   틱 로그의 「사람 N(갱신에 M)」이 서로 보이는지를 가르는 자리다.
  $spid = Spawn ('cmd /c ""' + (Join-Path $root 'build\Release\nox_server.exe') +
    '" --port 18591' + $dumpArg + ' --mode ' + $Mode + ' ' + $ServerArgs + ' > "' + $slog + '" 2>&1"') $root
  Write-Host ("내 서버 pid={0} 서두={1} 모드={2} 직업={3} {4}" -f $spid, $Prelude, $Mode, $Class, $ServerArgs)
}
Start-Sleep -Seconds 3

# 2. 프록시 (와이어를 뜬다)
$ppid = Spawn ('"' + (Join-Path $root 'build\Release\nox_proxy.exe') +
  '" --listen 18590 --host 127.0.0.1 --host-port 18591' + $(if ($Genuine) { ' --fix-players' } else { '' }) + ' --dump "' + $dump + '"') $root
Write-Host "프록시 pid=$ppid"
Start-Sleep -Seconds 2

# 3. 클라 A
$join = Join-Path $PSScriptRoot 'join_my_server.ps1'
Write-Host "--- A (Game3) 몰기 ---"
# 실측 목록(2026-09-09, 컨저러를 만든 뒤) — 1 컨저러 · 2 위저드 · 3~5 워리어
$charRow = switch ($Class) { 'conjurer' { 1 } 'wizard' { 2 } default { 3 } }
& $join -Exe 'Game3.exe' -Tag ("{0}A" -f $Tag) -WaitMenu $WaitMenu -CharRow $charRow
$aPid = LastPid
Write-Host ("A pid={0} · A 상태(B 몰기 전): {1}" -f $aPid, $(if (Alive $aPid) { '살아있음' } else { '죽음' }))

# 4. 클라 B
Write-Host "--- B (Game4) 몰기 ---"
& $join -Exe 'Game4.exe' -Tag ("{0}B" -f $Tag) -WaitMenu $WaitMenu -CharRow $charRow
$bPid = LastPid

# 여기서 기다리지 않는다 — 3초 사이에 A 가 죽어 못 찍은 적이 있다
$aAlive2 = Alive $aPid
$bAlive2 = Alive $bPid
Write-Host ("A 상태(B 몰기 후): {0} · B 상태: {1}" -f
  $(if ($aAlive2) { '살아있음' } else { '죽음' }), $(if ($bAlive2) { '살아있음' } else { '죽음' }))
# ★ B 가 들어온 뒤 **A 화면에 B 가 보이는지**가 판정이다
if ($aAlive2) { Shot $aPid ("{0}_A2" -f $Tag) }
if ($bAlive2) { Shot $bPid ("{0}_B2" -f $Tag) }

# 4.4 싸움 — **눌렀다 떼기를 되풀이한다.**
#
# ★★ 누른 채로 두면 안 된다. 왼버튼(입력 코드 6)은 **누르는 순간 한 번만** 온다 —
#   2026-09-09 실측: 180초를 누른 채 두었는데 조각 10,885 개 중 코드 6 은 **2건**뿐이고
#   나머지는 전부 방향(코드 1)이었다. 서버 쪽 주석의 「누르고 있으면 매 틱 온다」는 틀렸다.
#   (같은 것이 2026-09-08 기록에도 있었다 — 「4초 눌러도 코드 6 단 1회」)
# ★ 한 대 12 · 최대 체력 75 → 죽이는 데 7대. 판끝 10킬이면 한쪽이 70대를 때려야 한다.
#   근접 쉬는 시간이 0.5초라 **한 번 누르고 0.5초**가 가장 촘촘한 박자다.
if ($FightSeconds -gt 0 -and (Alive $aPid) -and (Alive $bPid)) {
  $di = Join-Path $PSScriptRoot 'di.ps1'
  Write-Host ("--- 싸움 {0}초 (눌렀다 떼기를 되풀이) ---" -f $FightSeconds)
  # ★ 한 번에 여섯 대씩(약 3초)만 때리고 로그를 본다.
  #   ⚠ 전에는 스물다섯 대(약 12.5초)씩 몰아쳤다. 그 사이에 판이 끝나면 **끝난 뒤에도
  #     계속 눌러** 승자 화면을 제 손으로 닫아 버렸다 — 2026-09-09 실측:
  #     진 쪽(A)에는 「Qack has taught you well.」 이 떴는데 **이긴 쪽(B)에는 아무것도 없었다.**
  #     클릭 한 번을 A 에 보내니 A 의 것도 사라져, 배너를 닫는 것이 클릭임을 확인했다.
  $pulse = @('inject 1')
  if ($Class -eq 'conjurer') {
    # ★ 컨저러 — A 가 소환(87 SUMMON_IMP)이다. 2026-09-09 실측 칸 —
    #   A 87 임프소환 · S 74 기절 · D 128 독구름 · F 58 요정떼 · G 41 약한치유.
    #   소환수가 알아서 다가가 때리므로 사람은 소환만 되풀이한다.
    foreach ($i in 1..3) { $pulse += @('k 0x1E 1','sleep 90','k 0x1E 0','sleep 900') }
  } elseif ($Class -eq 'wizard') {
    # ★ 위저드 — 주문키를 누른다. F=파이어볼(27) · D=연쇄번개(43).
    #   2026-09-09 실측으로 칸을 확정했다 — A 4 점멸 · S 51 보호막 · D 43 연쇄번개 ·
    #   F 27 파이어볼 · G 41 약한치유. 공격은 D·F 둘뿐이다.
    foreach ($i in 1..3) {
      $pulse += @('k 0x21 1','sleep 90','k 0x21 0','sleep 500',
                  'k 0x20 1','sleep 90','k 0x20 0','sleep 500')
    }
  } else {
    foreach ($i in 1..6) { $pulse += @('mb 0 1','sleep 80','mb 0 0','sleep 420') }
  }
  # ★★★ **겨누고 친다** (2026-09-12 교체).
  #
  #   옛 하네스는 위 `$pulse` 를 그대로 되풀이했다 — **커서를 한 번도 안 움직인다.**
  #   그래서 몸 방향이 접속 초기값 66 에 붙박였고(66 × π/128 ≈ 93°, 아래쪽),
  #   상대는 x 축으로 40 떨어진 자리(0°/180°)에 있어 **늘 빗나갔다.**
  #   09-12 회귀 실측 — wizard 아레나 `주문 172(마나막힘 152) · 투사체명중 0`,
  #   자리 줄에 둘 다 `facing 66` 이고 체력이 `75/75` 그대로였다.
  #
  #   지금은 A·B 각자 `walk_genuine.ps1 -Play arena` 를 띄워 **상대를 겨누고** 친다.
  #   겨눔은 걷기와 같은 길이다(`Build-Table` 이 커서를 돌려 서버가 되돌려 주는
  #   방향값으로 표를 만든다 — 짐작이 아니라 재는 것).
  #   ⚠ `$pulse` 는 남겨 둔다 — 겨눔 일꾼이 못 서면 이 자리가 비교선이 된다.
  $line = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '^\[자리\]' } | Select-Object -Last 1)
  $ncs = @()
  if ($line.Count -gt 0) {
    foreach ($m in [regex]::Matches($line[0], '사람 (\d+)=')) { $ncs += [int]$m.Groups[1].Value }
  }
  $ncs = @($ncs | Sort-Object)
  if ($ncs.Count -lt 2) {
    Write-Host '  ⚠ 자리 줄에 사람이 둘이 안 보인다 — 싸움을 건너뛴다'
  } else {
    # A 가 먼저 들어왔으므로 번호가 더 작다(번호는 5씩 올라간다).
    Write-Host ("--- 겨누고 치기 {0}초 — A={1} · B={2} ({3}) ---" -f $FightSeconds, $ncs[0], $ncs[1], $Class)
    $walk = Join-Path $PSScriptRoot 'walk_genuine.ps1'
    $procs = @()
    foreach ($pair in @(@($aPid, $ncs[0]), @($bPid, $ncs[1]))) {
      $procs += Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $walk,
        '-Pid_', $pair[0], '-NetCode', $pair[1], '-Log', $slog,
        '-Tx', '0', '-Ty', '0', '-Play', 'arena',
        '-PlaySeconds', $FightSeconds, '-Class', $Class)
    }
    # ★ 기다리는 동안 틱을 본다 — 판끝은 **여덟 틱 동안만 참**이라 끝나고 한 번만
    #   보면 놓친다(matrix_run.ps1 의 누적 판정 주석과 같은 함정).
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt ($FightSeconds + 30)) {
      Start-Sleep -Seconds 5
      $last = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
      if ($last.Count -gt 0) { Write-Host ("  {0}" -f $last[0]) }
      if ($last.Count -gt 0 -and $last[0] -match '판끝 예') { Write-Host '  ★ 판이 끝났다'; break }
      if (-not (Alive $aPid) -or -not (Alive $bPid)) { Write-Host '  ⚠ 클라가 죽었다 — 싸움 중단'; break }
      if (@($procs | Where-Object { -not $_.HasExited }).Count -eq 0) {
        Write-Host '  (겨눔 일꾼이 둘 다 끝났다)'; break
      }
    }
    foreach ($p in $procs) {
      if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
  }
  foreach ($p in @($aPid, $bPid)) { if (Alive $p) { & $di -Target $p -Cmd @('mb 0 0') | Out-Null } }
}

# 4.55 걷기 시험 — A 를 정해진 자리까지 걸려 본다.
#
# ★ 왜 여기 두나 — 깃발·퀘스트를 **정상 배치**(멀리 떨어진 자리)로 돌리려면
#   사람이 걸어가야 한다. 걸음은 오른버튼이고 방향은 커서인데, 화면각과 월드각의
#   차를 모르니 **먼저 배우는 자리**가 필요하다(walk_genuine.ps1 이 그걸 한다).
if ($WalkTest -ne '' -and (Alive $aPid)) {
  $parts = $WalkTest -split ','
  $offX = [double]$parts[0]; $offY = [double]$parts[1]
  $line = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '^\[자리\]' } | Select-Object -Last 1)
  if ($line.Count -eq 0) { Write-Host '  [FAIL] 자리 줄이 없다' }
  else {
    $codes = [regex]::Matches($line[0], '사람 (\d+)=(-?\d+),(-?\d+)')
    if ($codes.Count -lt 1) { Write-Host '  [FAIL] 자리 줄에 사람이 없다' }
    else {
      # A 가 먼저 들어왔으므로 번호가 더 작다 (번호는 5씩 올라간다)
      $best = $null
      foreach ($m in $codes) { if (-not $best -or [int]$m.Groups[1].Value -lt [int]$best.Groups[1].Value) { $best = $m } }
      $nc = [int]$best.Groups[1].Value
      $tx = [double]$best.Groups[2].Value + $offX
      $ty = [double]$best.Groups[3].Value + $offY
      Write-Host ("--- 걷기 시험 — 사람 {0} 을 ({1},{2}) 로 ---" -f $nc, [int]$tx, [int]$ty)
      if ($WalkTest -like 'cursor*') {
        & (Join-Path $PSScriptRoot 'walk_genuine.ps1') -Pid_ $aPid -NetCode $nc -Log $slog -Tx $tx -Ty $ty `
          -CursorProbe '100,60;200,60;300,60;400,60;100,160;100,260' -ShotDir $ctrl
      } elseif ($WalkTest -eq 'scale,0' -or $WalkTest -eq 'scale') {
        & (Join-Path $PSScriptRoot 'walk_genuine.ps1') -Pid_ $aPid -NetCode $nc -Log $slog -Tx $tx -Ty $ty -ScaleProbe
      } elseif ($WalkTest -eq 'sweep,0' -or $WalkTest -eq 'sweep') {
        & (Join-Path $PSScriptRoot 'walk_genuine.ps1') -Pid_ $aPid -NetCode $nc -Log $slog -Tx $tx -Ty $ty -Sweep
      } else {
        & (Join-Path $PSScriptRoot 'walk_genuine.ps1') -Pid_ $aPid -NetCode $nc -Log $slog -Tx $tx -Ty $ty
      }
    }
  }
}

# 4.57 판 뛰기 — A 를 몰아 실제로 깃발을 나르거나 몬스터를 잡는다.
if ($PlaySeconds -gt 0 -and $Mode -ne 'arena' -and (Alive $aPid)) {
  $line = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '^\[자리\]' } | Select-Object -Last 1)
  if ($line.Count -gt 0) {
    $codes = [regex]::Matches($line[0], '사람 (\d+)=')
    $nc = 0
    foreach ($m in $codes) { $v=[int]$m.Groups[1].Value; if ($nc -eq 0 -or $v -lt $nc) { $nc = $v } }
    if ($nc -gt 0) {
      Write-Host ("--- 판 뛰기 ({0}) — 사람 {1}, {2}초 ---" -f $Mode, $nc, $PlaySeconds)
      # ★ `-Class` 를 넘긴다 (2026-09-12) — 안 넘기면 퀘스트가 직업을 안 가리고
      #   왼버튼만 누르고, 위저드는 그 자리에 걸린 주문이 **치유**라 몬스터가 안 죽는다.
      & (Join-Path $PSScriptRoot 'walk_genuine.ps1') -Pid_ $aPid -NetCode $nc -Log $slog `
        -Tx 0 -Ty 0 -Play $Mode -PlaySeconds $PlaySeconds -Class $Class
    }
  }
}

# 4.6 키 탐침 — **어떤 키가 걸음이고 어떤 키가 무슨 주문인가**를 잰다.
#
# 왜 필요한가 — 3직업(근접·주문·소환)을 실클라로 돌리려면 「주문 키」를 알아야 하는데
#   지금 아는 것은 화면에 A~G 다섯 칸이 있다는 것뿐이다. 어떤 칸이 무슨 주문 번호인지,
#   그중에 소환(75~86)이 있는지 **잰 적이 없다.** 걸음도 마찬가지 — 지금껏 걸음 0 이었다.
#
# 재는 법 — 키를 하나씩 눌렀다 떼고, 그 사이 서버 틱로그의 「걸음·주문」이 오르는지 본다.
#   주문 번호는 이 회차 캡처의 `0x79 TRY_SPELL` 을 나중에 뜯어 읽는다(시각으로 짝짓는다).
if ($Probe -and (Alive $aPid)) {
  $di = Join-Path $PSScriptRoot 'di.ps1'
  # DIK 코드. 주문칸은 화면에 A·S·D·F·G 로 적혀 있다.
  $keys = [ordered]@{
    'A'=0x1E; 'S'=0x1F; 'D'=0x20; 'F'=0x21; 'G'=0x22;
    '위'=0xC8; '아래'=0xD0; '왼'=0xCB; '오른'=0xCD;
    'W'=0x11; 'Q'=0x10; 'E'=0x12; 'Z'=0x2C; 'X'=0x2D; 'C'=0x2E
  }
  function TickNums() {
    $l = @((ReadShared $slog) -split "`r?`n" | Where-Object { $_ -match '\[틱\]' } | Select-Object -Last 1)
    if ($l.Count -eq 0) { return @{ '걸음'=0; '주문'=0; '근접'=0 } }
    $h = @{}
    foreach ($k in @('걸음','주문','근접')) {
      if ($l[0] -match ($k + ' (\d+)')) { $h[$k] = [int]$Matches[1] } else { $h[$k] = 0 }
    }
    return $h
  }
  Write-Host '--- 키 탐침 (A 클라) ---'
  & $di -Target $aPid -Cmd @('inject 1') | Out-Null
  $before = TickNums
  foreach ($name in $keys.Keys) {
    $code = $keys[$name]
    $t0 = Get-Date -Format 'HH:mm:ss.fff'
    & $di -Target $aPid -Cmd @(("k {0} 1" -f $code), 'sleep 700', ("k {0} 0" -f $code), 'sleep 900') | Out-Null
    $after = TickNums
    $d = @{}
    foreach ($k in @('걸음','주문','근접')) { $d[$k] = $after[$k] - $before[$k] }
    Write-Host ("  {0,-4} (0x{1:x2}) {2}  걸음 +{3} · 주문 +{4} · 근접 +{5}" -f
      $name, $code, $t0, $d['걸음'], $d['주문'], $d['근접'])
    $before = $after
  }
  # 오른버튼도 본다 — 녹스의 걸음이 마우스일 가능성
  $t0 = Get-Date -Format 'HH:mm:ss.fff'
  & $di -Target $aPid -Cmd @('mb 1 1','sleep 1200','mb 1 0','sleep 900') | Out-Null
  $after = TickNums
  Write-Host ("  오른버튼      {0}  걸음 +{1} · 주문 +{2} · 근접 +{3}" -f $t0,
    ($after['걸음']-$before['걸음']), ($after['주문']-$before['주문']), ($after['근접']-$before['근접']))
}

# 4.5 생존 시계열 — ★ 「죽었다/살았다」 한 번 찍는 것으로는 **늦게 죽는 것**을 못 잡는다.
#   한 회차에서 「끄니까 살았다」로 읽었는데 실은 상대가 아직 안 들어와 있었다(2026-09-08).
#   여기서는 초 단위로 둘을 같이 재고, 죽은 순간을 초로 남긴다.
# ★ 계측 자기시험 — 예약해 둔 초가 되면 클라 B 를 죽인다(백그라운드 작업으로).
#   진짜 튕김과 같은 꼴을 만들어 **판정이 무효를 내는지** 본다.
if ($SelfTestKill -gt 0) {
  Write-Host ("[자기시험] {0}초 뒤 클라 B(pid={1}) 를 죽인다 — 판정이 무효를 내야 한다" -f $SelfTestKill, $bPid) -ForegroundColor Magenta
  Start-Job -ScriptBlock {
    param($sec, $target)
    Start-Sleep -Seconds $sec
    Stop-Process -Id $target -Force -ErrorAction SilentlyContinue
  } -ArgumentList $SelfTestKill, $bPid | Out-Null
}

$watchLog = Join-Path $ev ("alive_{0}.log" -f $Tag)
$sb2 = New-Object Text.StringBuilder
$aDeadAt = ''; $bDeadAt = ''
for ($k = 0; $k -lt $WatchSeconds; $k += 2) {
  $ta = Alive $aPid; $tb = Alive $bPid
  $stamp = (Get-Date -Format 'HH:mm:ss.fff')
  [void]$sb2.AppendLine(("{0} A={1} B={2}" -f $stamp, $(if($ta){1}else{0}), $(if($tb){1}else{0})))
  if (-not $ta -and $aDeadAt -eq '') { $aDeadAt = $stamp }
  if (-not $tb -and $bDeadAt -eq '') { $bDeadAt = $stamp }
  Start-Sleep -Seconds 2
}
[IO.File]::WriteAllText($watchLog, $sb2.ToString(), (New-Object Text.UTF8Encoding($false)))
Write-Host ("생존 감시 {0}초 — A {1} · B {2}" -f $WatchSeconds,
  $(if ($aDeadAt -eq '') { '끝까지 살아있음' } else { "죽음 $aDeadAt" }),
  $(if ($bDeadAt -eq '') { '끝까지 살아있음' } else { "죽음 $bDeadAt" }))
# 감시 뒤 화면 — 여기까지 살아 있으면 **정말 붙어 있는 것**이다
if (Alive $aPid) { Shot $aPid ("{0}_A3" -f $Tag) }
if (Alive $bPid) { Shot $bPid ("{0}_B3" -f $Tag) }

# 5. 판정 자리
Write-Host ""
Write-Host "================= 판정 ================="

# ⑴ 서버가 스스로 말하는 사람 수 — 와이어를 뜯어 세는 것보다 이쪽이 믿을 만하다
$logText = ReadShared $slog
$tick = @(($logText -split "`r?`n") | Where-Object { $_ -match '\[틱\]' })
Write-Host ("틱 {0}개" -f $tick.Count)
if ($tick.Count -gt 0) {
  $prevPeople = ''
  foreach ($l in $tick) {
    if ($l -match '(사람 \d+\(갱신에 \d+\))' -and $Matches[1] -ne $prevPeople) {
      Write-Host ("  사람 수 변화: {0}" -f $Matches[1]); $prevPeople = $Matches[1]
    }
  }
  Write-Host ("  마지막 틱: {0}" -f $tick[-1])
  # ★ **가장 큰 값**을 따로 뽑는다 — 판이 끝나면 계수기가 되돌아가서
  #   마지막 줄만 보면 「한 판이 돌았다」를 놓친다.
  $best = @{ '근접'=0; '피해'=0; '주문'=0; '걸음'=0 }
  $roundOver = $false
  foreach ($l in $tick) {
    foreach ($k in @('근접','피해','주문','걸음')) {
      if ($l -match ($k + ' (\d+)') -and [int]$Matches[1] -gt $best[$k]) { $best[$k] = [int]$Matches[1] }
    }
    if ($l -match '판끝 예') { $roundOver = $true }
  }
  Write-Host ("  최대: 걸음 {0} · 근접 {1} · 주문 {2} · 피해 {3} · 판끝 {4}" -f
    $best['걸음'], $best['근접'], $best['주문'], $best['피해'],
    $(if ($roundOver) { '**예**' } else { '아니오' }))
}

# ⑵ 세션별 시계열 — ★ **「사람 2」만 보고 「둘 다 붙었다」로 읽으면 안 된다.**
#   한 회차에서 둘째가 접속만 하고 8건 만에 조용해졌는데 사람 수는 2였다(2026-09-08).
#   상대가 **언제까지 보냈나**를 같이 봐야 붙어 있는 것과 들어오다 만 것이 갈린다.
$re = [regex]'^(\d\d:\d\d:\d\d\.\d+) (C->S|S->C) ([\d\.]+):(\d+) len=(\d+)'
$ses = @{}
foreach ($l in ((ReadShared $dump) -split "`r?`n")) {
  $m = $re.Match($l); if (-not $m.Success) { continue }
  $port = $m.Groups[4].Value
  if (-not $ses.ContainsKey($port)) { $ses[$port] = @{ cs=0; sc=0; first=$m.Groups[1].Value; lastCS='' } }
  if ($m.Groups[2].Value -eq 'C->S') { $ses[$port].cs++; $ses[$port].lastCS = $m.Groups[1].Value }
  else { $ses[$port].sc++ }
}
Write-Host "--- 상대별 (조각 2건짜리는 서버 찾기·접속 시도용 임시 포트다) ---"
$ses.Keys | Sort-Object | ForEach-Object {
  Write-Host ("  {0,-7} C->S {1,5} · S->C {2,5}  처음 {3}  마지막 보낸 때 {4}" -f
    $_, $ses[$_].cs, $ses[$_].sc, $ses[$_].first, $(if ($ses[$_].lastCS -eq '') { '(없음)' } else { $ses[$_].lastCS }))
}
Write-Host ("화면: {0}\{1}A*.png · {1}B*.png · {1}_A2/_A3.png · {1}_B2/_B3.png" -f $ctrl, $Tag)
Write-Host ("생존 시계열: {0}" -f $watchLog)

# ⑶ 복호본 — `nox_opcount` 는 이 형식만 읽는다(프록시 덤프를 그대로 주면 0줄).
$snap = Join-Path $ev ("SNAP_{0}.txt" -f $Tag)
& (Join-Path $PSScriptRoot 'cap2sniff.ps1') -In $dump -Out $snap | Out-Null
Write-Host ("복호본: {0}" -f $snap)

# ── 6. ★★ 최종 판정 — **계측이 거짓말 못 하게 하는 문** ────────────────
#
# 왜 있나 (2026-09-11) — 생존 감시는 **이미 있었는데 판정으로 안 이어졌다.**
#   죽어도 `Write-Host` 한 줄 찍고 끝이었고 **종료 코드는 늘 0** 이었다.
#   그래서 요약할 때 그 줄을 안 읽으면 그대로 「통과」가 됐다 —
#   실제로 2차 플랜 회귀가 그렇게 통과했고, 사용자가 5분 플레이하자 튕겼다.
#
# ★ 여기서 무효를 내면 **위의 수치는 믿을 수 없는 것**이다.
#   「걸음 605」도 「주문 33」도 클라가 죽은 뒤라면 아무 뜻이 없다.
#
# 종료 코드 — 0 유효 · 3 클라 사망 · 4 클라 멈춤 · 5 버림 과다
$verdictCode = 0
$why = @()

# ⑴ 클라가 끝까지 살아 있었나
if ($aDeadAt -ne '') { $verdictCode = 3; $why += ("클라 A 가 {0} 에 죽었다" -f $aDeadAt) }
if ($bDeadAt -ne '') { $verdictCode = 3; $why += ("클라 B 가 {0} 에 죽었다" -f $bDeadAt) }

# ⑵ 멈춤 — 프로세스는 살아 있는데 **게임이 죽은** 꼴.
#   ★ 지문은 실측이다(`srv_play.log`, 사용자 플레이) — 주문 쓰고 튕긴 직후
#     클라가 보내는 것이 **3바이트짜리(조각 머리뿐)** 로만 이어졌다.
#     살아 있는 클라는 입력·마우스를 실어 19바이트쯤 보낸다.
$recv = @([regex]::Matches($logText, '\[받음 \d+\][^\r\n]*?\s(\d+)바이트'))
if ($recv.Count -ge 40) {
  $tailN = 30
  $small = 0
  for ($i = $recv.Count - $tailN; $i -lt $recv.Count; $i++) {
    if ([int]$recv[$i].Groups[1].Value -le 3) { $small++ }
  }
  if ($small -eq $tailN) {
    if ($verdictCode -eq 0) { $verdictCode = 4 }
    $why += ("클라가 멈췄다 — 마지막 수신 {0}회가 전부 3바이트뿐(게임이 죽은 지문)" -f $tailN)
  }
}

# ⑶ 버림 — 클라가 보낸 것을 우리가 통째로 버리는 것. 조용히 묻히면 안 된다.
#   ★ 2026-09-11 사용자 플레이에서 `0xae` 가 63건 버려지는데 아무도 안 봤다.
#   ★★ 2026-09-12 — **두 번 틀린 것을 고쳤다.**
#     ⑴ `[버림]` **줄 수**를 세고 있었다. 그 줄은 자리 줄처럼 **매 틱 찍히므로**
#        430 은 「버린 조각 430개」가 아니라 「틱이 430번 돌았다」였다.
#     ⑵ 버린 것이 전부 `0d(MSG_SERVER_INFO)` — **서버 찾기 브로드캐스트**이지
#        게임 조각이 아니다. 정상으로 끝난 warrior 퀘스트에도 2,551건 있었다.
#     → 마지막 줄의 **누적 수**를 읽고 거기서 서버 찾기(0d)를 뺀다.
#     ⚠ 이 오판으로 09-12 wizard 퀘스트 시험이 **멀쩡한데 「판정 무효」**를 받았다.
$dropLines = @([regex]::Matches($logText, '\[버림\] 조각 (\d+)개 — ([^\r\n]*)'))
$dropCount = 0
if ($dropLines.Count -gt 0) {
  $lastDrop = $dropLines[$dropLines.Count - 1]
  $dropCount = [int]$lastDrop.Groups[1].Value
  if ($lastDrop.Groups[2].Value -match '0d\(MSG_SERVER_INFO\)x(\d+)') {
    $dropCount -= [int]$Matches[1]
  }
}
if ($dropCount -gt 20) {
  if ($verdictCode -eq 0) { $verdictCode = 5 }
  $why += ("서버가 클라 조각을 {0}건 버렸다" -f $dropCount)
} elseif ($dropCount -gt 0) {
  Write-Host ("  ⚠ 버림 {0}건 (임계 20 미만이라 무효는 아니다)" -f $dropCount) -ForegroundColor Yellow
}

# ⑷ 캡처가 남았나 — 화면으로만 보이는 것(애니메이션·마나바)은 이것 없이는 못 본다
$shots = @(Get-ChildItem (Join-Path $ctrl ("{0}*.png" -f $Tag)) -ErrorAction SilentlyContinue)
if ($shots.Count -eq 0) {
  Write-Host '  ⚠ 캡처가 한 장도 없다 — 화면으로만 보이는 것은 판정 불가' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '================= 최종 판정 ================='
if ($verdictCode -eq 0) {
  Write-Host ("✅ 유효 — 클라 둘 다 끝까지 살아있음 · 멈춤 없음 · 버림 {0}건 · 캡처 {1}장" -f
              $dropCount, $shots.Count) -ForegroundColor Green
  Write-Host ('   ⚠ 「유효」는 **잰 값을 믿어도 된다**는 뜻이지 「게임이 된다」가 아니다.') -ForegroundColor DarkGray
  Write-Host ('     화면으로만 보이는 것은 캡처를 열어 봐야 한다:') -ForegroundColor DarkGray
  foreach ($s in $shots) { Write-Host ("       {0}" -f $s.FullName) -ForegroundColor DarkGray }
} else {
  Write-Host ('⛔ **판정 무효** — 위 수치는 믿을 수 없다') -ForegroundColor Red
  foreach ($w in $why) { Write-Host ("   · {0}" -f $w) -ForegroundColor Red }
}
# ★★ 끝나면 **치운다.** 2026-09-10 — 하네스가 아무것도 안 죽이고 끝나서
#   서버·프록시·게임 클라 둘이 남았고, 그것이 exe 를 잠가 다음 빌드가
#   `LNK1104` 로 조용히 실패했다. 그 결과 7분짜리 다음 런을 **옛 바이너리로** 돌렸다.
#   이 다섯 줄이 그 사고를 막는다.
Get-Process nox_server, nox_proxy, opennox, opennox-server, deskhost -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Write-Host '치움: 서버·프록시·클라 (다음 빌드가 exe 를 잠기지 않게)'

# ★ **종료 코드로 낸다** — 요약할 때 줄을 골라 읽어도 이건 못 건너뛴다.
#   백그라운드로 돌리면 알림에 "failed with exit code N" 으로 뜬다.
exit $verdictCode
