# lobby_load.ps1 — 로비 부하 판 한 번 (★ 11-17 D-2 · `Docs/SERVERCORE_LOBBY_MOVE_PLAN_1116.md` 6절).
#
# 라우터 · 로비 · 채팅 서버 · 게임 서버(로비에 붙음)를 시험 포트로 띄우고 더미 `nox_lobby_load` 를 돌린 뒤, 넷을 닫기 이벤트로 끝내고
#   더미가 센 줄 수와 서버가 센 줄 수를 맞대 본다(계획 6-1 「줄 손실 없음」 — 양쪽 셈을 같은 단위로).
#   ① 더미 로비 보낸 줄 = 로비 받은 줄 · 로비 보낸 줄 = 더미 로비 받은 줄 · 더미 로비 붙음 = 로비 접속(라우터 · 채팅도 같은 꼴)
#   ② 로비 `[로비 구간]` 줄을 다 더한 받은 줄 · 보낸 줄 = 로비 끝 줄(초마다 셈이 빠지거나 겹치지 않았나)
#   ③ 더미 실패 0 · 답 없음 0 · 뜻밖 끊김 0 · 서버 종료 코드 0 · 강제 종료 0
# 첫 판은 `--no-auth --no-db`(계획 6-3 — 암호 확인 · DB 비용을 빼고 소켓 층만). 결과는 `_out/lobby_load/<Tag>/`(지우지 않는다)
# ⚠ 이 대본은 도구다 — 파일럿 · 측정의 M · R · 길이는 계획 6-4 차례대로 사람이 정한 뒤 돌린다. 기본값(10명 · 30초)은 「도구가 맞물리나」 확인용
param(
  [int]$People = 10, [double]$Active = 0.1, [double]$RateScale = 1.0, [int]$Seconds = 30, [int]$Ramp = 50, [int]$HoldSec = 30,
  [int]$ChatLobbies = 1, [int]$Seed = 1, [int]$Workers = 1, [string]$Tag = '',
  [int]$RouterPort = 14205, [int]$LobbyPort = 14200, [int]$LinkPort = 14210, [int]$ChatPort = 14220, [int]$UdpPort = 18791,
  [switch]$NoGame, [switch]$NoRouter, [string]$Bin = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Bin) { $Bin = Join-Path $root 'build\Release' }
if (-not $Tag) { $Tag = Get-Date -Format 'MMdd_HHmmss' }
$out = Join-Path $root "_out\lobby_load\$Tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$script:checks = 0; $script:fails = 0
$report = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { Write-Host $s; $report.Add($s) }
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Say "  [확인] $what" } else { $script:fails++; Say "  [실패] $what" }
}
function ReadShared([string]$path) {   # 다른 프로세스가 쓰는 중인 기록(읽기 공유로 연다)
  try {
    $fs = New-Object System.IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $t = $sr.ReadToEnd(); $sr.Close(); return $t
  } catch { return '' }
}
function Start-Hidden([string]$exe, [string]$log, [string[]]$argv) {
  $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -ArgumentList $argv
  $null = $p.Handle   # 종료 코드를 읽으려면 미리 잡아 둔다
  return $p
}
# 닫기 이벤트 → 15초 기다림 → 그래도 안 끝나면 그때만 강제 종료(CLAUDE.md 「강제 종료는 응답이 없을 때만」)
$script:forced = 0
function Stop-Gently($p, [string]$eventName, [string]$what) {
  if ($null -eq $p -or $p.HasExited) { return }
  try { [void][Threading.EventWaitHandle]::OpenExisting($eventName).Set() } catch { }
  if (-not $p.WaitForExit(15000)) { $script:forced++; Say "  [실패] $what 가 닫기 이벤트 뒤 15초에 안 끝남 — 강제 종료"; Stop-Process -Id $p.Id -Force }
}
function Num([string]$line, [string]$label) {   # 줄에서 「<label> <수>」의 수(없으면 -1)
  $m = [regex]::Match($line, [regex]::Escape($label) + ' (\d+)')
  if ($m.Success) { return [int64]$m.Groups[1].Value } else { return -1 }
}

$ini = Join-Path $out 'load.ini'
[IO.File]::WriteAllText($ini, "[token]`r`nkey = $('ab' * 32)`r`n[chat]`r`nhost = 127.0.0.1`r`nport = $ChatPort`r`n")
$rini = Join-Path $out 'router.ini'
[IO.File]::WriteAllText($rini, "[router]`r`nchannel = 127.0.0.1:$LobbyPort`r`n")
$logs = @{ lobby = (Join-Path $out 'lobby.log'); chat = (Join-Path $out 'chat.log'); router = (Join-Path $out 'router.log'); game = (Join-Path $out 'server.log'); dummy = (Join-Path $out 'dummy.log') }

Say "로비 부하 판 $Tag — 사람 $People · 활동 $Active · 배율 $RateScale · 잼 $Seconds 초 · 올림 초당 $Ramp · 방 머묾 $HoldSec 초 · 대화방 $ChatLobbies · 게임 서버 $(if ($NoGame) { '없음' } else { '있음' }) · 라우터 $(if ($NoRouter) { '안 씀' } else { '씀' })"
$lp = $cp = $rp = $gp = $dp = $null
try {
  $lp = Start-Hidden (Join-Path $Bin 'nox_lobby.exe') $logs.lobby @('--config', "`"$ini`"", '--bind', '127.0.0.1', '--lobby-port', $LobbyPort, '--link-port', $LinkPort,
                                                                     '--quiet', '--no-db', '--no-auth', '--sections')
  $cp = Start-Hidden (Join-Path $Bin 'nox_chat.exe') $logs.chat @('--config', "`"$ini`"", '--bind', '127.0.0.1', '--port', $ChatPort, '--quiet', '--no-db', '--no-auth')
  if (-not $NoRouter) { $rp = Start-Hidden (Join-Path $Bin 'nox_router.exe') $logs.router @('--config', "`"$rini`"", '--bind', '127.0.0.1', '--port', $RouterPort, '--quiet') }
  if (-not $NoGame) {
    $gp = Start-Hidden (Join-Path $Bin 'nox_server.exe') $logs.game @('--port', $UdpPort, '--config', "`"$ini`"", '--lobby', "127.0.0.1:$LinkPort", '--quiet', '--no-results')
    $registered = $false
    for ($i = 0; $i -lt 100 -and -not $registered; $i++) {
      Start-Sleep -Milliseconds 100
      $registered = (ReadShared $logs.lobby) -match "\[게임 서버\] c\d+ \(로비 쪽 주소\):$UdpPort 등록 — 실행 \d+"
    }
    Check $registered '게임 서버가 로비에 붙어 등록(10초 안)'
  } else {
    Start-Sleep -Milliseconds 500
  }
  $dargs = @('--lobby', "127.0.0.1:$LobbyPort", '--people', $People, '--active', $Active, '--rate-scale', $RateScale, '--ramp', $Ramp,
             '--seconds', $Seconds, '--hold-sec', $HoldSec, '--chat-lobbies', $ChatLobbies, '--seed', $Seed, '--workers', $Workers)
  if (-not $NoRouter) { $dargs += @('--router', "127.0.0.1:$RouterPort") }
  $dp = Start-Hidden (Join-Path $Bin 'nox_lobby_load.exe') $logs.dummy $dargs
  $limitMs = (([math]::Ceiling($People / [double]$Ramp)) + $Seconds + 120) * 1000
  if (-not $dp.WaitForExit($limitMs)) { $script:forced++; Say "  [실패] 더미가 $([int]($limitMs / 1000))초 안에 안 끝남 — 강제 종료"; Stop-Process -Id $dp.Id -Force }
  # 서버 CPU(프로세스 전체 · 시작부터) — 닫기 전에 읽는다
  foreach ($x in @(@('로비', $lp), @('채팅', $cp), @('라우터', $rp), @('게임 서버', $gp))) {
    if ($null -ne $x[1] -and -not $x[1].HasExited) { Say ("  CPU {0} {1:N2}초 · 작업 집합 {2:N1}MB" -f $x[0], (Get-Process -Id $x[1].Id).TotalProcessorTime.TotalSeconds, ((Get-Process -Id $x[1].Id).WorkingSet64 / 1MB)) }
  }
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  Stop-Gently $gp "nox_server_stop_$($gp.Id)" '게임 서버'
  Stop-Gently $rp "nox_router_stop_$($rp.Id)" '라우터'
  Stop-Gently $cp "nox_chat_stop_$($cp.Id)" '채팅 서버'
  Stop-Gently $lp "nox_lobby_stop_$($lp.Id)" '로비'
}

$lobbyLines = @((ReadShared $logs.lobby) -split "`r?`n")
$dummyLines = @((ReadShared $logs.dummy) -split "`r?`n")
$chatLines = @((ReadShared $logs.chat) -split "`r?`n")
$routerLines = @((ReadShared $logs.router) -split "`r?`n")
foreach ($line in @($dummyLines | Where-Object { $_ -match '^\[더미 끝\]' })) { Say $line }
$lEnd = @($lobbyLines | Where-Object { $_ -match '\[로비\] 끝 — 접속' })
$cEnd = @($chatLines | Where-Object { $_ -match '\[채팅\] 끝 — 접속' })
$rEnd = @($routerLines | Where-Object { $_ -match '\[라우터\] 끝 — 접속' })
foreach ($line in @($lEnd + $cEnd + $rEnd)) { Say $line }
$dLobby = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] 로비 — ' })
$dChat = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] 채팅 — ' })
$dRouter = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] 라우터 — ' })
$dHead = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] 사람 ' })
$dTimeout = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] 답 없음 ' })
Check ($null -ne $dp -and $dp.HasExited -and $dp.ExitCode -eq 0) "더미 종료 코드 $(if ($dp) { $dp.ExitCode })"
Check ($lp.ExitCode -eq 0 -and $cp.ExitCode -eq 0 -and ($NoRouter -or $rp.ExitCode -eq 0) -and ($NoGame -or $gp.ExitCode -eq 0)) "서버 종료 코드 — 로비 $($lp.ExitCode) · 채팅 $($cp.ExitCode) · 라우터 $(if ($rp) { $rp.ExitCode } else { '-' }) · 게임 서버 $(if ($gp) { $gp.ExitCode } else { '-' })"
Check ($script:forced -eq 0) "강제 종료 $($script:forced)"
Check ($lEnd.Count -eq 1 -and $dLobby.Count -eq 1) "끝 줄 — 로비 $($lEnd.Count) · 더미 로비 $($dLobby.Count)"
if ($lEnd.Count -eq 1 -and $dLobby.Count -eq 1) {
  $lIn = Num $lEnd[0] '받은 줄'; $lOut = Num $lEnd[0] '보낸 줄'; $lConn = Num $lEnd[0] '접속'
  $dSent = Num $dLobby[0] '보낸 줄'; $dRecv = Num $dLobby[0] '받은 줄'; $dOpen = Num $dLobby[0] '붙음'
  Check ($dSent -eq $lIn -and $dSent -gt 0) "① 로비 — 더미 보낸 줄 $dSent = 로비 받은 줄 $lIn"
  Check ($dRecv -eq $lOut -and $dRecv -gt 0) "① 로비 — 로비 보낸 줄 $lOut = 더미 받은 줄 $dRecv"
  Check ($dOpen -eq $lConn -and $dOpen -gt 0) "① 로비 — 더미 붙음 $dOpen = 로비 접속 $lConn"
  $sect = @($lobbyLines | Where-Object { $_ -match '\[로비 구간\] 초 ' })
  $sumIn = 0; $sumOut = 0
  foreach ($s in $sect) { $sumIn += (Num $s '받은 줄'); $sumOut += (Num $s '보낸 줄') }
  Check ($sect.Count -gt 0 -and $sumIn -eq $lIn -and $sumOut -eq $lOut) "② [로비 구간] $($sect.Count)줄 합 — 받은 줄 $sumIn · 보낸 줄 $sumOut(끝 줄 $lIn · $lOut)"
}
if ($cEnd.Count -eq 1 -and $dChat.Count -eq 1) {
  Check ((Num $dChat[0] '보낸 줄') -eq (Num $cEnd[0] '받은 줄') -and (Num $cEnd[0] '보낸 줄') -eq (Num $dChat[0] '받은 줄') -and (Num $dChat[0] '붙음') -eq (Num $cEnd[0] '접속')) "① 채팅 — 더미 보냄 $(Num $dChat[0] '보낸 줄') = 받음 $(Num $cEnd[0] '받은 줄') · 보냄 $(Num $cEnd[0] '보낸 줄') = 더미 받음 $(Num $dChat[0] '받은 줄') · 붙음 $(Num $dChat[0] '붙음') = 접속 $(Num $cEnd[0] '접속')"
} else { Check $false "끝 줄 — 채팅 $($cEnd.Count) · 더미 채팅 $($dChat.Count)" }
if (-not $NoRouter) {
  if ($rEnd.Count -eq 1 -and $dRouter.Count -eq 1) {
    Check ((Num $dRouter[0] '보낸 줄') -eq (Num $rEnd[0] '받은 줄') -and (Num $rEnd[0] '보낸 줄') -eq (Num $dRouter[0] '받은 줄') -and (Num $dRouter[0] '붙음') -eq (Num $rEnd[0] '접속')) "① 라우터 — 더미 보냄 $(Num $dRouter[0] '보낸 줄') = 받음 $(Num $rEnd[0] '받은 줄') · 보냄 $(Num $rEnd[0] '보낸 줄') = 더미 받음 $(Num $dRouter[0] '받은 줄') · 붙음 $(Num $dRouter[0] '붙음') = 접속 $(Num $rEnd[0] '접속')"
  } else { Check $false "끝 줄 — 라우터 $($rEnd.Count) · 더미 라우터 $($dRouter.Count)" }
}
if ($dHead.Count -eq 1) {
  Check ($dHead[0] -match '실패 0\(없음\)' -and (Num $dHead[0] '끝') -eq $People) "③ 더미 — 끝 $(Num $dHead[0] '끝')/$People · 실패 0"
} else { Check $false "끝 줄 — 더미 머리 $($dHead.Count)" }
Check ($dTimeout.Count -eq 1 -and (Num $dTimeout[0] '답 없음') -eq 0) "③ 답 없음 $(if ($dTimeout.Count -eq 1) { Num $dTimeout[0] '답 없음' } else { '?' })"
$unexp = @($dummyLines | Where-Object { $_ -match '^\[더미 끝\] (로비|채팅|라우터) — ' -and $_ -notmatch '뜻밖 끊김 0\(없음\)' })
Check ($unexp.Count -eq 0) "③ 뜻밖 끊김 없음($($unexp.Count)몫)"

Say "확인 $($script:checks - $script:fails)/$($script:checks) · 기록 $out"
[IO.File]::WriteAllLines((Join-Path $out 'summary.txt'), $report, (New-Object Text.UTF8Encoding($true)))
if ($script:fails -gt 0) { exit 1 } else { exit 0 }
