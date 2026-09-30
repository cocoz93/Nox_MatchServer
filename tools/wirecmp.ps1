param([string]$Tag = 'wc1', [switch]$Ref, [int]$Secs = 120, [string]$Map = 'Estate', [switch]$NoWalk, [string]$SrvExtra = '')
# -SrvExtra — 우리 서버에 덧붙일 인자(공백으로 가름 · 대조군에는 안 붙음). 예: '--spawn-at 2466 2838 --debug-mob 150 1382 2566 2838'
# ★ 09-29 — 와이어 대조 녹음(리포 지침 「같은 클라·같은 자동화로 **서버만 바꿔**」). 양쪽 다 중계기를 끼운다(`Docs/PROTOCOL.md:1438~1444`).
#     우리  : 자동 클라 → 중계기(18590 · 기록) → nox_server(18591)
#     대조군: 자동 클라 → 중계기(18590 · 기록) → opennox-server(18591)
#   기록 = `_evidence\_runs\<Tag>.cap.txt` → `build\Release\nox_opcount.exe <기록>` 으로 번호별로 센다.
#   crashrun.ps1 과 달리 **이미 떠 있는 녹스 프로세스를 죽이지 않는다** — 있으면 멈춘다(사용자가 쓰는 중일 수 있다)
#   끝낼 때는 창 닫기 먼저, 응답이 없을 때만 강제 종료. 정품 설정(nox.cfg · Version 65538)은 주행 뒤 해시로 보고 바뀌었으면 되돌린다
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bld  = Join-Path $root 'third_party\opennox-build'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
$runs = Join-Path $root '_evidence\_runs'
if (-not (Test-Path $runs)) { New-Item -ItemType Directory -Path $runs -Force | Out-Null }

$busy = Get-Process opennox,opennox-server,nox_server,nox_proxy,Game -ErrorAction SilentlyContinue
if ($busy) { "이미 떠 있다 — 멈춤: " + (($busy | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ' '); exit 1 }

$cap    = Join-Path $runs "$Tag.cap.txt"
$srvOut = Join-Path $runs "$Tag.srv.txt"
$srvErr = Join-Path $runs "$Tag.srverr.txt"
$pxOut  = Join-Path $runs "$Tag.proxy.txt"
$cliErr = Join-Path $runs "$Tag.clierr.txt"
foreach ($f in @($cap)) { if (Test-Path $f) { [IO.File]::Delete($f) } }

$mapFile = Join-Path $game "maps\$Map\$Map.map"
if ($Ref) {
  $env:NOX_XWIS='false'; $env:NOX_NET_NAT='false'; $env:NOX_SRV_NO_OBSERVER='1'
  $srv = Start-Process -FilePath (Join-Path $bld 'opennox-server.exe') `
    -ArgumentList '-data',"`"$game`"",'-serveronly','-noDraw','-port','18591','-autoexec',"`"load $($Map.ToLower())`"" `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr
  $wait = 90
  "대조군 서버(opennox-server) pid=$($srv.Id)"
} else {
  $srvArgs = @('--port','18591','--map',"`"$mapFile`"")
  if ($SrvExtra -ne '') { $srvArgs += ($SrvExtra -split '\s+' | Where-Object { $_ -ne '' }) }
  $srv = Start-Process -FilePath (Join-Path $root 'build\Release\nox_server.exe') -ArgumentList $srvArgs `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr
  $wait = 6
  "우리 서버 pid=$($srv.Id)"
}
Start-Sleep -Seconds $wait
if ($srv.HasExited) { "서버가 바로 죽었다 exit=$($srv.ExitCode)"; exit 1 }

$px = Start-Process -FilePath (Join-Path $root 'build\Release\nox_proxy.exe') -ArgumentList @('--listen','18590','--host-port','18591','--dump',"`"$cap`"",'--quiet') `
  -PassThru -WindowStyle Minimized -RedirectStandardOutput $pxOut
Start-Sleep -Seconds 1
if ($px.HasExited) { "중계기가 바로 죽었다 exit=$($px.ExitCode)"; $srv | Stop-Process -Force; exit 1 }

$env:NOX_JOIN_ADDR = '127.0.0.1:18590'
if ($NoWalk) { [Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null) } else { $env:NOX_AUTO_WALK = '1' }
$env:NOX_AUTO_WALK_TURN = '32'
$env:NOX_JOIN_CLASS = '1'
if ($Ref) {
  $env:NOX_NO_OBSERVER_RELEASE = '1'                 # 대조군에서 관전 풀기 클릭은 서버를 죽였다(crashrun.ps1 09-23)
  [Environment]::SetEnvironmentVariable('NOX_OBSERVER_CLICKS', $null)
} else {
  [Environment]::SetEnvironmentVariable('NOX_NO_OBSERVER_RELEASE', $null)
  $env:NOX_OBSERVER_CLICKS = '1'
}
$env:NOX_XWIS = 'false'; $env:NOX_NET_NAT = 'false'
[Environment]::SetEnvironmentVariable('NOX_DUMP', $null)
[Environment]::SetEnvironmentVariable('NOX_AUTO_WALK_FACE', $null)
[Environment]::SetEnvironmentVariable('NOX_AUTO_CURSOR', $null)

$cfg  = Join-Path $game 'nox.cfg'
$keep = Join-Path $game 'nox.cfg.wirecmp-keep'
Copy-Item $cfg $keep -Force
$cfgHash = (Get-FileHash $cfg).Hash

$cli = Start-Process -FilePath (Join-Path $bld 'opennox.exe') -WorkingDirectory $bld -ArgumentList '-data',"`"$game`"" -PassThru -RedirectStandardError $cliErr
"클라 pid=$($cli.Id) · $Secs 초 주행"
$result = ''
for ($i = 1; $i -le $Secs; $i++) {
  Start-Sleep -Seconds 1
  $cli.Refresh(); $srv.Refresh(); $px.Refresh()
  if ($cli.HasExited) { $result = "★ 클라가 $i 초에 끝남 (exit=$($cli.ExitCode))"; break }
  if ($srv.HasExited) { $result = "★ 서버가 $i 초에 끝남 (exit=$($srv.ExitCode))"; break }
  if ($px.HasExited)  { $result = "★ 중계기가 $i 초에 끝남 (exit=$($px.ExitCode))"; break }
}
if (-not $result) { $result = "$Secs 초 동안 셋 다 살아 있었다" }
$result

function Close-Gently($p, [int]$waitSec) {
  $p.Refresh()
  if ($p.HasExited) { return 'already' }
  if ($p.CloseMainWindow()) { for ($k = 0; $k -lt $waitSec * 4; $k++) { Start-Sleep -Milliseconds 250; $p.Refresh(); if ($p.HasExited) { return 'closed' } } }
  $p | Stop-Process -Force
  return 'forced'
}
"클라 끝냄: " + (Close-Gently $cli 5)
Start-Sleep -Seconds 1
"중계기 끝냄: " + (Close-Gently $px 2)
"서버 끝냄: " + (Close-Gently $srv 3)
if ((Get-FileHash $cfg).Hash -ne $cfgHash) { Copy-Item $keep $cfg -Force; "클라가 nox.cfg 를 덮었다 — 주행 전 것으로 되돌림" }
"nox.cfg 첫 줄: " + (Get-Content $cfg -TotalCount 1)
"기록: $cap ($((Get-Item $cap).Length) 바이트)"
