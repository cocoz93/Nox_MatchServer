# nox_lobby_smoke.ps1 — 로비 프로세스 nox_lobby 소켓 왕복 + 가짜 게임 서버 (MO 6단계 v2 작업 3c · Docs/LOBBY_SPLIT_DESIGN.md).
#
# nox_lobby 를 띄우고(클라 포트 · 게임 서버 연결 포트 · --seconds 로 스스로 끝남) 이 스크립트가 게임 서버 노릇을 한다.
#   ① 가짜 게임 서버 — HELLO 받음 → AUTH(연결 키 서명) → REGISTER(127.0.0.2:18590) → ROOM(alice 방) · 서명 틀린 둘째는 BYE 뒤 끊김
#   ② 클라 — 로그인 → 목록에 alice 방(방장 주소 칸 = 등록 주소 127.0.0.2) → 대화방 둘이서 채팅 한 줄
#   ③ 새 방 — 클라 JOINGAME #wn's_game → 게임 서버에 CREATE(만든 사람 wn) → ROOM + CREATED → 클라에 미룬 참가 답(NOXTOKEN 127.0.0.2:18590)
#   ③-2 답 없는 새 방 — 로비 한도 5초에 시간 초과(실행 파일 루프의 Tick) → ★ 10-20 클라에 거절 471(시간 초과)
#   ④ 판 결과 RESULT → 로비 기록 [판 결과] · ★ 7단계 조각 5 SAVED ok · dup · bad(판 번호도 없으면 답 없음) · 게임 서버 끊김 → 목록 0줄 → ★ 10-20 그때 새 방 → 거절 471(곧바로 실패 — 사람 확인 50 의 꼴) · QUIT 607
#   ⑤ ★ 7단계 살아 있음 — 말없는 게임 서버(전원째 꺼진 꼴 — 끊김 신호가 안 옴)가 살아 있다고 보는 동안 같은 주소 등록은 거절 ·
#     15초 뒤 BYE 뒤 끊김 · 그 방이 목록에서 빠짐 · 같은 주소 다시 등록이 받힘(6단계 미결) · 로비가 보낸 PING 은 읽을 때 건너뜀
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
# ★ 7단계 — 게임 서버 연결 줄 하나(로비가 5초마다 보내는 PING 은 건너뜀)
function ReadLink($k) { do { $l = ReadOne $k } while ($l -eq 'PING'); return $l }
# 로비 기록(쓰는 중인 파일)에 정규식이 `n` 번 이상 나올 때까지(초 한도) — v2_split_smoke.ps1 과 같은 꼴
function ReadLog([string]$path) {
  try {
    $fs = New-Object System.IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd(); $sr.Close(); return $t
  } catch { return '' }
}
function WaitLog([string]$path, [string]$pattern, [int]$n, [int]$sec) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt $sec) {
    if (([regex]::Matches((ReadLog $path), $pattern)).Count -ge $n) { return $true }
    Start-Sleep -Milliseconds 100
  }
  return $false
}
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
function Sig([string]$nonce) { return ToHex (Hmac $authKey ([Text.Encoding]::ASCII.GetBytes($nonce))) }
# 가짜 게임 서버 하나 붙여 HELLO 에 확인 서명 + 줄들(돌려줌 = 연결)
function Gs([string[]]$lines) {
  $x = Open $LinkPort $utf8
  $n = if ((ReadLink $x) -match '^HELLO ([0-9a-f]{32})$') { $Matches[1] } else { '' }
  Send $x (@("AUTH $(Sig $n)") + $lines)
  return $x
}

$log = Join-Path $env:TEMP ("nox_lobby_smoke_{0}.txt" -f $PID)
$ini = Join-Path $env:TEMP ("nox_lobby_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $keyHex`r`n")

# ⓪ ★ 7단계 조각 8 — 저장소를 못 고르면 로비가 안 뜬다(포트도 안 잡음 · 종료 코드 2 · 까닭 한 줄)
#   [db] 없음 + --no-db 없음 · [db] 가 닫힌 포트(127.0.0.1:1 — 붙기가 곧바로 거절됨)
function Refused([string]$iniText, [string]$pattern, [string]$what) {
  $i0 = Join-Path $env:TEMP ("nox_lobby_smoke_{0}_r.ini" -f $PID); $l0 = Join-Path $env:TEMP ("nox_lobby_smoke_{0}_r.txt" -f $PID)
  [IO.File]::WriteAllText($i0, $iniText)
  $r = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $l0 -ArgumentList @(
    '--config', "`"$i0`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--seconds', '5')
  $null = $r.Handle
  $done = $r.WaitForExit(10000)
  if (-not $done) { Stop-Process -Id $r.Id -Force }
  $t = if (Test-Path -LiteralPath $l0) { [IO.File]::ReadAllText($l0, [Text.Encoding]::UTF8) } else { '' }
  Check ($done -and $r.ExitCode -eq 2 -and $t -match $pattern -and $t -notmatch '듣기 —') "⓪ $what → 안 뜸(종료 코드 $(if ($done) { $r.ExitCode } else { '끝나지 않음' }) · 포트 안 잡음)"
  Remove-Item -LiteralPath $i0, $l0 -ErrorAction SilentlyContinue
}
Refused "[token]`r`nkey = $keyHex`r`n" '\[멈춤\] 설정 파일에 \[db\] user 가 없다' '[db] 없음 · --no-db 없음'
Refused "[token]`r`nkey = $keyHex`r`n[db]`r`nhost = 127.0.0.1`r`nport = 1`r`nuser = nobody`r`npassword = x`r`n" '\[멈춤\] DB 127\.0\.0\.1:1/nox_mo — 붙기' '[db] 가 닫힌 포트'

$p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList @(
  '--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort, '--seconds', '90', '--no-db')   # ★ 7단계 — 끝은 닫기 이벤트(⑤ 가 15초를 기다림) · --seconds 는 안전 한도 · 메모리 저장소
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
  Send $k @('NOXCLIENT 2', 'CVERS 11020 9472', 'NICK wn', 'apgar s3cr3t9 0', 'USER UserName HostName irc.westwood.com :RealName', 'SETCODEPAGE 949')   # ★ 로그인 암호 조각 4 — 평문 apgar(아이디만 판은 안 봄 · 기록에선 가림)
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
  $create = ReadLink $g
  $req = if ($create -match '^CREATE (\d+) wn$') { $Matches[1] } else { '' }
  Check ($req -ne '') "새 방 → 게임 서버에 CREATE(요청 번호 · 만든 사람 wn) ($create)"
  Send $g @("ROOM 1 0 8 0 wn's%20game wn wn so_lod chat", "CREATED $req 1")
  $j = ReadUntil $k '366'
  $kinds = ($j | ForEach-Object { Kind $_ }) -join ','
  $nt = if ($j.Count -ge 1) { $j[0].Split(' ') } else { @() }
  Check ($kinds -eq '?,user:JOINGAME,332,353,366' -and $nt[0] -eq 'NOXTOKEN' -and $nt[1] -eq '127.0.0.2:18590' -and $nt[2] -match '^[0-9a-f]{102}$') "ROOM + CREATED → 클라에 참가 답(NOXTOKEN 게임 서버 등록 주소 · 원래 답 넷) ($kinds · $($nt[1]))"

  # ③-2 답 없는 새 방 — 게임 서버가 CREATE 에 답하지 않으면 로비 한도(5초 kRoomRequestTimeoutMs)에 접는다(실행 파일 루프의 Tick)
  Send $k2 @("JOINGAME #bob's_game 1 ")
  $create2 = ReadLink $g
  Check ($create2 -match '^CREATE \d+ bob$') "둘째 새 방 → CREATE(bob) — 게임 서버는 답하지 않음 ($create2)"
  Start-Sleep -Milliseconds 5600
  # ★ 10-20 거절 답 — 한도에 접히면 기다리던 bob 에게 471 한 줄(그 전엔 답이 없어 정품이 「서버 접속 시도중...」에서 멈췄다 — 사람 확인 50)
  $rj1 = ReadOne $k2
  Check ($rj1 -eq ": 471 u #bob's_game :Cannot create game (game server timed out)") "시간 초과 → bob 에 거절 471 한 줄(참가 채널 · 시간 초과) ($rj1)"

  # ④ 판 결과 · 끊김 — ★ 7단계 조각 5: 온전한 판 → SAVED ok · 같은 판 다시 → dup · 못 읽는 판(판 번호는 있음) → bad · 판 번호도 없는 줄 → 답 없음
  #   ok · dup 은 저장소 답(다음 바퀴) · bad 는 받은 바퀴에 곧바로라 차례는 안 본다
  $res = '{"match":"1760000000-1-1","room":1,"roomName":"wn''s game","map":"so_lod","mode":"chat","start":1760000010,"end":1760000027,"frames":510,"sec":17,' +
         '"endReason":"score_player","winner":{"kind":"player","id":2,"account":"bob"},"limit":1,"timeLimitSec":0,' +
         '"players":[{"account":"wn","netCode":1,"state":"finished","team":0,"score":0,"deaths":1}]}'
  Send $g @("RESULT $res", "RESULT $res", 'RESULT {"match":"1760000000-1-2"}', 'RESULT {nope')
  $saved = @((ReadLink $g), (ReadLink $g), (ReadLink $g)) | Sort-Object
  Check (($saved -join '|') -eq 'SAVED 1760000000-1-1 dup|SAVED 1760000000-1-1 ok|SAVED 1760000000-1-2 bad') "④ 판 결과 넷 → SAVED 셋(ok · 같은 판 dup · 못 읽음 bad · 판 번호도 없으면 답 없음) ($($saved -join ' | '))"
  Start-Sleep -Milliseconds 300
  $g.c.Close()
  Start-Sleep -Milliseconds 500
  Send $k @('LIST 37 37')
  $l2 = ReadUntil $k '323'
  Check ((($l2 | ForEach-Object { Kind $_ }) -join ',') -eq '321,323') "게임 서버 끊김 → 목록 0줄 ($($l2.Count)줄)"
  # ★ 10-20 거절 답 — 게임 서버가 하나도 없을 때 새 방(사람 확인 50 과 같은 꼴) → 묻지 않고 곧바로 거절 471
  Send $k @("JOINGAME #wn's_game 1 ")
  $rj2 = ReadOne $k
  Check ($rj2 -eq ": 471 u #wn's_game :Cannot create game (request failed)") "게임 서버 없음 → 새 방에 곧바로 거절 471 한 줄 ($rj2)"

  # ⑤ ★ 7단계 — 살아 있음(말없는 게임 서버 g2 · 같은 주소 g3 거절 · 한도 뒤 g4 받힘)
  $g2 = Gs @('REGISTER 1760000200 127.0.0.3 18590', "ROOM 0 1 8 0 carl's%20game carl carl so_lod chat")
  Start-Sleep -Milliseconds 300
  Send $k @('LIST 37 37')
  $l3all = ReadUntil $k '323'
  $l3 = @($l3all | Where-Object { (Kind $_) -eq '326' })
  Check ($l3.Count -eq 1 -and $l3[0].Split(' ')[3] -eq "#carl's_game") "⑤ 이제 말없을 게임 서버 g2(127.0.0.3) 등록 · 목록에 그 방 ($($l3.Count)줄)"
  $g3 = Gs @('REGISTER 1760000300 127.0.0.3 18590')
  $bye3 = ReadLink $g3
  Check ($bye3 -eq 'BYE same%20address%20registered' -and $null -eq (ReadLink $g3)) "⑤ g2 가 살아 있다고 보는 동안 같은 주소 등록 → BYE 뒤 끊김 ($bye3)"
  $g3.c.Close()
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $deadSeen = WaitLog $log '127\.0\.0\.3:18590 15초 동안 받은 줄 없음 — 끊음' 1 20
  $waited = [int]$sw.Elapsed.TotalSeconds
  $bye2 = ReadLink $g2
  Check ($deadSeen -and $bye2 -eq 'BYE no%20line%20for%2015s' -and $null -eq (ReadLink $g2)) "⑤ 말없는 g2 → 한도에 BYE 뒤 끊김(그 사이 PING 은 건너뜀 · ${waited}초 기다림) ($bye2)"
  $g2.c.Close()
  Send $k @('LIST 37 37')
  $l4 = ReadUntil $k '323'
  Check ((($l4 | ForEach-Object { Kind $_ }) -join ',') -eq '321,323') "⑤ 끊김에 g2 방이 목록에서 빠짐 ($($l4.Count)줄)"
  $g4 = Gs @('REGISTER 1760000400 127.0.0.3 18590')
  Check (WaitLog $log '127\.0\.0\.3:18590 등록 — 실행 1760000400' 1 5) '⑤ 끊긴 뒤 같은 주소 다시 등록 → 받음(6단계 미결이 풀림)'
  $g4.c.Close()
  Start-Sleep -Milliseconds 300

  Send $k @('QUIT')
  Check ((ReadUntil $k '607').Count -eq 1) 'QUIT 답 607'
  $k.c.Close(); $k2.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  try { [void][Threading.EventWaitHandle]::OpenExisting("nox_lobby_stop_$($p.Id)").Set() } catch { }   # ★ 7단계 — 닫기 이벤트(끝 줄을 찍고 나감)
  if (-not $p.WaitForExit(15000)) { Check $false '로비가 닫기 이벤트 뒤에 끝나지 않음 — 강제 종료'; Stop-Process -Id $p.Id -Force }
}
$all = @(Get-Content -LiteralPath $log -Encoding UTF8)
# ★ 로그인 암호 조각 4 — 받은 줄 기록에서 apgar 인자(암호)를 가림 — 친 암호가 기록 어디에도 없음
Check (@($all | Where-Object { $_ -match '>> apgar \*\*\* 0' }).Count -eq 1 -and @($all | Where-Object { $_ -match 's3cr3t9' }).Count -eq 0) '기록 — apgar 암호 가림(*** 한 줄 · 친 암호 0줄)'
$end = @($all | Where-Object { $_ -match '\[로비\] 끝 — ' })
Write-Host ($end -join "`n")
# ★ 7단계 조각 6 — 끝 줄 넷(접속 · 로그인 · 게임 서버 연결 · 판 저장)을 머리글로 고른다(차례로 고르면 줄이 늘 때마다 밀린다)
$endOf = @{}
foreach ($head in '접속', '로그인', '게임 서버 연결', '판 저장 요청') {   # ★ 로그인 암호 조각 4 — 「로그인 확인」 줄이 「로그인」 줄로
  $endOf[$head] = @($end | Where-Object { $_ -match ('\[로비\] 끝 — ' + $head + ' ') }) -join "`n"
}
# ★ 로그인 암호 조각 4 — 로그인 wn · bob 둘 다 아이디만(이 시험 설정엔 [auth] 가 없다) · 토큰 · 실패 표시 갈래 0 · 끊음 · 보기만 0
$okLogin = $false
if ($endOf['로그인'] -match '로그인 2\(토큰 \[[^\]1-9]*\] · 다시 쓴 토큰 0 · 실패 표시 \[[^\]1-9]*\] · 모르는 까닭 0 · 손님 0 · 평문 0 · apgar 없음 0 · 이름 틀림 0 · 아이디만 2\) · 옛 접속 끊음 0 · 로그인된 계정 0 · 대화방 보기만 0 · 마감 끊음 0 · 실패 끊음 0') {
  $okLogin = $true
}
Check $okLogin '끝 줄 — 로그인 2(아이디만 2 · 토큰 · 실패 표시 갈래 0 · 손님 · 평문 · 없음 · 이름 틀림 0 · 옛 접속 끊음 · 보기만 · 마감 끊음 0)'
Check ($p.ExitCode -eq 0) "로비 종료 코드 0 ($($p.ExitCode))"
Check (@($all | Where-Object { $_ -match '\[판 결과\] \{"match":"1760000000-1-1"' }).Count -eq 2) '로비 기록 [판 결과] 같은 판 두 줄(JSON 그대로 · ★ 7단계 — 다시 보낸 것도 찍고 저장소가 dup 으로 가름)'
Check (@($all | Where-Object { $_ -match '127\.0\.0\.2:18590 판 결과를 못 읽음\(빠진 칸 · 칸 room\) — SAVED bad' }).Count -eq 1 -and
       @($all | Where-Object { $_ -match '127\.0\.0\.2:18590 판 결과를 못 읽음\(문법\) — 판 번호도 못 읽어 답 없음' }).Count -eq 1) '로비 기록 — ★ 7단계 못 읽은 판 결과 둘(빠진 칸 room → bad · 문법 → 답 없음)'
Check (@($all | Where-Object { $_ -match '\[게임 서버\] c\d+ 127\.0\.0\.2:18590 등록 — 실행 1760000000' }).Count -eq 1) '로비 기록 — 게임 서버 등록 한 줄'
Check (@($all | Where-Object { $_ -match '127\.0\.0\.2:18590 끊김 — 방 2개 목록에서 내림 · 기다리던 새 방 0개 실패' }).Count -eq 1) '로비 기록 — 끊김에 방 둘 내림(시간 초과로 접은 요청은 기다림에 없음)'
Check (@($all | Where-Object { $_ -match "\[로비\] \[새 방 — 시간 초과\] bob — 요청 \d+ 에 5000ms 안에 답 없음" }).Count -eq 1) '로비 기록 — 답 없는 새 방 시간 초과 한 줄'
Check (@($all | Where-Object { $_ -match '\[로비\] \[새 방 — 실패\] wn — 등록된 게임 서버 없음 \(거절 471\)' }).Count -eq 1) '로비 기록 — 게임 서버 없음 새 방 실패 한 줄(거절 471)'
# ★ 7단계 ⑤ 기록 — 무소식 끊음 한 줄 · 그 게임 서버 끊김(등록했던 것 — 주소 · 방 1개) · 같은 주소 등록 거절 한 줄
Check (@($all | Where-Object { $_ -match '127\.0\.0\.3:18590 15초 동안 받은 줄 없음 — 끊음\(방 1개를 끊김에 내림\)' }).Count -eq 1) '로비 기록 — ⑤ 무소식 끊음 한 줄(g2 · 방 1개)'
Check (@($all | Where-Object { $_ -match '127\.0\.0\.3:18590 끊김 — 방 1개 목록에서 내림' }).Count -eq 1) '로비 기록 — ⑤ 무소식으로 끊은 g2 의 끊김도 등록했던 게임 서버로(주소 · 방 1개 — 「등록 전」 아님)'
Check (@($all | Where-Object { $_ -match '등록 거절 — 127\.0\.0\.3:18590' }).Count -eq 1) '로비 기록 — ⑤ 같은 주소 등록 거절 한 줄(g3)'
# 클라 줄 — wn 5 + LIST + 대화방 + JOINGAME + LIST + JOINGAME(게임 서버 없음) + ★ ⑤ LIST 2 + QUIT = 13 · bob 3 + JOIN + PRIVMSG + JOINGAME(답 없음) = 6 → 19
#   보낸 줄 — wn 로그인(NOXSERVER · 375 · 372 · 376 · 329 = 5) · 목록 3 · 대화방 3 + 들어옴 1 + 채팅 1 · 참가 5 · 목록 2 · 거절 471 1(★ 10-20) ·
#     ★ ⑤ 목록 3(g2 방) + 목록 2 · 607 → 27 · bob 로그인 4 · 대화방 3 · 시간 초과 거절 471 1(★ 10-20) · wn 이 끊길 때 대화방 PART 알림 1 → 9 · 합 36
#   새 방 — 물음 2(만듦 1 · 시간 초과 1) + 곧바로 실패 1(게임 서버 없음 — 묻지 않음) · 참가 거절 2 = 곧바로 실패 1 + 시간 초과 1
Check ($end.Count -eq 4 -and $endOf['접속'] -match '접속 2 · 받은 줄 20 · 보낸 줄 36 · 토큰 1 · 새 방 1\(물음 2 · 실패 1 · 시간 초과 1 · 버림 0\) · 채팅 1 · 참가 거절 2\(곧바로 실패 1 · 게임 서버 실패 0 · 시간 초과 1 · 없는 방 0 · 로그인 안 됨 0\)') '끝 줄 — 클라 셈(접속 2 · 받은 줄 20(★ 조각 4 apgar 한 줄 더함) · 보낸 줄 36 · 토큰 1 · 새 방 1 · 물음 2 · 실패 1 · 시간 초과 1 · 참가 거절 2)'
# 연결 — 게임 서버 g · 서명 틀린 것 · ★ ⑤ g2 · g3 · g4 = 5 · 받음 = g 9(★ 7단계 조각 5 — RESULT 넷) + 틀린 것 1 + g2 3 + g3 2 + g4 2 = 17(가짜는 PING 을 안 보냄) ·
#   보냄(PING 뺌) = HELLO 5 + BYE 3(서명 틀림 · g3 같은 주소 · g2 무소식) + CREATE 2 + SAVED 3(ok · dup · bad) = 13 · PING = g(③-2 에서 5.6초를 기다림) 1 이상 + g2(15초) 2 → 3 이상
$linkOk = $false
if ($end.Count -eq 4 -and $endOf['게임 서버 연결'] -match '게임 서버 연결 5\(확인 4 · 확인 실패 1 · 등록 3 · 등록 거절 1 · 차례 틀림 0\) · 연결 줄 받음 17\(PING 0\) · 보냄 (\d+)\(PING (\d+)\) · 방 알림 3 · 사라짐 0 · 못 읽는 줄 0 · 판 결과 4 · 무소식 끊음 1') {
  $linkOk = ([int]$Matches[1] - [int]$Matches[2]) -eq 13 -and [int]$Matches[2] -ge 3
}
Check $linkOk '끝 줄 — 연결 셈(연결 5 · 확인 4 · 실패 1 · 등록 3 · 거절 1 · 받음 17 · 보냄 PING 빼고 13 · PING 3 이상 · 방 알림 3 · 판 결과 4 · 무소식 끊음 1)'
# ★ 7단계 조각 5 — 판 결과 넷 = 저장 요청 2(ok 1 · dup 1) + 못 읽음 2(빠진 칸 1 → bad · 문법 1 → 판 번호도 없음)
Check ($end.Count -eq 4 -and $endOf['판 저장 요청'] -match '판 저장 요청 2\(SAVED ok 1 · dup 1 · 실패 0 · 끊긴 뒤 답 0 · 모르는 답 0 · 끝날 때 남음 0\) · 못 읽음 2\(문법 1 · 빠진 칸 1 · 타입 0 · 값 0 · SAVED bad 1 · 판 번호도 못 읽음 1\) · 저장소 없음 0') '끝 줄 — 판 저장(요청 2 · ok 1 · dup 1 · 못 읽음 2 — 문법 1 · 빠진 칸 1 · bad 1 · 판 번호 없음 1)'
Remove-Item -LiteralPath $log, $ini -ErrorAction SilentlyContinue
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
