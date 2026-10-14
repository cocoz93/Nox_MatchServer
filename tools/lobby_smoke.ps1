# lobby_smoke.ps1 — 채널 안 로비 소켓 왕복 (MO 2단계 Docs/LOBBY_DESIGN.md 「시험」 · ★ 10-14 플랜 B 작업 8d 채널로 옮김).
#
# nox_server 를 로비 방식(--lobby-port 14000 · 토큰 방식 · 방 없이 시작)으로 띄우고, 정품 클라가 보내는 순서(1단계 녹음) 그대로 TCP 로 말해 본다.
#   DLL 없는 로그인(인사말 안내) → 로그인 376 · 379 → 목록(방 0줄 · 채팅 로비 3) → 채팅 로비 입·퇴장 → 게임 방 목록(0줄)
#   → 없는 방 참가(답 없음 · 로비 기록) → QUIT 607 · 끊김
# 로비 로직의 꼴은 lobby_replay 가 녹음과 대조한다. 여기서는 채널의 소켓(네트워크 스레드 TCP · 같은 큐 · 게임 스레드 로비)만 본다.
# ★ 10-14 작업 8b — 로비 찾기(4005 · 605)는 라우터로 떼어 냈다 — 그 왕복 · 답 줄 CRLF 날바이트 확인은 router_smoke.ps1
# ★ 10-14 작업 8d — 옛 nox_lobby(따로 뜬 로비 · 설정 [rooms] 의 방 하나 · --seconds 로 스스로 끝남) 대신 채널. 방은 로비가 만드는 것만
#   목록에 나오므로(작업 10) 여기서는 0줄이다. 끝은 닫기 이벤트(nox_server_stop_<pid>) — 강제 종료는 응답이 없을 때만
param([int]$LobbyPort = 14000, [int]$UdpPort = 18591, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\nox_server.exe' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)   # 방 정보 덩어리 바이트가 그대로 오가게
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
# $stop 종류의 줄이 올 때까지 읽는다. 시간 초과·끊김이면 거기까지
function ReadUntil($k, [string]$stop) {
  $got = @()
  while ($true) {
    try { $l = $k.r.ReadLine() } catch { return ,$got }
    if ($null -eq $l) { return ,$got }
    $got += $l
    if ((Kind $l) -eq $stop) { return ,$got }
  }
}

$log = Join-Path $env:TEMP ("lobby_smoke_{0}.txt" -f $PID)
# ★ 10-10 토큰 작업 3 — 토큰 키가 있어야 뜬다. 키는 시험용 고정값(★ 8d — 옛 [rooms] 는 없앴다)
$ini = Join-Path $env:TEMP ("lobby_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $('ab' * 32)`r`n")
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--port', $UdpPort, '--config', "`"$ini`"", '--lobby-port', $LobbyPort, '--lobby-bind', '127.0.0.1', '--quiet')
$null = $p.Handle   # 종료 코드를 읽으려면 미리 잡아 둔다
try {
  Write-Host "채널 로비 :$LobbyPort"
  # DLL 없는 로그인(NOXCLIENT 없음) — 끊지 않고 로그인 끝(376)까지, 인사말에 패치 안내(10-11 — ERROR 로 끊으면 정품이 멈춘 듯 보였다)
  $k = Open $LobbyPort
  Send $k @('CVERS 11020 9472', 'NICK plain', 'USER UserName HostName irc.westwood.com :RealName')
  $x = ReadUntil $k '376'
  Check (($x | Where-Object { $_ -like ': 372 u :- `*`*`* Nox_Origin client patch*' }).Count -eq 1 -and (Kind $x[-1]) -eq '376') "DLL 없는 로그인 → 376 까지 · 인사말에 패치 안내 (받은 줄 $($x.Count))"
  Send $k @('QUIT')
  Check ((ReadUntil $k '607').Count -ge 1) 'DLL 없는 접속도 QUIT 답 607'
  $k.c.Close()

  $k = Open $LobbyPort
  Send $k @('NOXCLIENT 1', 'CVERS 11020 9472', 'PASS supersecret', 'NICK wn', 'apgar 1234 0', 'SERIAL 7726',
            'USER UserName HostName irc.westwood.com :RealName', 'verchk 32512 720916', 'SETOPT 17,33')
  $b = ReadUntil $k '379'
  Check ((($b | ForEach-Object { Kind $_ }) -contains '376') -and ((Kind $b[-1]) -eq '379')) "로그인 끝 376 · 패치 없음 379 (받은 줄 $($b.Count))"
  Send $k @('SETCODEPAGE 949')
  Check ((ReadUntil $k '329') -contains ': 329 u 949') '코드페이지 329'

  Send $k @('LIST -1 37')
  $l1 = ReadUntil $k '323'
  $n326 = @($l1 | Where-Object { (Kind $_) -eq '326' }).Count; $n327 = @($l1 | Where-Object { (Kind $_) -eq '327' }).Count
  Check ($n326 -eq 0 -and $n327 -eq 3 -and (Kind $l1[0]) -eq '321') "첫 목록 = 방 $n326(방 없이 시작) · 채팅 로비 $n327"

  Send $k @('JOIN #Lob_37_0 zot0blat')
  $j = ReadUntil $k '366'
  Check ((($j | ForEach-Object { Kind $_ }) -join ',') -eq 'user:JOIN,353,366') '채팅 로비 입장 JOIN · 353 · 366'
  Send $k @('PART #Lob_37_0')
  Check ((ReadUntil $k 'user:PART').Count -eq 1) '채팅 로비 퇴장 PART'

  Send $k @('LIST 37 37')
  $l2 = ReadUntil $k '323'
  Check ((($l2 | ForEach-Object { Kind $_ }) -join ',') -eq '321,323') '게임 방 목록 = 0줄(321 · 323 만)'

  # 없는 방 참가 — 로비는 답하지 않는다(로비 기록에 [모르는 방]). 바로 QUIT 을 보내 그 답이 첫 줄인지로 「답 없음」을 본다
  Send $k @("JOINGAME #nobody's_game 1 ", 'QUIT')
  $q = ReadUntil $k '607'
  Check ($q.Count -eq 1 -and (Kind $q[0]) -eq '607') "없는 방 참가엔 답 없음 · QUIT 답 607 (받은 줄 $($q.Count))"
  Check ($null -eq $k.r.ReadLine()) 'QUIT 뒤 채널이 끊는다'
  $k.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  # 닫기 이벤트로 끝낸다(nox_server 는 스스로 안 끝난다). 응답이 없을 때만 강제 종료
  try { [void][Threading.EventWaitHandle]::OpenExisting("nox_server_stop_$($p.Id)").Set() } catch { }
  if (-not $p.WaitForExit(15000)) { Check $false '채널이 닫기 이벤트 뒤에 끝나지 않음 — 강제 종료'; Stop-Process -Id $p.Id -Force }
}
$all = @(Get-Content -LiteralPath $log -Encoding UTF8)
$summary = $all | Where-Object { $_ -match '\[로비\] 끝' }
$unknown = @($all | Where-Object { $_ -match "\[로비\] \[모르는 방\] JOINGAME #nobody's_game" }).Count
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $ini -ErrorAction SilentlyContinue
Write-Host "채널 출력: $summary"
Check ($p.ExitCode -eq 0) "채널 종료 코드 0 ($($p.ExitCode))"
Check ($unknown -eq 1) "로비 기록 [모르는 방] 한 줄 ($unknown)"
# 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 로비가 센 접속·줄 수를 네트워크 스레드가 센 TCP 줄 수와 같이 본다
#   접속 = DLL 없는 로그인 1 + 로비 1 · 보낸 클라 줄 = 4 + 16 = 20
#   받은 답 = DLL 없는 로그인 6(375 · 372 · 안내 372 둘 · 376 · 607) + 로그인 4 · 329 · 목록 5 · 입장 3 · 퇴장 1 · 목록 2 · 없는 방 0 · 607 = 23
#   (★ 10-14 작업 8d 전 nox_lobby = 설정 방 하나가 있어 목록 둘에 326 이 한 줄씩 · 참가 5 → 받은 줄 20 · 보낸 줄 30)
Check ($summary -match '접속 2 · 받은 줄 20 · 보낸 줄 23 · 토큰 0 · TCP\(받음 20 · 보냄 23 · 끊긴 접속으로 0 · 한도 0\)') '로비 셈 = TCP 셈 — 접속 2 · 받은 줄 20 · 보낸 줄 23'
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
