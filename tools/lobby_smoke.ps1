# lobby_smoke.ps1 — 채널 안 로비 소켓 왕복 (MO 2단계 Docs/LOBBY_DESIGN.md 「시험」 · ★ 10-14 플랜 B 작업 8d 채널로 옮김).
#
# nox_server 를 로비 방식(--lobby-port 14000 · 토큰 방식 · 방 없이 시작)으로 띄우고, 정품 클라가 보내는 순서(1단계 녹음) 그대로 TCP 로 말해 본다.
#   DLL 없는 로그인(인사말 안내) → 로그인 376 · 379 → 목록(방 0줄 · 채팅 로비 5 — ★ 10-20 v2 작업 3b) → 채팅 로비 입·퇴장 → 게임 방 목록(0줄)
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

# ★ 10-14 작업 10a — 새 방의 대기 맵(채널의 기본 후보 So_LOD). 게임 설치본에 있어야 새 방 · UDP 접속 시도를 본다(없으면 건너뛰고 셈도 그만큼)
$waitMap = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\So_LOD\So_LOD.map'
$haveWaitMap = Test-Path -LiteralPath $waitMap
$log = Join-Path $env:TEMP ("lobby_smoke_{0}.txt" -f $PID)
# ★ 10-10 토큰 작업 3 — 토큰 키가 있어야 뜬다. 키는 시험용 고정값(★ 8d — 옛 [rooms] 는 없앴다)
$ini = Join-Path $env:TEMP ("lobby_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $('ab' * 32)`r`n")
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--port', $UdpPort, '--config', "`"$ini`"", '--lobby-port', $LobbyPort, '--lobby-bind', '127.0.0.1', '--quiet', '--no-results')   # ★ 10-19 판 결과 기록 끄기
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
  Check ($n326 -eq 0 -and $n327 -eq 5 -and (Kind $l1[0]) -eq '321') "첫 목록 = 방 $n326(방 없이 시작) · 채팅 로비 $n327"

  Send $k @('JOIN #Lob_37_0 zot0blat')
  $j = ReadUntil $k '366'
  Check ((($j | ForEach-Object { Kind $_ }) -join ',') -eq 'user:JOIN,353,366') '채팅 로비 입장 JOIN · 353 · 366'
  Send $k @('PART #Lob_37_0')
  Check ((ReadUntil $k 'user:PART').Count -eq 1) '채팅 로비 퇴장 PART'

  Send $k @('LIST 37 37')
  $l2 = ReadUntil $k '323'
  Check ((($l2 | ForEach-Object { Kind $_ }) -join ',') -eq '321,323') '게임 방 목록 = 0줄(321 · 323 만)'

  # 없는 방 참가 — 로비는 답하지 않는다(로비 기록에 [모르는 방]). 바로 SETCODEPAGE 를 보내 그 답(329)이 첫 줄인지로 「답 없음」을 본다
  Send $k @("JOINGAME #nobody's_game 1 ", 'SETCODEPAGE 949')
  $q = ReadUntil $k '329'
  Check ($q.Count -eq 1 -and (Kind $q[0]) -eq '329') "없는 방(남의 이름 채널) 참가엔 답 없음 (받은 줄 $($q.Count))"

  if ($haveWaitMap) {
    # ★ 10-14 작업 10a — 「새 방」 — 제 이름 채널(#wn's_game)로 온 없는 방 참가 → 채널이 방을 만들고 그 방 토큰(정품 Host Game 을 DLL 이 이 참가로 돌린다)
    Send $k @("JOINGAME #wn's_game 1 ")
    $g = ReadUntil $k '366'
    Check ((($g | ForEach-Object { Kind $_ }) -join ',') -eq '?,user:JOINGAME,332,353,366') "새 방 — NOXTOKEN · JOINGAME · 332 · 353 · 366 (받은 줄 $($g.Count))"
    $nt = $g[0].Split(' ')
    Check ($nt.Count -eq 3 -and $nt[0] -eq 'NOXTOKEN' -and $nt[1] -eq "127.0.0.1:$UdpPort" -and $nt[2] -match '^[0-9a-f]{102}$') "NOXTOKEN = 붙은 채널 주소 127.0.0.1:$UdpPort · 토큰 16진 102자"
    Check (($g | Where-Object { (Kind $_) -eq '353' }) -like '*:@wn,0,2130706433 wn,0,2130706433') '353 방장 = 만든 사람(wn) · 게임 서버 = 붙은 채널 주소'
    $tokA = $nt[2]
    Send $k @('LIST 37 37')
    $l3all = ReadUntil $k '323'   # 함수가 배열을 한 덩어리로 돌려주므로 변수에 받은 뒤 거른다
    $l3 = @($l3all | Where-Object { (Kind $_) -eq '326' })
    Check ($l3.Count -eq 1 -and $l3[0].Split(' ')[3] -eq "#wn's_game") "다음 목록에 새 방 한 줄 (#wn's_game · $($l3.Count)줄)"

    # 다른 사람(bob) — 목록에서 그 방을 보고 참가하면 같은 방 토큰(방 번호 = 토큰 [1..2] 작은 끝수)
    $k2 = Open $LobbyPort
    Send $k2 @('NOXCLIENT 1', 'NICK bob', 'USER UserName HostName irc.westwood.com :RealName')
    $null = ReadUntil $k2 '376'
    Send $k2 @('LIST 37 37')
    $l4all = ReadUntil $k2 '323'
    $l4 = @($l4all | Where-Object { (Kind $_) -eq '326' })
    Send $k2 @("JOINGAME #wn's_game 1 ")
    $g2 = ReadUntil $k2 '366'
    $tokB = if ($g2.Count -ge 1) { $g2[0].Split(' ')[2] } else { '' }
    function RoomOf([string]$hex) { if ($hex.Length -lt 6) { return -1 }; [Convert]::ToInt32($hex.Substring(2, 2), 16) + 256 * [Convert]::ToInt32($hex.Substring(4, 2), 16) }
    Check ($l4.Count -eq 1 -and $g2.Count -eq 5 -and (RoomOf $tokA) -eq (RoomOf $tokB) -and (RoomOf $tokA) -ge 0) "bob — 목록 1줄 · 같은 방 토큰(방 $(RoomOf $tokA) · $(RoomOf $tokB))"
    Send $k2 @('QUIT')
    $null = ReadUntil $k2 '607'
    $k2.c.Close()

    # UDP 접속 시도 + 토큰 꼬리(DLL 이 붙이는 꼴 — 토큰 51 + NXT1) → 0x14 SERVER_JOIN_OK. 서명 한 바이트를 바꾼 토큰은 받지 않는다
    $tryJoinHex = '00000e004a00610063006b00000000000000000000000000000000001c13616e01980309000000000000000049018501f07a' +
                  '69000000020a3132333435363738393031323334353637383930000000009a030100000000007465737465720000d0af0000'   # 정품 접속 시도 100바이트(proto_test RecordedTryJoin)
    function HexBytes([string]$h) { $b = New-Object byte[] ($h.Length / 2); for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }; return ,$b }
    function TryJoin([string]$tokHex) {
      $u = New-Object System.Net.Sockets.UdpClient (New-Object System.Net.IPEndPoint ([Net.IPAddress]::Parse('127.0.0.1')), 0)
      $u.Client.ReceiveTimeout = 2000
      $pkt = (HexBytes $tryJoinHex) + (HexBytes $tokHex) + [Text.Encoding]::ASCII.GetBytes('NXT1')
      [void]$u.Send($pkt, $pkt.Length, '127.0.0.1', $UdpPort)
      $ep = New-Object System.Net.IPEndPoint ([Net.IPAddress]::Any), 0
      try { $r = $u.Receive([ref]$ep) } catch { $r = $null }
      $u.Close()
      return ,$r
    }
    $ok = TryJoin $tokA
    Check ($null -ne $ok -and $ok.Length -eq 3 -and $ok[2] -eq 0x14) "UDP 접속 시도 + 새 방 토큰 → 0x14 SERVER_JOIN_OK ($(if ($ok) { ($ok | ForEach-Object { $_.ToString('x2') }) -join ' ' } else { '답 없음' }))"
    $bad = $tokB.Substring(0, 100) + $(if ($tokB.Substring(100, 2) -eq '00') { '01' } else { '00' })   # 서명 마지막 바이트를 바꿈
    $no = TryJoin $bad
    Check (-not ($null -ne $no -and $no.Length -eq 3 -and $no[2] -eq 0x14)) '서명을 바꾼 토큰 → JOIN_OK 아님'
  } else {
    Write-Host "  [건너뜀] 대기 맵(So_LOD)이 없다 — 새 방 · UDP 접속 시도 확인을 건너뜀: $waitMap"
  }

  Send $k @('QUIT')
  $q2 = ReadUntil $k '607'
  Check ($q2.Count -eq 1 -and (Kind $q2[0]) -eq '607') "QUIT 답 607 (받은 줄 $($q2.Count))"
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
$newRoomLog = @($all | Where-Object { $_ -match "\[로비\] \[새 방\] wn → #wn's_game \(게임 방 0 · 맵 so_lod\)" }).Count
$roomMade = @($all | Where-Object { $_ -match "^\[방\] 0 만듦 — 이름 wn's game · 맵 so_lod · 모드 chat · 비면 닫음" }).Count
$joinOk = @($all | Where-Object { $_ -match '^\[입장\] 접속 시도 받음 — 127\.0\.0\.1:\d+ · 방 0 · 로비 이름 wn$' }).Count
$joinNo = @($all | Where-Object { $_ -match '^\[입장\] 접속 시도 거절 — 127\.0\.0\.1:\d+ · 까닭 ' }).Count
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $ini -ErrorAction SilentlyContinue
Write-Host "채널 출력: $summary"
Check ($p.ExitCode -eq 0) "채널 종료 코드 0 ($($p.ExitCode))"
Check ($unknown -eq 1) "로비 기록 [모르는 방] 한 줄 ($unknown)"
# 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 로비가 센 접속·줄 수를 네트워크 스레드가 센 TCP 줄 수와 같이 본다
#   대기 맵 없음 — 접속 2(DLL 없는 로그인 · wn) · 보낸 클라 줄 = 4 + 17 = 21
#     받은 답 = DLL 없는 로그인 6(375 · 372 · 안내 372 둘 · 376 · 607) + 로그인 4 · 329 · 목록 7(321 · 327 다섯 · 323 — ★ 10-20 v2 작업 3b 대화방 다섯) · 입장 3 · 퇴장 1 · 목록 2 · 없는 방 0 · 329 · 607 = 26
#   대기 맵 있음(★ 10-14 작업 10a) — 위에 wn 새 방 참가 5 · 목록 3(321 · 326 · 323)과 bob 접속(줄 6 — 로그인 3 · 목록 3 · 참가 5 · 607) → 접속 3 · 받은 줄 29 · 보낸 줄 46 · 토큰 2
#   (★ 10-14 작업 8d 전 nox_lobby = 설정 방 하나가 있어 목록 둘에 326 이 한 줄씩 · 참가 5 → 받은 줄 20 · 보낸 줄 30)
if ($haveWaitMap) {
  Check ($summary -match '접속 3 · 받은 줄 29 · 보낸 줄 46 · 토큰 2 · TCP\(받음 29 · 보냄 46 · 끊긴 접속으로 0 · 한도 0\)') '로비 셈 = TCP 셈 — 접속 3 · 받은 줄 29 · 보낸 줄 46 · 토큰 2'
  Check ($newRoomLog -eq 1 -and $roomMade -eq 1) "채널 기록 — [새 방] wn $newRoomLog 줄 · [방] 0 만듦(wn's game · so_lod · chat · 비면 닫음) $roomMade 줄"
  Check ($joinOk -eq 1 -and $joinNo -eq 1) "입장 판정 — 접속 시도 받음(방 0 · wn) $joinOk · 거절 $joinNo"
} else {
  Check ($summary -match '접속 2 · 받은 줄 21 · 보낸 줄 26 · 토큰 0 · TCP\(받음 21 · 보냄 26 · 끊긴 접속으로 0 · 한도 0\)') '로비 셈 = TCP 셈 — 접속 2 · 받은 줄 21 · 보낸 줄 26'
}
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
