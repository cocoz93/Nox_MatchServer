param([Parameter(Mandatory=$true)][int]$Pose, [int]$Frames = 14, [int]$EveryMs = 250, [int]$Join = 30)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bld  = Join-Path $root 'third_party\opennox-build'
$game = 'C:\Program Files\GOG Galaxy\Games\Nox'
# 산출물은 리포 안 `_evidence/_runs`(gitignore) 에. 세션 임시 폴더를 박아 두면 다음 세션에 안 돈다
$scratch = Join-Path $root '_evidence\_runs'
if (-not (Test-Path $scratch)) { New-Item -ItemType Directory -Path $scratch -Force | Out-Null }
$dir = Join-Path $scratch ("pose{0}" -f $Pose)
if (Test-Path $dir) { Get-ChildItem $dir -Filter 'f_*.png' | ForEach-Object { [IO.File]::Delete($_.FullName) } }
else { New-Item -ItemType Directory -Path $dir | Out-Null }

Get-Process opennox,opennox-server,nox_server -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$mapArg = "`"$game\maps\Estate\Estate.map`""
$srvArgs = @('--port','18590','--standalone','--map',$mapArg,'--mode','arena','--quiet','--anim-pose',"$Pose",'--no-join-token')   # 10-10 토큰 작업 2
$srv = Start-Process -FilePath (Join-Path $root 'build\Release\nox_server.exe') -ArgumentList $srvArgs `
  -PassThru -WindowStyle Minimized -RedirectStandardOutput (Join-Path $scratch "pose$Pose.srv.txt") `
  -RedirectStandardError (Join-Path $scratch "pose$Pose.srverr.txt")
Start-Sleep -Seconds 6

$env:NOX_JOIN_ADDR = '127.0.0.1:18590'
$env:NOX_JOIN_CLASS = '1'
$env:NOX_OBSERVER_CLICKS = '1'
$env:NOX_XWIS = 'false'
$env:NOX_NET_NAT = 'false'
[Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null)
[Environment]::SetEnvironmentVariable('NOX_AUTO_WALK_FACE', $null)
[Environment]::SetEnvironmentVariable('NOX_DUMP', $null)

$cli = Start-Process -FilePath (Join-Path $bld 'opennox.exe') -WorkingDirectory $bld `
  -ArgumentList '-data',"`"$game`"" -PassThru
"자세 $Pose · 서버 $($srv.Id) · 클라 $($cli.Id) — 접속까지 $Join 초 기다림"
Start-Sleep -Seconds $Join

for ($i=0; $i -lt $Frames; $i++) {
  $out = Join-Path $dir ("f_{0:d2}.png" -f $i)
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'capwin.ps1') -Pid_ $cli.Id -Out $out | Out-Null
  Start-Sleep -Milliseconds $EveryMs
}

$cli.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 4
$cli.Refresh(); if (-not $cli.HasExited) { $cli | Stop-Process -Force }
Start-Sleep -Seconds 1
$srv.Refresh(); if (-not $srv.HasExited) { $srv | Stop-Process -Force }

"===== 자세 $Pose — 프레임 사이 다른 픽셀 ====="
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'framediff.ps1') -Dir $dir