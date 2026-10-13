# lobby_smoke.ps1 — 로비 서버 소켓 왕복 (MO 2단계, Docs/LOBBY_DESIGN.md 「시험」의 lobby_smoke).
#
# nox_lobby 를 시험용 포트(14000)로 띄우고, 정품 클라가 보내는 순서(1단계 녹음) 그대로 TCP 로 말해 본다.
#   로그인 376 · 379 → 목록(방 1줄 · 방장 주소)
#   → 채팅 로비 입·퇴장 → 게임 방 목록 → 방 참가(353 에 방장 주소) → QUIT 607
# 로비 로직의 꼴은 lobby_replay 가 녹음과 대조한다. 여기서는 소켓(accept · 줄 나누기 · 보내기 · 끊기)만 본다.
# ★ 10-14 작업 8b — 로비 찾기(4005 · 605)는 라우터로 떼어 냈다 — 그 왕복 · 답 줄 CRLF 날바이트 확인은 router_smoke.ps1
param([int]$LobbyPort = 14000, [string]$Exe = '')
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
# ★ 10-10 토큰 작업 3 — 로비는 설정 파일(키 · 방 목록)이 있어야 뜬다. 키는 시험용 고정값(게임 서버와 맞출 일이 없다)
$ini = Join-Path $env:TEMP ("lobby_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $('ab' * 32)`r`n[rooms]`r`nNoxOrigin = 0`r`n")
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort,
  '--game-ip', $gameIp, '--seconds', '8', '--quiet')
try {
  Write-Host "로비 :$LobbyPort"
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
  Check ((($g | ForEach-Object { Kind $_ }) -join ',') -eq '?,user:JOINGAME,332,353,366') '방 참가 NOXTOKEN · JOINGAME · 332 · 353 · 366'
  $nt = $g[0].Split(' ')
  Check ($nt.Count -eq 3 -and $nt[0] -eq 'NOXTOKEN' -and $nt[1] -eq "${gameIp}:18590" -and $nt[2] -match '^[0-9a-f]{102}$') "NOXTOKEN 게임 서버 주소 · 토큰 16진 102자 ($($g[0].Length)글자)"
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
Remove-Item -LiteralPath $ini -ErrorAction SilentlyContinue
Write-Host "로비 출력: $summary"
# 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 로비가 센 접속·줄 수를 같이 본다
#   접속 = DLL 없는 로그인 1 + 로비 1 · 보낸 클라 줄 = 4 + 16 = 20
#   받은 답 = DLL 없는 로그인 6(375 · 372 · 안내 372 둘 · 376 · 607) + 로그인 4 · 329 · 목록 6 · 입장 3 · 퇴장 1 · 목록 3 · 참가 5(NOXTOKEN 포함) · 607 = 30
#   (★ 10-14 작업 8b 전 = 로비 찾기 둘을 더해 접속 4 · 받은 줄 26 · 보낸 줄 34)
Check ($summary -match '접속 2 · 받은 줄 20 · 보낸 줄 30') '로비가 센 접속 2 · 받은 줄 20 · 보낸 줄 30'
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
