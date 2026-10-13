# router_smoke.ps1 — 라우터 소켓 왕복 (플랜 B 작업 8b, Docs/ROOMS_DESIGN.md 목표 구조 v3 ①).
#
# nox_router 를 시험용 포트로 둘 띄우고, 정품 클라가 4005 에 보내는 다섯 줄(1단계 녹음) 그대로 TCP 로 말해 본다.
#   ① 설정 없음 — 605 가 「클라가 붙은 주소」:4000 을 알려 주나 · 610 · 605 · 607 차례 · QUIT 뒤 끊기 · 답 줄 CRLF
#   ② 설정 `[router] channel = 192.0.2.10:4100` · `name = SmokeName` — 605 가 그 주소 · 포트 · 이름을 알려 주나
# 답의 꼴은 lobby_replay 가 녹음과 대조한다. 여기서는 소켓과 설정 읽기만 본다.
param([int]$Port = 14005, [int]$Port2 = 14006, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\nox_router.exe' }
$script:checks = 0; $script:fails = 0
function Check($ok, $what) {
  $script:checks++
  if ($ok) { Write-Host "  [확인] $what" } else { $script:fails++; Write-Host "  [실패] $what" }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)
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
function ReadUntil($k, [string]$stop) {
  $got = @()
  while ($true) {
    try { $l = $k.r.ReadLine() } catch { return ,$got }
    if ($null -eq $l) { return ,$got }
    $got += $l
    if ((Kind $l) -eq $stop) { return ,$got }
  }
}
$five = @('verchk 32512 65556', 'verchk 9472 65538', 'lobcount 9472', 'whereto wn 1234 9472 65538 7726', 'QUIT')

$log1 = Join-Path $env:TEMP ("router_smoke_{0}_a.txt" -f $PID)
$log2 = Join-Path $env:TEMP ("router_smoke_{0}_b.txt" -f $PID)
$ini = Join-Path $env:TEMP ("router_smoke_{0}.ini" -f $PID)
[IO.File]::WriteAllText($ini, "[router]`r`nchannel = 192.0.2.10:4100`r`nname = SmokeName`r`n")
$p1 = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log1 -ArgumentList @(
  '--bind', '127.0.0.1', '--port', $Port, '--seconds', '6', '--quiet')
$p2 = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log2 -ArgumentList @(
  '--config', "`"$ini`"", '--bind', '127.0.0.1', '--port', $Port2, '--seconds', '6', '--quiet')
try {
  Write-Host "① 설정 없음 :$Port"
  $k = Open $Port
  Send $k $five
  $a = ReadUntil $k '607'
  Check ((($a | ForEach-Object { Kind $_ }) -join ',') -eq '610,605,607') "답 순서 610 · 605 · 607 (받은 줄 $($a.Count))"
  Check (($a | Where-Object { (Kind $_) -eq '605' }) -ceq ": 605 u :127.0.0.1 4000 '0:Nox_Origin' -8 36.1083 -115.0582") '605 = 붙은 주소 127.0.0.1:4000 · 이름 Nox_Origin'
  Check ($null -eq $k.r.ReadLine()) 'QUIT 뒤 라우터가 끊는다'
  $k.c.Close()
  # 줄 끝 바이트 — ReadLine 은 LF 만 와도 줄로 받으므로 CRLF 는 날바이트로 본다
  $k = Open $Port
  Send $k @('lobcount 9472')
  Start-Sleep -Milliseconds 300
  $buf = New-Object byte[] 64
  $n = $k.c.GetStream().Read($buf, 0, $buf.Length)
  Check ($latin1.GetString($buf, 0, $n) -ceq ": 610 u 1`r`n") '답 줄이 CRLF 로 끝난다 (610 날바이트)'
  $k.c.Close()

  Write-Host "② 설정 [router] channel · name :$Port2"
  $k = Open $Port2
  Send $k $five
  $b = ReadUntil $k '607'
  Check (($b | Where-Object { (Kind $_) -eq '605' }) -ceq ": 605 u :192.0.2.10 4100 '0:SmokeName' -8 36.1083 -115.0582") '605 = 설정한 채널 192.0.2.10:4100 · 이름 SmokeName'
  $k.c.Close()
} catch {
  Check $false "예외: $($_.Exception.Message)"
} finally {
  foreach ($p in @($p1, $p2)) {
    if (-not $p.WaitForExit(15000)) { Check $false '라우터가 --seconds 뒤에 스스로 끝나지 않음 — 강제 종료'; Stop-Process -Id $p.Id -Force }
  }
}
$s1 = Get-Content -LiteralPath $log1 -Encoding UTF8 | Where-Object { $_ -match '\[라우터\] 끝' }
$s2 = Get-Content -LiteralPath $log2 -Encoding UTF8 | Where-Object { $_ -match '\[라우터\] 끝' }
Remove-Item -LiteralPath $log1, $log2, $ini -ErrorAction SilentlyContinue
Write-Host "라우터 출력: $s1 / $s2"
# 0 은 「없었다」와 「못 읽었다」를 안 가른다 — 라우터가 센 접속 · 줄 수를 같이 본다
#   ① 접속 2(왕복 · CRLF) · 받은 줄 5 + 1 · 보낸 줄 3 + 1   ② 접속 1 · 받은 줄 5 · 보낸 줄 3
Check ($s1 -match '접속 2 · 받은 줄 6 · 보낸 줄 4') '① 라우터가 센 접속 2 · 받은 줄 6 · 보낸 줄 4'
Check ($s2 -match '접속 1 · 받은 줄 5 · 보낸 줄 3') '② 라우터가 센 접속 1 · 받은 줄 5 · 보낸 줄 3'
Write-Host ("{0} — 확인 {1}건 · 실패 {2}건" -f $(if ($script:fails) { '[실패]' } else { '[통과]' }), $script:checks, $script:fails)
exit $(if ($script:fails) { 1 } else { 0 })
