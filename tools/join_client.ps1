# join_client.ps1 — 클라를 띄워 목록의 첫 서버에 접속시킨다 (캐릭터 선택까지).
#   .\tools\join_client.ps1 -Sp .\tools -Ev <작업폴더> -Exe Game2.exe -CharRow 2 -Tag c1
# CharRow: Load Character 목록의 몇 번째 줄인가 (1=첫 줄, 2=둘째 줄 …)
#
# -Stage list : 기동해서 서버 목록까지만 간다 (PID 를 찍고 멈춘다)
# -Stage join : 이미 목록에 있는 클라(-Pid_)를 접속시킨다
# -Stage all  : 둘 다 (기본)
#
# ★ 호스트는 게임이 시작되면 탐색 응답을 멈춘다. 그래서 클라 둘을 붙이려면
#   둘 다 먼저 목록까지 세워둔 뒤(-Stage list) 연달아 접속시켜야(-Stage join) 한다.
#
# 파워셸은 변수 이름의 대소문자를 가리지 않는다. $Tag 와 $tag 는 같은 변수다.
#   (한 번 당했다 — 스크린샷이 전부 mp_mp.png 같은 이름으로 저장됐다)
# 기동 대기가 짧으면 메인메뉴가 아직 안 떠서 클릭이 통째로 헛돈다. 넉넉히 기다린다.
param([string]$Sp, [string]$Ev, [string]$Exe = 'Game2.exe', [int]$CharRow = 2,
      [string]$Tag = 'c', [int]$BootWait = 45, [string]$Stage = 'all', [int]$Pid_ = 0)
$dir = "C:\Program Files\GOG Galaxy\Games\Nox"
function Step($n, $ok) { Write-Host ("[{0}] {1}" -f $(if ($ok) { "OK  " } else { "FAIL" }), $n); return [bool]$ok }
function Nav($procId, $x, $y, $shotName, [switch]$Rel) {
  $navArgs = @{ Pid_ = $procId; Cx = $x; Cy = $y; Click = $true
                Out = (Join-Path $Ev ($Tag + "_" + $shotName + ".png")) }
  if ($Rel) { $navArgs.NoActivate = $true; $navArgs.NoHome = $true }
  $r = & "$Sp\nav.ps1" @navArgs 2>&1
  return (($r | Out-String) -match 'shot ok')
}

$id = $Pid_
if ($Stage -eq 'all' -or $Stage -eq 'list') {
  $p = Start-Process -FilePath "$dir\$Exe" -WorkingDirectory $dir -PassThru
  for ($i = 0; $i -lt $BootWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    $h = (Get-Process -Id $p.Id -ErrorAction SilentlyContinue).MainWindowHandle
    if ($h -and $h -ne 0 -and $i -ge 30) { break }
  }
  Start-Sleep -Seconds 6
  if (-not (Step "기동 PID=$($p.Id)" (-not $p.HasExited))) { return }
  $id = $p.Id
  if (-not (Step "멀티플레이" (Nav $id 595 337 'mp'))) { return }
  Start-Sleep -Seconds 3
  if (-not (Step "네트워크" (Nav $id 660 540 'lan'))) { return }
  Start-Sleep -Seconds 3
  foreach ($k in 1..2) { [void](Nav $id 235 605 "ref$k"); Start-Sleep -Seconds 4 }
  Write-Host "PID=$id"
}

if ($Stage -eq 'all' -or $Stage -eq 'join') {
  if (-not $id) { Write-Host "FAIL: -Pid_ 가 필요하다"; return }
  if (-not (Step "서버 줄" (Nav $id 527 114 'row'))) { return }
  Start-Sleep -Seconds 2
  if (-not (Step "참여" (Nav $id 28 202 'join' -Rel))) { return }
  Start-Sleep -Seconds 3
  $rowY = 225 + ($CharRow - 1) * 33
  if (-not (Step "캐릭터 $CharRow 줄" (Nav $id 592 $rowY 'char'))) { return }
  Start-Sleep -Seconds 2
  if (-not (Step "불러오기" (Nav $id 647 759 'load'))) { return }
  Start-Sleep -Seconds 18
  & "$Sp\capwin.ps1" -Pid_ $id -Out (Join-Path $Ev ($Tag + "_ingame.png")) | Out-Null
  Write-Host "PID=$id"
}
