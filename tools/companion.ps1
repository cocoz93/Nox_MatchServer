# companion.ps1 — 판정용 「상대 역할」 클라(OpenNox)를 띄우고, **나갈 때 덮어쓴 정품 설정을 되돌린다.**
#
# 왜 — OpenNox 와 정품은 같은 `nox.cfg` 를 쓰는데 **꼴이 다르다**
#   OpenNox : Version 65540 · Fullscreen/Stretched 키 있음
#   정품    : Version 65538 · 그 키 없음
# 09-23 에 OpenNox 가 쓴 설정이 남은 채로 정품이 들어가려 하자 **캐릭터 로드에서 꺼졌다.**
# OpenNox 는 판마다 죽으면서 그 파일을 다시 썼고, 그때마다 정품이 막혔다.
param([string]$Addr = '127.0.0.1:18590', [int]$Class = 1)
$ErrorActionPreference = 'Stop'
$뿌리 = Split-Path -Parent $PSScriptRoot
$빌드 = Join-Path $뿌리 'third_party\opennox-build'
$게임 = 'C:\Program Files\GOG Galaxy\Games\Nox'
$cfg  = Join-Path $게임 'nox.cfg'
$지킴 = Join-Path $게임 'nox.cfg.keep'

Copy-Item $cfg $지킴 -Force
$본 = ([IO.File]::ReadAllText($cfg, [Text.Encoding]::ASCII) -split "`r?`n" | Where-Object { $_ -match '^Version' })
"설정 사본 떠 둠 — $본"

$env:NOX_JOIN_ADDR = $Addr
$env:NOX_JOIN_CLASS = "$Class"
$env:NOX_OBSERVER_CLICKS = '1'
$env:NOX_XWIS = 'false'
$env:NOX_NET_NAT = 'false'
[Environment]::SetEnvironmentVariable('NOX_AUTO_WALK', $null)
[Environment]::SetEnvironmentVariable('NOX_DUMP', $null)

$오류 = Join-Path $빌드 'logs\companion.err.txt'
$c = Start-Process -FilePath (Join-Path $빌드 'opennox.exe') -WorkingDirectory $빌드 `
     -ArgumentList '-data',"`"$게임`"" -PassThru -RedirectStandardError $오류
"상대 클라 pid $($c.Id) — 25초쯤 뒤 맵에 섭니다"

$c.WaitForExit()
Start-Sleep -Seconds 1
$뒤 = ([IO.File]::ReadAllText($cfg, [Text.Encoding]::ASCII) -split "`r?`n" | Where-Object { $_ -match '^Version' })
if ($뒤 -ne $본) {
  Copy-Item $지킴 $cfg -Force
  "상대 클라가 나가며 설정을 덮었다($뒤) — 정품 것으로 되돌림($본)"
} else {
  "상대 클라 종료 — 설정은 그대로($뒤)"
}
$t2 = [IO.File]::ReadAllText($오류, [Text.Encoding]::UTF8)
if ($t2 -match 'panic: (.+)') { "그리고 죽은 까닭 : " + $matches[1] }