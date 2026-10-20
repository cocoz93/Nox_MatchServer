# nox_lobby_smoke.ps1 — 로비 프로세스 nox_lobby 소켓 왕복 + 가짜 게임 서버 (MO 6단계 v2 작업 3c · Docs/LOBBY_SPLIT_DESIGN.md).
#
# nox_lobby 를 띄우고(클라 포트 · 게임 서버 연결 포트 · --seconds 로 스스로 끝남) 이 스크립트가 게임 서버 노릇을 한다.
#   ① 가짜 게임 서버 — HELLO 받음 → AUTH(연결 키 서명) → REGISTER(127.0.0.2:18590) → ROOM(alice 방) · 서명 틀린 둘째는 BYE 뒤 끊김
#   ② 클라 — 로그인 → 목록에 alice 방(방장 주소 칸 = 등록 주소 127.0.0.2) → 대화방 둘이서 채팅 한 줄
#   ③ 새 방 — 클라 JOINGAME #wn's_game → 게임 서버에 CREATE(만든 사람 wn) → ROOM + CREATED → 클라에 미룬 참가 답(NOXTOKEN 127.0.0.2:18590)
#   ③-2 답 없는 새 방 — 로비 한도 5초에 시간 초과(실행 파일 루프의 Tick)
#   ④ 판 결과 RESULT → 로비 기록 [판 결과] · 게임 서버 끊김 → 목록 0줄 · QUIT 607
#   끝 줄의 셈(접속 · 줄 · 토큰 · 새 방 · 게임 서버 확인 · 등록 · 판 결과)을 대조한다 — 0 은 「없었다」와 「못 읽었다」를 안 가르므로 지나간 양을 같이 본다
# 로비 로직의 갈래는 lobby_test(MirrorUnitTests · PendingRoomUnitTests · ChatUnitTests)가 본다. 여기서는 실행 파일의 소켓 · 사건 가르기 · 루프만.
param([int]$LobbyPort = 14200, [int]$LinkPort = 14210, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\nox_lobby.exe' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)   # 방 정보 덩어리 바이트가 그대로 오가게
$utf8 = New-Object Text.UTF8Encoding($false)       # 연결 줄은 UTF-8 글
function Open($port, $enc) {
  for ($i = 0; $i -lt 50; $i++) {
    try {
      $c = New-Object System.Net.Sockets.TcpClient
      $c.Connect('127.0.0.1', $port)
      $s = $c.GetStream(); $s.ReadTimeout = 3000
      $w = New-Object System.IO.StreamWriter($s, $enc); $w.NewLine = "`r`n"
      return @{ c = $c; r = (New-Object System.IO.StreamReader($s, $enc)); w = $w }
    } catch { Start-Sleep -Milliseconds 100 }
  }
  throw "접속 실패: 127.0.0.1:$port"
}
function Send($k, [string[]]$lines) { foreach ($l in $lines) { $k.w.WriteLine($l) }; $k.w.Flush() }
function ReadOne($k) { try { return $k.r.ReadLine() } catch { return $null } }
function Kind([string]$l) { $t = $l.Split(' '); if ($t[0] -eq ':') { $t[1] } elseif ($t[0].StartsWith(':')) { 'user:' + $t[1] } else { '?' } }
function ReadUntil($k, [string]$stop) {
  $got = @()
  while ($true) {
    $l = ReadOne $k
    if ($null -eq $l) { return ,$got }
    $got += $l
    if ((Kind $l) -eq $stop) { return ,$got }
  }
}

# 키 — 설정 토큰 키에서 연결 확인 키를 뽑고(HMAC(토큰 키, "nox-link-auth-v1")) 그 키로 일회용 값 글자를 서명(LobbyLink.h 「키」)
$keyHex = 'ab' * 32
function HexBytes([string]$h) { $b = New-Object byte[] ($h.Length / 2); for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }; return ,$b }
function Hmac([byte[]]$key, [byte[]]$data) { $h = New-Object System.Security.Cryptography.HMACSHA256 (,$key); try { return ,$h.ComputeHash($data) } finally { $h.Dispose() } }
function ToHex([byte[]]$b) { return (($b | ForEach-Object { $_.ToString('x2') }) -join '') }
$authKey = Hmac (HexBytes $keyHex) ([Text.Encoding]::ASCII.GetBytes('nox-link-auth-v1'))

$log = Join-Path $env:TEMP ("nox_lobby_smoke_{0}.txt" -f $PID)
$ini = Join-Path $env:TEMP ("nox_lobby_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $keyHex`r`n")
$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--seconds', '14')
$null = $p.Handle   # 종료 코드를 읽으려면 미리 잡아 둔다
try {
  Write-Host "로비 :$LobbyPort · 게임 서버 연결 :$LinkPort"
  # ① 가짜 게임 서버
  $g = Open $LinkPort $utf8
  $hello = ReadOne $g
  $nonce = if ($hello -match '^HELLO ([0-9a-f]{32})$') { $Matches[1] } else { '' }
  Check ($nonce -ne '') "게임 서버 연결 — 붙자마자 HELLO 일회용 값 ($hello)"
  $sig = ToHex (Hmac $authKey ([Text.Encoding]::ASCII.GetBytes($nonce)))
  Send $g @("AUTH $sig", 'REGISTER 1760000000 127.0.0.2 18590', "ROOM 0 1 8 0 alice's%20game alice alice so_lod chat")

  $bad = Open $LinkPort $utf8
  $null = ReadOne $bad
  Send $bad @("AUTH $('0' * 64)")
  $bye = ReadOne $bad
  $after = ReadOne $bad
  Check ($bye -like 'BYE *' -and $null -eq $after) "서명 틀린 게임 서버 → BYE 한 줄 뒤 끊김 ($bye)"
  $bad.c.Close()

  # ② 클라 — 로그인 · 목록 · 채팅
  $k = Open $LobbyPort $latin1
  Send $k @('NOXCLIENT 2', 'CVERS 11020 9472', 'NICK wn', 'USER UserName HostName irc.westwood.com :RealName', 'SETCODEPAGE 949')
  $null = ReadUntil $k '329'
  Send $k @('LIST 37 37')
  $l1all = ReadUntil $k '323'   # 함수가 배열을 한 덩어리로 돌려주므로 변수에 받은 뒤 거른다(lobby_smoke.ps1 과 같은 꼴)
  $l1 = @($l1all | Where-Object { (Kind $_) -eq '326' })
  $t1 = if ($l1.Count -ge 1) { $l1[0].Split(' ') } else { @() }
  Check ($l1.Count -eq 1 -and $t1[3] -eq "#alice's_game" -and $t1[9] -eq '2130706434') "목록 = 게임 서버가 알린 방 하나 · 방장 주소 칸 = 등록 주소 127.0.0.2 ($($l1 -join ' | '))"

  $k2 = Open $LobbyPort $latin1
  Send $k2 @('NOXCLIENT 2', 'NICK bob', 'USER UserName HostName irc.westwood.com :RealName')
  $null = ReadUntil $k2 '376'
  Send $k @('JOIN #Lob_37_3 zot0blat')
  $null = ReadUntil $k '366'
  Send $k2 @('JOIN #Lob_37_3 zot0blat')
  $null = ReadUntil $k2 '366'
  $seenJoin = ReadOne $k
  Send $k2 @('PRIVMSG #Lob_37_3 :hi wn')
  $chat = ReadOne $k
  Check ($seenJoin -eq ':bob!u@h JOIN :0,0 #Lob_37_3' -and $chat -eq ':bob!u@h PRIVMSG #Lob_37_3 :hi wn') "대화방 — 들어옴 알림 · 채팅 한 줄 ($seenJoin | $chat)"

  # ③ 새 방 — 게임 서버에 묻고 답이 오면 미룬 참가 답
  Send $k @("JOINGAME #wn's_game 1 ")
  $create = ReadOne $g
  $req = if ($create -match '^CREATE (\d+) wn$') { $Matches[1] } else { '' }
  Check ($req -ne '') "새 방 → 게임 서버에 CREATE(요청 번호 · 만든 사람 wn) ($create)"
  Send $g @("ROOM 1 0 8 0 wn's%20game wn wn so_lod chat", "CREATED $req 1")
  $j = ReadUntil $k '366'
  $kinds = ($j | ForEach-Object { Kind $_ }) -join ','
  $nt = if ($j.Count -ge 1) { $j[0].Split(' ') } else { @() }
  Check ($kinds -eq '?,user:JOINGAME,332,353,366' -and $nt[0] -eq 'NOXTOKEN' -and $nt[1] -eq '127.0.0.2:18590' -and $nt[2] -match '^[0-9a-f]{102}$') "ROOM + CREATED → 클라에 참가 답(NOXTOKEN 게임 서버 등록 주소 · 원래 답 넷) ($kinds · $($nt[1]))"

  # ③-2 답 없는 새 방 — 게임 서버가 CREATE 에 답하지 않으면 로비 한도(5초 kRoomRequestTimeoutMs)에 접는다(실행 파일 루프의 Tick)
  Send $k2 @("JOINGAME #bob's_game 1 ")
  $create2 = ReadOne $g
  Check ($create2 -match '^CREATE \d+ bob$') "둘째 새 방 → CREATE(bob) — 게임 서버는 답하지 않음 ($create2)"
  Start-Sleep -Milliseconds 5600

  # ④ 판 결과 · 끊김
  Send $g @('RESULT {"match":"1760000000-1-1","roomName":"wn''s game"}')
  Start-Sleep -Milliseconds 300
  $g.c.Close()
  Start-Sleep -Milliseconds 500
  Send $k @('LIST 37 37')
  $l2 = ReadUntil $k '323'
  Check ((($l2 | ForEach-Object { Kind $_ }) -join ',') -eq '321,323') "게임 서버 끊김 → 목록 0줄 ($($l2.Count)줄)"
  Send $k @('QUIT')
  Check ((ReadUntil $k '607').Count -eq 1) 'QUIT 답 607'
  $k.c.Close(); $k2.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  if (-not $p.WaitForExit(15000)) { Check $false '로비가 --seconds 뒤에 끝나지 않음 — 강제 종료'; Stop-Process -Id $p.Id -Force }
}
$all = @(Get-Content -LiteralPath $log -Encoding UTF8)
$end = @($all | Where-Object { $_ -match '\[로비\] 끝 — ' })
Write-Host ($end -join "`n")
Check ($p.ExitCode -eq 0) "로비 종료 코드 0 ($($p.ExitCode))"
Check (@($all | Where-Object { $_ -match '\[판 결과\] \{"match":"1760000000-1-1"' }).Count -eq 1) '로비 기록 [판 결과] 한 줄(JSON 그대로)'
Check (@($all | Where-Object { $_ -match '\[게임 서버\] c\d+ 127\.0\.0\.2:18590 등록 — 실행 1760000000' }).Count -eq 1) '로비 기록 — 게임 서버 등록 한 줄'
Check (@($all | Where-Object { $_ -match '127\.0\.0\.2:18590 끊김 — 방 2개 목록에서 내림 · 기다리던 새 방 0개 실패' }).Count -eq 1) '로비 기록 — 끊김에 방 둘 내림(시간 초과로 접은 요청은 기다림에 없음)'
Check (@($all | Where-Object { $_ -match "\[로비\] \[새 방 — 시간 초과\] bob — 요청 \d+ 에 5000ms 안에 답 없음" }).Count -eq 1) '로비 기록 — 답 없는 새 방 시간 초과 한 줄'
# 클라 줄 — wn 5 + LIST + 대화방 + JOINGAME + LIST + QUIT = 10 · bob 3 + JOIN + PRIVMSG + JOINGAME(답 없음) = 6 → 16
#   보낸 줄 — wn 로그인(NOXSERVER · 375 · 372 · 376 · 329 = 5) · 목록 3 · 대화방 3 + 들어옴 1 + 채팅 1 · 참가 5 · 목록 2 · 607 → 21 ·
#     bob 로그인 4 · 대화방 3 · 시간 초과 거절 471 1(★ 10-20) · wn 이 끊길 때 대화방 PART 알림 1 → 9 · 합 30
Check ($end.Count -eq 2 -and $end[0] -match '접속 2 · 받은 줄 16 · 보낸 줄 30 · 토큰 1 · 새 방 1\(물음 2 · 실패 0 · 시간 초과 1 · 버림 0\) · 채팅 1') '끝 줄 — 클라 셈(접속 2 · 받은 줄 16 · 보낸 줄 30 · 토큰 1 · 새 방 1 · 물음 2 · 시간 초과 1)'
Check ($end.Count -eq 2 -and $end[1] -match '게임 서버 연결 2\(확인 1 · 확인 실패 1 · 등록 1 · 등록 거절 0 · 차례 틀림 0\) · 연결 줄 받음 7 · 보냄 5 · 방 알림 2 · 사라짐 0 · 못 읽는 줄 0 · 판 결과 1') '끝 줄 — 연결 셈(연결 2 · 확인 1 · 실패 1 · 받음 7 · 보냄 5 · 방 알림 2 · 판 결과 1)'
Remove-Item -LiteralPath $log, $ini -ErrorAction SilentlyContinue
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
