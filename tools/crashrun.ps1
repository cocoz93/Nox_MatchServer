param([int]$Secs = 120, [string]$Tag = 'run1', [switch]$Judge, [switch]$Ref, [string]$Cursor = '', [switch]$NoWalk,
      [string]$SrvExtra = '')
# -SrvExtra — 우리 서버에 덧붙일 인자(공백으로 가름). 예: -SrvExtra '--spawn-at 3000 2500'
#   (09-23 nine-defects 1-d — 불 자리에 태어나게 해 닿음 피해를 실물로 보려고 넣음)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bld  = Join-Path $root 'third_party\opennox-build'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
# 판 산출물은 **리포 안**에 남긴다(`_evidence/` 는 gitignore 라 git 에는 안 들어간다).
#   전에는 세션 임시 폴더에 쌓아 21GB 가 되었고, 그 경로가 스크립트에 박혀 다음 세션엔 안 돌았다
$scratch = Join-Path $root '_evidence\_runs'
if (-not (Test-Path $scratch)) { New-Item -ItemType Directory -Path $scratch -Force | Out-Null }

Get-Process opennox,opennox-server,nox_server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$srvOut = Join-Path $scratch "$Tag.srv.txt"
$srvErr = Join-Path $scratch "$Tag.srverr.txt"
$cliErr = Join-Path $scratch "$Tag.clierr.txt"
$cliLog = Join-Path $bld 'logs\opennox.log'
if (Test-Path $cliLog) { [IO.File]::Delete($cliLog) }

$mapArg = "`"$game\maps\Estate\Estate.map`""
if ($Ref) {
  $env:NOX_XWIS='false'; $env:NOX_NET_NAT='false'; $env:NOX_SRV_NO_OBSERVER='1'
  $srv = Start-Process -FilePath (Join-Path $bld 'opennox-server.exe') `
    -ArgumentList '-data',"`"$game`"",'-serveronly','-noDraw','-port','18590','-autoexec','"load estate"' `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr
  $wait = 90
  "대조군 서버(opennox-server) pid=$($srv.Id)"
} else {
  if ($Judge) { $srvArgs = @('--port','18590','--standalone','--map',$mapArg,'--mode','arena') }
  else        { $srvArgs = @('--no-spawn-invuln','--port','18590','--map',$mapArg) }
  if ($SrvExtra -ne '') { $srvArgs += ($SrvExtra -split '\s+' | Where-Object { $_ -ne '' }) }
  $srv = Start-Process -FilePath (Join-Path $root 'build\Release\nox_server.exe') -ArgumentList $srvArgs `
    -PassThru -WindowStyle Minimized -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr
  $wait = 6
  "우리 서버 pid=$($srv.Id) · 인자: $($srvArgs -join ' ')"
}
Start-Sleep -Seconds $wait
if ($srv.HasExited) { "서버가 바로 죽었다 exit=$($srv.ExitCode)"; exit 1 }

$env:NOX_DUMP = (Join-Path $scratch "$Tag.dump.txt")
$env:NOX_JOIN_ADDR = '127.0.0.1:18590'
if ($NoWalk) {
  # d7 처럼 **걷지 않아도 흐르는 것**을 잴 때. 대조군은 자동 걷기 판에서 스스로 깨지므로
  # 양쪽 다 가만히 세워 두고 같은 잣대로 잰다(09-23)
  [Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null)
} else {
  $env:NOX_AUTO_WALK = '1'
}
$env:NOX_AUTO_WALK_TURN = '32'
$env:NOX_JOIN_CLASS = '1'
if ($Ref) {
  # 대조군은 들어오면 이미 게임 안이라 이 왼클릭이 거꾸로 관전을 켠다(main.go:753~757).
  # 09-23 — 이걸 보낸 판에서 대조군 서버가 패닉으로 죽었다. 대조군에서는 안 보낸다
  $env:NOX_NO_OBSERVER_RELEASE = '1'
  [Environment]::SetEnvironmentVariable('NOX_OBSERVER_CLICKS', $null)
} else {
  [Environment]::SetEnvironmentVariable('NOX_NO_OBSERVER_RELEASE', $null)
  $env:NOX_OBSERVER_CLICKS = '1'
}
$env:NOX_XWIS = 'false'
$env:NOX_NET_NAT = 'false'
[Environment]::SetEnvironmentVariable('NOX_AUTO_WALK_FACE', $null)
# 커서를 몸에서 100 밖으로 알리면 서버가 달리기로 친다(Server.cpp:4296) — 걷기와 갈라 재려고
if ($Cursor -ne '') { $env:NOX_AUTO_CURSOR = $Cursor } else { [Environment]::SetEnvironmentVariable('NOX_AUTO_CURSOR', $null) }

# ★ 09-23 저녁 — **정품 설정을 지킨다**(companion.ps1 과 같은 수). OpenNox 는 같은 `nox.cfg` 를 자기 꼴(65540)로
#   덮어쓰고, 그 설정이 남으면 정품이 캐릭터 로드에서 꺼진다. 이 주행기는 되돌리지 않아서
#   nine-defects 0-c 판 둘이 정품 설정을 덮었다(16:19 — 사본 `nox.cfg.keep` 으로 손으로 되돌림)
$cfg  = Join-Path $game 'nox.cfg'
$지킴 = Join-Path $game 'nox.cfg.crashrun-keep'
Copy-Item $cfg $지킴 -Force
$cfgHash = (Get-FileHash $cfg).Hash

$cli = Start-Process -FilePath (Join-Path $bld 'opennox.exe') -WorkingDirectory $bld `
  -ArgumentList '-data',"`"$game`"" -PassThru -RedirectStandardError $cliErr
"클라 pid=$($cli.Id) · $Secs 초 주행"

$결과 = ''
for ($i=1; $i -le $Secs; $i++) {
  Start-Sleep -Seconds 1
  $cli.Refresh(); $srv.Refresh()
  if ($cli.HasExited) { $결과 = "★ 클라가 $i 초에 죽음 (exit=$($cli.ExitCode))"; break }
  if ($srv.HasExited) { $결과 = "★ 서버가 $i 초에 죽음 (exit=$($srv.ExitCode))"; break }
}
if (-not $결과) { $결과 = "$Secs 초 동안 둘 다 살아 있었다" }
$결과

if (-not $cli.HasExited) { $cli.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 4; $cli.Refresh() }
if (-not $cli.HasExited) { $cli | Stop-Process -Force }
Start-Sleep -Seconds 2
$srv.Refresh()
if (-not $srv.HasExited) { $srv | Stop-Process -Force }
if ((Get-FileHash $cfg).Hash -ne $cfgHash) {
  Copy-Item $지킴 $cfg -Force
  "클라가 nox.cfg 를 덮었다 — 주행 전 것으로 되돌림"
}

"===== 클라 로그: 깜빡임 =====" 
if (Test-Path $cliLog) { (Select-String -Path $cliLog -Pattern '깜빡임' | Select-Object -Last 2).Line } else { '(클라 로그 없음)' }