# onox_restart.ps1 — 오픈녹스 클라를 다시 띄우고 새 pid 를 돌려준다.
#
# 왜 필요한가 — 접속이 끊기면 클라가 「The server is not responding」 대화상자에서
# 굳는다. ★ **그 대화상자는 클릭이 안 먹는다**(2026-09-07·09-08 두 번 실측).
# ESC 로 닫히는 때도 있으나, 이미 죽은 세션에 초당 30번씩 재전송을 계속하는
# 상태라 그대로 두면 프록시 덤프가 그 쓰레기로 찬다. 다시 띄우는 편이 싸다.
#
# ★ **툴 세션이 끝나면 자식 프로세스가 같이 죽는다** → WMI 로 띄운다.
#
#   $pid = & .\tools\onox_restart.ps1
param(
  [int]$WaitSeconds = 16,
  # ★ 창모드로 띄우면 opennox.yml 의 크기를 쓰고, 끄면 전체화면이다.
  #   onox_join.ps1 의 좌표는 **1920x1080 전체화면 기준**이라 기본을 전체화면으로 둔다.
  [switch]$Windowed
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'third_party\opennox\opennox.exe'
$logs = Join-Path $root '_evidence\onox_cli_auto'

Get-Process opennox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$cmd = '"' + $exe + '"' + $(if ($Windowed) { ' -window' } else { '' }) + ' -logs "' + $logs + '"'
$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = $cmd; CurrentDirectory = (Split-Path -Parent $exe) }
if ($res.ReturnValue -ne 0) { Write-Host "[FAIL] 못 띄움 반환=$($res.ReturnValue)"; exit 1 }

# 창이 생길 때까지 기다린다 — 바로 쓰면 MainWindowHandle 이 0 이다
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
  $p = Get-Process -Id $res.ProcessId -ErrorAction SilentlyContinue
  if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { break }
  Start-Sleep -Milliseconds 500
  if ($p) { $p.Refresh() }
}
Write-Host ("클라 pid={0}" -f $res.ProcessId)
$res.ProcessId
