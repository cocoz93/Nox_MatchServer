# lobby_smoke.ps1 — 로비 서버 소켓 왕복 (MO 2단계, Docs/LOBBY_DESIGN.md 「시험」의 lobby_smoke).
#
# nox_lobby 를 시험용 포트(14005·14000)로 띄우고, 정품 클라가 보내는 순서(1단계 녹음) 그대로 TCP 로 말해 본다.
#   로비 찾기 → 605 가 로비 주소·포트를 알려 주나 → 로그인 376 · 379 → 목록(방 1줄 · 방장 주소)
#   → 채팅 로비 입·퇴장 → 게임 방 목록 → 방 참가(353 에 방장 주소) → QUIT 607
# 로비 로직의 꼴은 lobby_replay 가 녹음과 대조한다. 여기서는 소켓(accept · 줄 나누기 · 보내기 · 끊기)만 본다.
param([int]$ServPort = 14005, [int]$LobbyPort = 14000, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\nox_lobby.exe' }
$gameIp = '192.0.2.10'; $gameIpInt = '3221225994'   # 문서용 주소 — 192<<24 | 0<<16 | 2<<8 | 10
$room = "#NoxOrigin's_game"
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)   # 방 정보 덩어리 바이트가 그대로 오가게
function Open($port) {
  for ($i = 0; $i -lt 30; $i++) {
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
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--bind', '127.0.0.1', '--servserv-port', $ServPort, '--lobby-port', $LobbyPort,
  '--game-ip', $gameIp, '--seconds', '8', '--quiet')
try {
  Write-Host "로비 찾기 :$ServPort"
  $k = Open $ServPort
  Send $k @('verchk 32512 65556', 'verchk 9472 65538', 'lobcount 9472', 'whereto wn 1234 9472 65538 7726', 'QUIT')
  $a = ReadUntil $k '607'
  Check ((($a | ForEach-Object { Kind $_ }) -join ',') -eq '610,605,607') "답 순서 610 · 605 · 607 (받은 줄 $($a.Count))"
  Check (($a | Where-Object { (Kind $_) -eq '605' }) -like "*:127.0.0.1 $LobbyPort '0:*") "605 가 로비 주소 127.0.0.1:$LobbyPort 를 알려 준다"
  Check ($null -eq $k.r.ReadLine()) 'QUIT 뒤 로비가 끊는다'
  $k.c.Close()

  # 줄 끝 바이트 — 위 ReadLine 은 LF 만 와도 줄로 받으므로 CRLF 는 날바이트로 본다.
  #   정품 클라는 1단계 때 중계기가 CRLF 로 넘긴 줄을 받아 동작했다(wol_relay.ps1 이 줄마다 "`r`n")
  $k = Open $ServPort
  Send $k @('lobcount 9472')
  Start-Sleep -Milliseconds 300
  $buf = New-Object byte[] 64
  $n = $k.c.GetStream().Read($buf, 0, $buf.Length)
  Check ($latin1.GetString($buf, 0, $n) -ceq ": 610 u 1`r`n") '답 줄이 CRLF 로 끝난다 (610 날바이트)'
  $k.c.Close()

  Write-Host "로비 :$LobbyPort"
  $k = Open $LobbyPort
  Send $k @('CVERS 11020 9472', 'PASS supersecret', 'NICK wn', 'apgar 1234 0', 'SERIAL 7726',
            'USER UserName HostName irc.westwood.com :RealName', 'verchk 32512 720916', 'SETOPT 17,33')
  $b = ReadUntil $k '379'
  Check ((($b | ForEach-Object { Kind $_ }) -contains '376') -and ((Kind $b[-1]) -eq '379')) "로그인 끝 376 · 패치 없음 379 (받은 줄 $($b.Count))"
  Send $k @('SETCODEPAGE 949')
  Check ((ReadUntil $k '329') -contains ': 329 u 949') '코드페이지 329'

  Send $k @('LIST -1 37')
  $l1 = ReadUntil $k '323'
  $r326 = @($l1 | Where-Object { (Kind $_) -eq '326' }); $n327 = @($l1 | Where-Object { (Kind $_) -eq '327' }).Count
  Check ($r326.Count -eq 1 -and $n327 -eq 3) "첫 목록 = 방 $($r326.Count) · 채팅 로비 $n327"
  Check ($r326.Count -eq 1 -and $r326[0].Split(' ')[9] -eq $gameIpInt) '방 줄의 방장 주소 = 게임 서버 IP'

  Send $k @('JOIN #Lob_37_0 zot0blat')
  $j = ReadUntil $k '366'
  Check ((($j | ForEach-Object { Kind $_ }) -join ',') -eq 'user:JOIN,353,366') '채팅 로비 입장 JOIN · 353 · 366'
  Send $k @('PART #Lob_37_0')
  Check ((ReadUntil $k 'user:PART').Count -eq 1) '채팅 로비 퇴장 PART'

  Send $k @('LIST 37 37')
  $l2 = ReadUntil $k '323'
  Check (@($l2 | Where-Object { (Kind $_) -eq '326' }).Count -eq 1 -and @($l2 | Where-Object { (Kind $_) -eq '327' }).Count -eq 0) '게임 방 목록 = 방 1 · 채팅 로비 0'

  Send $k @("JOINGAME $room 1 ")
  $g = ReadUntil $k '366'
  Check ((($g | ForEach-Object { Kind $_ }) -join ',') -eq 'user:JOINGAME,332,353,366') '방 참가 JOINGAME · 332 · 353 · 366'
  Check (($g | Where-Object { (Kind $_) -eq '353' }) -like "*:@NoxOrigin,0,$gameIpInt wn,0,2130706433") '353 에 방장(게임 서버 IP) · 나(127.0.0.1)'

  Send $k @('QUIT')
  Check ((ReadUntil $k '607').Count -ge 1) 'QUIT 답 607'
  Check ($null -eq $k.r.ReadLine()) 'QUIT 뒤 로비가 끊는다'
  $k.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  if (-not $p.WaitForExit(15000)) { Check $false '로비가 --seconds 뒤에 스스로 끝나지 않음 — 강제 종료'; Stop-Process -Id $p.Id -Force }
}
$summary = Get-Content -LiteralPath $log -Encoding UTF8 | Where-Object { $_ -match '\[로비\] 끝' }
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
Write-Host "로비 출력: $summary"
# 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 로비가 센 접속·줄 수를 같이 본다
#   접속 = 로비 찾기 2(왕복 · CRLF) + 로비 1 · 보낸 클라 줄 = 5 + 1 + 15 = 21
#   받은 답 = 로비 찾기 3 + 610 1 + 로그인 4 · 329 · 목록 6 · 입장 3 · 퇴장 1 · 목록 3 · 참가 4 · 607 = 27
Check ($summary -match '접속 3 · 받은 줄 21 · 보낸 줄 27') '로비가 센 접속 3 · 받은 줄 21 · 보낸 줄 27'
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
