param([int]$Secs = 120, [string]$Conds = '0:0,1:40,5:80,10:150', [string]$Tag = '', [int]$PilotMs = 500, [int]$Seed = 7, [switch]$Dump)
# ★ 11-03 손실 파일럿 — 녹스 릴라이어블 UDP(고정 2초 재전송 · 순서 밖 버림)가 손실 회선에서 얼마나 체감되나.
#   조건마다: 우리 서버(18591 · --pilot-reliable) ← 중간자 nox_proxy(18590 · --loss/--delay/--lat) ← 자동 클라(opennox · 가만히 섬)
#   재는 것 = 서버→클라 신뢰 순번마다 첫 송신 ↔ 클라 확인(비신뢰 머리 「다음에 받을 번호」) 시각 차(중간자 CSV) · 서버 [다시보냄] 줄
#   -Conds '손실%:지연ms,…' · 산출물 = _evidence\_runs\<Tag>\ (조건별 srv.txt · px.txt · lat.csv · summary.md)
#   ⚠ 조건마다 돌고 있는 nox_server · nox_proxy · opennox 를 전부 죽이고 시작한다(Stop-All) — 다른 판이 떠 있으면 같이 죽는다
#   읽을 때 — lat.csv 의 지연은 서버에서 잰 「처음 실음 → 클라 확인」이라 주입 지연이 **양방향** 다 들어가고(중앙값 ≈ 2×지연 + 틱 반),
#     확인 패킷이 잃어진 경우도 2초로 잡힌다(클라는 제때 받음) — 서버→클라 버림 수(px.txt)로 절반쯤이 진짜 체감이라고 읽을 것
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bld  = Join-Path $root 'third_party\opennox-build'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
$out  = Join-Path $root '_evidence\_runs'
if ($Tag -eq '') { $Tag = 'loss_' + (Get-Date -Format 'MMdd_HHmmss') }
$dir = Join-Path $out $Tag
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$mapArg = "`"$game\maps\Estate\Estate.map`""

# OpenNox 가 정품 nox.cfg 를 제 꼴로 덮어쓰므로(crashrun.ps1 과 같은 수) 사본을 떠 두고 끝에 되돌린다
$cfg  = Join-Path $game 'nox.cfg'
$keep = Join-Path $game 'nox.cfg.losspilot-keep'
Copy-Item $cfg $keep -Force
$cfgHash = (Get-FileHash $cfg).Hash

function Stop-All {
  Get-Process opennox,nox_server,nox_proxy -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 800
}
function Pct([double[]]$v, [double]$q) {
  if ($v.Count -eq 0) { return 0 }
  $i = [int][math]::Floor($q * ($v.Count - 1))
  return $v[$i]
}

$rows = @()
foreach ($c in ($Conds -split ',')) {
  $parts = $c.Trim() -split ':'
  $loss = [int]$parts[0]; $delay = [int]$parts[1]
  $name = "L$($loss)_D$($delay)"
  "===== 조건 $name — 손실 $loss% · 지연 $delay ms · $Secs 초 ====="
  Stop-All
  $srvOut = Join-Path $dir "$name.srv.txt"; $srvErr = Join-Path $dir "$name.srverr.txt"
  $pxOut  = Join-Path $dir "$name.px.txt";  $pxErr  = Join-Path $dir "$name.pxerr.txt"
  $cliErr = Join-Path $dir "$name.clierr.txt"
  $csv    = Join-Path $dir "$name.lat.csv"      # 서버가 적는 중요 메시지(cc 봉투) 전달 지연 — 본 측정
  $pcsv   = Join-Path $dir "$name.plat.csv"     # 중간자가 적는 전송 층(0x80) 전달 지연 — 접속 절차 몇 건뿐 · 참고용

  # 서버 — --quiet 를 안 준다([다시보냄] 줄은 조용하지 않을 때만 초마다 찍힌다)
  $srvArgs = @('--port','18591','--standalone','--map',$mapArg,'--mode','arena','--no-join-token',
               '--pilot-reliable',"$PilotMs",'--pilot-lat',$csv)
  $srv = Start-Process -FilePath (Join-Path $root 'build\Release\nox_server.exe') -ArgumentList $srvArgs `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr
  Start-Sleep -Seconds 6
  if ($srv.HasExited) { "서버가 바로 죽었다 exit=$($srv.ExitCode)"; exit 1 }

  # 중간자 — 손실·지연 주입 + 전달 지연 CSV. 클라를 닫은 뒤 남은 확인까지 받게 Secs+15 초 뒤 스스로 끝난다
  $pxArgs = @('--listen','18590','--host','127.0.0.1','--host-port','18591','--quiet',
              '--loss',"$loss",'--delay',"$delay",'--seed',"$Seed",'--lat',$pcsv,'--secs',"$($Secs + 15)")
  if ($Dump) { $pxArgs += @('--dump', (Join-Path $dir "$name.dump.txt")) }   # 패킷 전부 기록(계측이 안 돌 때 보는 용)
  $px = Start-Process -FilePath (Join-Path $root 'build\Release\nox_proxy.exe') -ArgumentList $pxArgs `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $pxOut -RedirectStandardError $pxErr
  Start-Sleep -Seconds 1
  if ($px.HasExited) { "중간자가 바로 죽었다 exit=$($px.ExitCode)"; Get-Content $pxOut; exit 1 }

  # 자동 클라 — 중간자(18590)로 들어가 관전을 풀고 가만히 선다(crashrun.ps1 -NoWalk 와 같은 환경)
  $env:NOX_JOIN_ADDR = '127.0.0.1:18590'
  [Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null)
  [Environment]::SetEnvironmentVariable('NOX_AUTO_CURSOR', $null)
  [Environment]::SetEnvironmentVariable('NOX_NO_OBSERVER_RELEASE', $null)
  [Environment]::SetEnvironmentVariable('NOX_DUMP', $null)
  $env:NOX_JOIN_CLASS = '1'
  $env:NOX_OBSERVER_CLICKS = '1'
  $env:NOX_XWIS = 'false'
  $env:NOX_NET_NAT = 'false'
  $cli = Start-Process -FilePath (Join-Path $bld 'opennox.exe') -WorkingDirectory $bld `
    -ArgumentList '-data',"`"$game`"" -PassThru -RedirectStandardError $cliErr
  $why = ''
  for ($i = 1; $i -le $Secs; $i++) {
    Start-Sleep -Seconds 1
    $cli.Refresh(); $srv.Refresh(); $px.Refresh()
    if ($cli.HasExited) { $why = "클라가 $i 초에 죽음 (exit=$($cli.ExitCode))"; break }
    if ($srv.HasExited) { $why = "서버가 $i 초에 죽음 (exit=$($srv.ExitCode))"; break }
    if ($px.HasExited)  { $why = "중간자가 $i 초에 죽음 (exit=$($px.ExitCode))"; break }
  }
  if ($why -eq '') { $why = "$Secs 초 동안 셋 다 살아 있었다" }
  $why

  if (-not $cli.HasExited) { $cli.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 4; $cli.Refresh() }
  if (-not $cli.HasExited) { $cli | Stop-Process -Force }
  # 중간자는 제 시간에 스스로 끝나며 요약을 찍는다 — 기다린다
  for ($i = 0; $i -lt 40; $i++) { $px.Refresh(); if ($px.HasExited) { break }; Start-Sleep -Seconds 1 }
  if (-not $px.HasExited) { $px | Stop-Process -Force; '중간자가 제때 안 끝나 강제 종료' }
  # 서버 — 이름 있는 이벤트로 곱게 끄고, 안 꺼지면 강제
  try { [System.Threading.EventWaitHandle]::OpenExisting("nox_server_stop_$($srv.Id)").Set() | Out-Null } catch {}
  for ($i = 0; $i -lt 10; $i++) { $srv.Refresh(); if ($srv.HasExited) { break }; Start-Sleep -Seconds 1 }
  if (-not $srv.HasExited) { $srv | Stop-Process -Force }

  # 읽기 — 중간자 요약 · 서버 [다시보냄] 마지막 줄 · CSV 직접 집계(중간자 요약과 교차)
  $pxSum  = @(); if (Test-Path $pxOut) { $pxSum = Get-Content $pxOut -Encoding UTF8 | Where-Object { $_ -match '주입|전달 지연' } }
  $srvRe  = ''; if (Test-Path $srvOut) { $srvRe = (Get-Content $srvOut -Encoding UTF8 | Where-Object { $_ -match '\[다시보냄\]' } | Select-Object -Last 1) }
  $lat = @(); if (Test-Path $csv) { $lat = @(Import-Csv $csv | ForEach-Object { [double]$_.latency_ms } | Sort-Object) }
  $n = $lat.Count
  $over = @($lat | Where-Object { $_ -gt 2000 }).Count
  $med = Pct $lat 0.5; $p90 = Pct $lat 0.9; $max = 0; if ($n -gt 0) { $max = $lat[$n - 1] }
  $rows += [pscustomobject]@{ 조건 = $name; 표본 = $n; 중앙값ms = [math]::Round($med, 1); 분위90ms = [math]::Round($p90, 1); 최대ms = [math]::Round($max, 1); 초과2s = $over; 비고 = $why }
  $pxSum; $srvRe
}

if ((Get-FileHash $cfg).Hash -ne $cfgHash) { Copy-Item $keep $cfg -Force; '클라가 nox.cfg 를 덮었다 — 판 전 것으로 되돌림' }

$md = @()
$md += "# 손실 파일럿 $Tag — $Secs 초 · 파일럿 신뢰 $PilotMs ms · 씨앗 $Seed"
$md += ''
$md += '| 조건 | 표본 | 중앙값 ms | 90분위 ms | 최대 ms | 2초 초과 | 비고 |'
$md += '|---|---:|---:|---:|---:|---:|---|'
foreach ($r in $rows) { $md += "| $($r.조건) | $($r.표본) | $($r.중앙값ms) | $($r.분위90ms) | $($r.최대ms) | $($r.초과2s) | $($r.비고) |" }
$md += ''
foreach ($c in ($Conds -split ',')) {
  $parts = $c.Trim() -split ':'; $name = "L$([int]$parts[0])_D$([int]$parts[1])"
  $pxOut = Join-Path $dir "$name.px.txt"; $srvOut = Join-Path $dir "$name.srv.txt"
  $md += "## $name"
  if (Test-Path $pxOut) { $md += (Get-Content $pxOut -Encoding UTF8 | Where-Object { $_ -match '주입|전달 지연' } | ForEach-Object { '- ' + $_.Trim() }) }
  if (Test-Path $srvOut) { $md += (Get-Content $srvOut -Encoding UTF8 | Where-Object { $_ -match '\[다시보냄\]' } | Select-Object -Last 1 | ForEach-Object { '- ' + $_.Trim() }) }
  $md += ''
}
[IO.File]::WriteAllText((Join-Path $dir 'summary.md'), ($md -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
''
$rows | Format-Table -AutoSize | Out-String -Width 200
"산출물: $dir"
