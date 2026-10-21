# v2_split_smoke.ps1 — 로비 하나 + 게임 서버 둘 소켓 왕복 (MO 6단계 v2 작업 4c · Docs/LOBBY_SPLIT_DESIGN.md 「다 됐다고 보는 것」).
#
# 같은 PC 에 게임 서버 둘 — A 127.0.0.1 · B 127.0.0.2(같은 UDP 포트 · IP 하나에 게임 서버 하나 — 방 목록 줄엔 IP 만 실린다)
#   ① 둘 다 로비에 등록 → 두 사람의 「새 방」이 방이 적은 게임 서버로 하나씩(NOXTOKEN 주소가 서로 다름) · 둘 다 방 번호 0
#   ② 토큰은 그 게임 서버의 그 실행에만 맞음 — A 방 0 토큰을 B(방 0 이 있음)에 내면 거절 · 제 게임 서버엔 JOIN_OK
#   ③ 게임 서버 A 껐다 켜기 → 로비 목록에서 A 방이 빠짐 · 다시 등록 · 옛 A 토큰은 새 A 에 거절(새 실행 키) · 새 방 토큰은 받음
#   ④ 로비 껐다 켜기 → 게임 서버 둘이 다시 붙어 등록하고 방을 다시 알림 → 새 로비 목록에 방이 돌아옴
#   ⑤ ★ 7단계 살아 있음(게임 서버 쪽) — 로비를 멈춰 세움(NtSuspendProcess — 로비 PC 전원 꺼짐 흉내: 끊김 신호가 안 온다) →
#     두 게임 서버가 15초 무소식에 끊고 다시 붙음 → 로비를 풀면 다시 등록(멈춘 로비의 대기 줄에 붙은 접속도 HELLO 를 받음)
# 빈 방은 30초에 닫히므로(접속 시도만 하고 안 들어감) ①~④를 그 안에 끝낸다(⑤ 는 방을 안 본다). 끝은 모두 닫기 이벤트 — 강제 종료는 응답이 없을 때만
param([int]$LobbyPort = 14400, [int]$LinkPort = 14410, [int]$UdpPort = 18611, [string]$Exe = '', [string]$LobbyExe = '')
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release'
if (-not $Exe) { $Exe = Join-Path $bin 'nox_server.exe' }
if (-not $LobbyExe) { $LobbyExe = Join-Path (Split-Path -Parent $Exe) 'nox_lobby.exe' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}
$latin1 = [Text.Encoding]::GetEncoding(28591)
function Open($port) {
  for ($i = 0; $i -lt 50; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $c.Connect('127.0.0.1', $port)
      $s = $c.GetStream(); $s.ReadTimeout = 3000
      $w = New-Object System.IO.StreamWriter($s, $latin1); $w.NewLine = "`r`n"
      return @{ c = $c; r = (New-Object System.IO.StreamReader($s, $latin1)); w = $w }
    } catch { Start-Sleep -Milliseconds 100 }
  }
  throw "접속 실패: 127.0.0.1:$port"
}
function Send($k, [string[]]$lines) { foreach ($l in $lines) { $k.w.WriteLine($l) }; $k.w.Flush() }
function Kind([string]$l) { $t = $l.Split(' '); if ($t[0] -eq ':') { $t[1] } elseif ($t[0].StartsWith(':')) { 'user:' + $t[1] } else { '?' } }
function ReadUntil($k, [string]$stop) {
  $got = @()
  while ($true) {
    try { $l = $k.r.ReadLine() } catch { return ,$got }
    if ($null -eq $l) { return ,$got }
    $got += $l
    if ((Kind $l) -eq $stop) { return ,$got }
  }
}
function ReadLog([string]$path) {
  try {
    $fs = New-Object System.IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd(); $sr.Close(); return $t
  } catch { return '' }
}
# 로비 기록에 정규식이 `n` 번 이상 나올 때까지(초 한도)
function WaitLog([string]$path, [string]$pattern, [int]$n, [int]$sec) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $sec) {
    if (([regex]::Matches((ReadLog $path), $pattern)).Count -ge $n) { return $true }
    Start-Sleep -Milliseconds 100
  }
  return $false
}
function Login([string]$nick) {
  $k = Open $LobbyPort
  Send $k @('NOXCLIENT 2', "NICK $nick", 'USER UserName HostName irc.westwood.com :RealName')
  $null = ReadUntil $k '376'
  return $k
}
# 「새 방」 — NOXTOKEN 줄의 [주소, 16진] (답이 없으면 빈 것)
function HostGame($k, [string]$nick) {
  Send $k @("JOINGAME #$($nick)'s_game 1 ")
  $g = ReadUntil $k '366'
  if ($g.Count -ge 1 -and $g[0].StartsWith('NOXTOKEN ')) { $t = $g[0].Split(' '); return @($t[1], $t[2]) }
  return @('', '')
}
function ListRooms($k) {
  Send $k @('LIST 37 37')
  $all = ReadUntil $k '323'
  return @($all | Where-Object { (Kind $_) -eq '326' } | ForEach-Object { $_.Split(' ')[3] })
}
$tryJoinHex = '00000e004a00610063006b00000000000000000000000000000000001c13616e01980309000000000000000049018501f07a' +
              '69000000020a3132333435363738393031323334353637383930000000009a030100000000007465737465720000d0af0000'   # 정품 접속 시도 100바이트(proto_test RecordedTryJoin)
function HexBytes([string]$h) { $b = New-Object byte[] ($h.Length / 2); for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }; return ,$b }
# 접속 시도 + 토큰 꼬리를 그 게임 서버 주소로(127.0.0.1 에서 보낸다 — 토큰의 클라 IP 와 같게) → JOIN_OK(0x14) 인가
function JoinOk([string]$tokHex, [string]$to) {
  $u = New-Object System.Net.Sockets.UdpClient (New-Object System.Net.IPEndPoint ([Net.IPAddress]::Parse('127.0.0.1')), 0)
  $u.Client.ReceiveTimeout = 2000
  $pkt = (HexBytes $tryJoinHex) + (HexBytes $tokHex) + [Text.Encoding]::ASCII.GetBytes('NXT1')
  [void]$u.Send($pkt, $pkt.Length, $to, $UdpPort)
  $ep = New-Object System.Net.IPEndPoint ([Net.IPAddress]::Any), 0
  try { $r = $u.Receive([ref]$ep) } catch { $r = $null }
  $u.Close()
  return ($null -ne $r -and $r.Length -eq 3 -and $r[2] -eq 0x14)
}
function RoomOf([string]$hex) { if ($hex.Length -lt 6) { return -1 }; [Convert]::ToInt32($hex.Substring(2, 2), 16) + 256 * [Convert]::ToInt32($hex.Substring(4, 2), 16) }
function StartGs([string]$ip, [string]$log) {
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
    '--port', $UdpPort, '--bind', $ip, '--register-ip', $ip, '--config', "`"$ini`"", '--lobby', "127.0.0.1:$LinkPort", '--quiet', '--no-results')
  $null = $p.Handle
  return $p
}
function StartLobby([string]$log) {
  $p = Start-Process -FilePath $LobbyExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
    '--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--seconds', '120', '--quiet')
  $null = $p.Handle
  return $p
}
# ★ 7단계 ⑤ — 프로세스 멈춰 세우기 · 풀기(멈춘 프로세스도 OS 는 TCP 를 살려 두어 상대에게 끊김 신호가 안 간다)
Add-Type -Namespace NoxSmoke -Name Proc -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
'@
function StopProc($p, [string]$kind) {
  try { [void][Threading.EventWaitHandle]::OpenExisting("$($kind)_stop_$($p.Id)").Set() } catch { }
  if (-not $p.WaitForExit(15000)) { Check $false "$kind $($p.Id) 이 닫기 이벤트 뒤에 끝나지 않음 — 강제 종료"; Stop-Process -Id $p.Id -Force }
}

$waitMap = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\So_LOD\So_LOD.map'
if (-not (Test-Path -LiteralPath $waitMap)) { Write-Host "  [건너뜀] 대기 맵(So_LOD)이 없다 — 새 방을 못 만들어 이 시험을 건너뜀: $waitMap"; exit 0 }
$tag = $PID
$ini = Join-Path $env:TEMP "v2split_$tag.ini"
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $('cd' * 32)`r`n")
$l1 = Join-Path $env:TEMP "v2split_lobby1_$tag.txt"; $l2 = Join-Path $env:TEMP "v2split_lobby2_$tag.txt"
$ga1 = Join-Path $env:TEMP "v2split_gsA1_$tag.txt"; $ga2 = Join-Path $env:TEMP "v2split_gsA2_$tag.txt"; $gb = Join-Path $env:TEMP "v2split_gsB_$tag.txt"
$lp = StartLobby $l1
$pa = StartGs '127.0.0.1' $ga1
$pb = StartGs '127.0.0.2' $gb
$pa2 = $null; $lp2 = $null
try {
  Write-Host "로비 :$LobbyPort · 연결 :$LinkPort · 게임 서버 A 127.0.0.1:$UdpPort · B 127.0.0.2:$UdpPort"
  Check (WaitLog $l1 "등록 — 실행" 2 10) '① 게임 서버 둘 등록(로비 기록 둘)'

  $alice = Login 'alice'; $bob = Login 'bob'
  $ta = HostGame $alice 'alice'; $tb = HostGame $bob 'bob'
  $addrs = @($ta[0], $tb[0]) | Sort-Object
  Check (($addrs -join ',') -eq "127.0.0.1:$UdpPort,127.0.0.2:$UdpPort" -and (RoomOf $ta[1]) -eq 0 -and (RoomOf $tb[1]) -eq 0) "① 새 방 둘 → 게임 서버 하나씩 · 둘 다 방 0 ($($ta[0]) · $($tb[0]))"
  $aliceOnA = $ta[0] -eq "127.0.0.1:$UdpPort"
  $tokA = if ($aliceOnA) { $ta[1] } else { $tb[1] }   # A(127.0.0.1) 방 0 토큰
  $tokB = if ($aliceOnA) { $tb[1] } else { $ta[1] }   # B(127.0.0.2) 방 0 토큰
  $hostOnB = if ($aliceOnA) { 'bob' } else { 'alice' }

  Check ((JoinOk $tokA '127.0.0.1') -and (JoinOk $tokB '127.0.0.2')) '② 제 게임 서버에 낸 토큰 → JOIN_OK 둘'
  Check (-not (JoinOk $tokA '127.0.0.2') -and -not (JoinOk $tokB '127.0.0.1')) '② 남의 게임 서버(같은 방 번호 0 이 있음)에 낸 토큰 → 거절 둘(게임 서버마다 다른 키)'

  # ③ 게임 서버 A 껐다 켜기
  StopProc $pa 'nox_server'
  Start-Sleep -Milliseconds 500
  $rooms1 = @(ListRooms $alice)   # 하나뿐이면 함수 출력이 글자 하나로 풀려 [0] 이 첫 글자가 된다
  Check ($rooms1.Count -eq 1 -and $rooms1[0] -eq "#$($hostOnB)'s_game") "③ A 를 끄면 로비 목록에서 A 방이 빠짐(남은 방 $($rooms1 -join ','))"
  Start-Sleep -Milliseconds 1100   # 실행 번호(뜬 유닉스 초)가 앞 실행과 꼭 다르게
  $pa2 = StartGs '127.0.0.1' $ga2
  Check (WaitLog $l1 "등록 — 실행" 3 10) '③ 다시 켠 A 가 다시 등록(로비 기록 셋째)'
  $carol = Login 'carol'
  $tc = HostGame $carol 'carol'
  Check ($tc[0] -eq "127.0.0.1:$UdpPort" -and (RoomOf $tc[1]) -eq 0) "③ 다음 새 방은 방이 적은 새 A 로 · 방 0 ($($tc[0]))"
  Check ((-not (JoinOk $tokA '127.0.0.1')) -and (JoinOk $tc[1] '127.0.0.1')) '③ 옛 A 의 방 0 토큰 → 새 A 가 거절(새 실행 키) · 새 방 0 토큰 → JOIN_OK'

  # ④ 로비 껐다 켜기 — 게임 서버 방은 그대로 · 다시 붙어 등록하고 방을 다시 알림
  $alice.c.Close(); $bob.c.Close(); $carol.c.Close()
  StopProc $lp 'nox_lobby'
  $lp2 = StartLobby $l2
  Check (WaitLog $l2 "등록 — 실행" 2 12) '④ 새 로비에 게임 서버 둘이 다시 붙어 등록(다시 붙기 3초)'
  Start-Sleep -Milliseconds 300
  $dave = Login 'dave'
  $rooms2 = @(ListRooms $dave | Sort-Object)
  Check (($rooms2 -join ',') -eq (@("#$($hostOnB)'s_game", "#carol's_game") | Sort-Object) -join ',') "④ 새 로비 목록에 게임 서버 방이 돌아옴 ($($rooms2 -join ','))"
  $dave.c.Close()

  # ⑤ ★ 7단계 — 로비 멈춰 세우기 → 두 게임 서버 무소식 끊기 → 풀면 다시 등록
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [void][NoxSmoke.Proc]::NtSuspendProcess($lp2.Handle)
  $lobbySuspended = $true
  $deadA = WaitLog $ga2 '\[로비 연결\] 15초 동안 받은 줄 없음' 1 25
  $deadB = WaitLog $gb '\[로비 연결\] 15초 동안 받은 줄 없음' 1 5
  $waited = [int]$sw.Elapsed.TotalSeconds
  [void][NoxSmoke.Proc]::NtResumeProcess($lp2.Handle)
  $lobbySuspended = $false
  Check ($deadA -and $deadB) "⑤ 로비를 멈춰 세우면 두 게임 서버가 15초 무소식에 끊음(기록 한 줄씩 · ${waited}초)"
  Check (WaitLog $l2 "등록 — 실행" 4 15) '⑤ 로비를 풀면 두 게임 서버가 다시 붙어 등록(새 로비 기록 넷째)'
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  if ($lobbySuspended) { [void][NoxSmoke.Proc]::NtResumeProcess($lp2.Handle) }   # 멈춘 채면 닫기 이벤트를 못 받는다
  foreach ($x in @($pa2, $pb)) { if ($x) { StopProc $x 'nox_server' } }
  if ($pa -and -not $pa.HasExited) { StopProc $pa 'nox_server' }
  foreach ($x in @($lp2, $lp)) { if ($x -and -not $x.HasExited) { StopProc $x 'nox_lobby' } }
}
$tl1 = ReadLog $l1; $tl2 = ReadLog $l2; $tb2 = ReadLog $gb; $ta2 = ReadLog $ga2
# ★ 7단계 ⑤ — 새 로비: ④ 에 둘 + ⑤ 에 다시 둘 = 등록 4 · 로비 쪽은 멈췄던 것이라 무소식 끊음 0(풀린 뒤 옛 연결은 끊김 사건으로 받음) · 게임 서버 PING 을 받음
$okL2 = $false
if ($tl2 -match '게임 서버 연결 (\d+)\(확인 (\d+) · 확인 실패 0 · 등록 4 · 등록 거절 0 · 차례 틀림 0\) · 연결 줄 받음 \d+\(PING (\d+)\).* · 무소식 끊음 0') {
  $okL2 = [int]$Matches[1] -ge 4 -and [int]$Matches[2] -eq 4 -and [int]$Matches[3] -ge 1
}
Check $okL2 '끝 줄 — 새 로비: 확인 4 · 등록 4 · 거절 0 · PING 받음 1 이상 · 무소식 끊음 0'
# 게임 서버 — ④ 로비 둘 + ⑤ 무소식 끊고 다시 붙음 = 붙음 3 · 끊김 2 · 등록 3 · 무소식 끊음 1(끊기 요청 1) · PING 보냄 1 이상
$gsEnd = '\[로비 연결\] 끝 — 붙음 3 · 끊김 2 · 등록 3 · .* · PING\(받음 (\d+) · 보냄 (\d+)\) · 무소식 끊음 1\(끊기 요청 1\)'
Check ($tb2 -match $gsEnd -and [int]$Matches[2] -ge 1) '끝 줄 — B: 붙음 3(로비 둘 + ⑤ 다시) · 끊김 2 · 등록 3 · PING 보냄 · 무소식 끊음 1(끊기 요청 1)'
Check ($ta2 -match $gsEnd -and [int]$Matches[2] -ge 1) '끝 줄 — 다시 켠 A: 붙음 3 · 끊김 2 · 등록 3 · PING 보냄 · 무소식 끊음 1(끊기 요청 1)'
Check ($tl1 -match '끊김 — 방 1개 목록에서 내림') '로비 기록 — A 를 끌 때 방 하나 내림'
Remove-Item -LiteralPath $ini, $l1, $l2, $ga1, $ga2, $gb -ErrorAction SilentlyContinue
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
