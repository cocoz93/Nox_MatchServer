# rcon.ps1 — OpenNox 전용 서버의 원격 콘솔에 붙어 명령을 보낸다.
#
#   .\tools\rcon.ps1 -Cmds 'show game','list users','say MARK IDLE'
#   .\tools\rcon.ps1 -Cmds 'load estate' -Port 7777
#
# 왜 따로 있나 — 정품 호스트의 콘솔은 **평문 텔넷**(tools/console.ps1)인데,
# OpenNox 의 `-rcon` 은 **SSH** 다(`SSH-2.0-OpenNox-v1.9.0-dev`). 붙는 법이 아예 다르다.
#
# 서버 띄우는 법:
#   $env:NOX_DUMP='<덤프 경로>'
#   opennox-server.exe -serveronly -noDraw -port 18590 `
#     -autoexec "load estate" -rcon :7777 -rcon-pass <비번>
#
# ⚠ 함정 셋 — 셋 다 한 번씩 당했다.
#  1. **`-data` 에 공백 있는 경로를 주면 쪼개진다** (`C:\Program` 으로 잘림).
#     레지스트리 자동탐지가 이미 맞으니 **`-data` 를 아예 빼면 된다**
#  2. **`exec` 요청은 막혀 있다** — `ssh host "명령"` 은 `exec request failed on channel 0`.
#     대화형 셸(`-tt`)에 **stdin 으로 밀어 넣어야** 한다
#  3. **파워셸 파이프는 BOM 을 앞에 붙인다** — 첫 명령이 `﻿show game` 이 되어 Syntax error.
#     그래서 여기서는 명령을 **BOM 없는 ASCII 파일**로 쓰고 `cmd /c type` 으로 흘린다
param(
  [string[]]$Cmds = @(),
  [string]$Server = '127.0.0.1',
  [int]$Port = 7777,
  [string]$User = 'nox',
  [string]$Pass = 'CHANGE_ME',
  [string]$Out = '',
  [int]$Wait = 900,       # 명령 사이 기다림(ms). 응답이 길면 늘린다
  [switch]$Raw            # 배너·프롬프트를 걷어내지 않고 그대로 보여 준다
)
$ErrorActionPreference = 'Continue'
if (-not $Cmds -or $Cmds.Count -eq 0) { Write-Host "쓸 명령이 없다"; return }

$ssh = "$env:SystemRoot\System32\OpenSSH\ssh.exe"
if (-not (Test-Path $ssh)) { Write-Host "[FAIL] ssh.exe 를 못 찾았다: $ssh"; return }

$tmp = Join-Path $env:TEMP ("noxrcon_" + $PID)
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$askPath = Join-Path $tmp 'askpass.cmd'
$cmdPath = Join-Path $tmp 'cmds.txt'
$outPath = Join-Path $tmp 'out.txt'

# 비밀번호는 SSH_ASKPASS 로 넣는다 (ssh 는 stdin 에서 비번을 안 읽는다)
Set-Content -Path $askPath -Value "@echo off`r`necho $Pass" -Encoding ascii
# ★ BOM 없는 ASCII 로 써야 첫 줄이 안 깨진다
[IO.File]::WriteAllText($cmdPath, ($Cmds -join "`n") + "`n", (New-Object System.Text.ASCIIEncoding))

$env:SSH_ASKPASS = $askPath
$env:SSH_ASKPASS_REQUIRE = 'force'
$env:DISPLAY = 'localhost:0'

# ⚠ 명령을 한꺼번에 밀어 넣으면 `exit` 이 먼저 닿아 **응답이 오기 전에 닫힌다.**
#   그래서 stdin 을 직접 쥐고 한 줄씩 넣으며 사이를 띄운다.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ssh
$psi.Arguments = "-tt -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o ConnectTimeout=8 -p $Port $User@$Server"
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.EnvironmentVariables['SSH_ASKPASS'] = $askPath
$psi.EnvironmentVariables['SSH_ASKPASS_REQUIRE'] = 'force'
$psi.EnvironmentVariables['DISPLAY'] = 'localhost:0'

$proc = [System.Diagnostics.Process]::Start($psi)
$sb = New-Object System.Text.StringBuilder
$onOut = { if ($EventArgs.Data -ne $null) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
$e1 = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action $onOut -MessageData $sb
$e2 = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived  -Action $onOut -MessageData $sb
$proc.BeginOutputReadLine(); $proc.BeginErrorReadLine()

Start-Sleep -Milliseconds 2500          # 인증·배너를 기다린다
# ⚠ 파워셸이 **첫 쓰기에 BOM 을 붙인다** — 그대로 두면 첫 명령이 `﻿show game` 이 되어 Syntax error.
#   빈 줄을 먼저 보내 BOM 을 거기서 흘려보낸다.
$proc.StandardInput.WriteLine('')
$proc.StandardInput.Flush()
Start-Sleep -Milliseconds 300
foreach ($c in $Cmds) {
  $proc.StandardInput.WriteLine($c)
  $proc.StandardInput.Flush()
  Start-Sleep -Milliseconds $Wait
}
# ⚠⚠ **`exit` 을 보내지 말 것 — 서버가 통째로 종료된다.**
#   정품 콘솔에서는 「메인 메뉴로 나가기」지만, OpenNox 전용 서버에서는 프로세스가 끝난다
#   (로그: `[rcon] command: "exit"` → `[game] client not connected` → 종료).
#   그냥 stdin 을 닫고 ssh 를 내린다.
$proc.StandardInput.Close()
if (-not $proc.WaitForExit(3000)) { $proc.Kill() }
Start-Sleep -Milliseconds 400
Unregister-Event -SourceIdentifier $e1.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $e2.Name -ErrorAction SilentlyContinue

$text = $sb.ToString()
# ANSI 색코드를 걷어낸다 — 프롬프트가 색을 입혀 온다
$text = [regex]::Replace($text, "`e\[[0-9;]*m", '')
$text = $text -replace "\x1b\[[0-9;]*m", ''

if (-not $Raw) {
  $lines = $text -split "`r?`n"
  $keep = @()
  $started = $false
  foreach ($l in $lines) {
    if (-not $started) {
      # 배너는 |__/ 로 끝난다
      if ($l -match '^\s*\|__/\s*$') { $started = $true }
      continue
    }
    $keep += ($l -replace '^nox@opennox:~\$\s*', '')
  }
  $text = ($keep -join "`n").Trim()
}

if ($Out) { [IO.File]::WriteAllText($Out, $text, (New-Object System.Text.UTF8Encoding($false))) }
Write-Host $text
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
