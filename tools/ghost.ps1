# ghost.ps1 — 화면 없는 **가짜 플레이어**. 접속해서 그 자리에 계속 서 있는다.
#
# 왜 만드나 — 「둘이 서로 보이나」를 재려면 진짜 클라가 둘 필요했다.
#   그런데 진짜 클라는 메뉴를 마우스로 클릭해 넣어야 하고, 그 클릭이 자주 빗나가
#   하루의 절반이 판정이 아니라 클릭 고치는 데 갔다(2026-09-09).
#   한쪽을 이 가짜로 바꾸면 **클릭이 절반**이고 이쪽은 어긋날 일이 없다.
#
# 접속 절차는 tools/smoke.ps1 에서 그대로 가져왔다(실측으로 통과한 순서다):
#   0x0c 서버찾기 → 0x0e 접속시도 → 0x00 접속 → 0x20 준비완료 → 그 뒤 핑·입력 유지
#
#   .\tools\ghost.ps1 -Port 18590 -Seconds 300
param(
  [int]$Port = 18590,
  [string]$Host_ = '127.0.0.1',
  [int]$Seconds = 300,
  [string]$Tag = 'ghost'
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$log  = Join-Path $repo ("_evidence\{0}.log" -f $Tag)
function Say([string]$s) {
  $line = ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss.fff'), $s)
  Write-Host $line
  [IO.File]::AppendAllText($log, $line + "`r`n", (New-Object Text.UTF8Encoding($false)))
}
if (Test-Path $log) { [IO.File]::Delete($log) }

$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = 2000
$ep  = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($Host_), $Port)
$any = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)

# ① 서버 찾기 — 토큰을 그대로 돌려받아야 내 요청인 줄 안다
[void]$udp.Send(([byte[]](0x00,0x00,0x0c, 0,0,0,0,0, 0xEF,0xBE,0xAD,0xDE)), 12, $ep)
$r = $null; try { $r = $udp.Receive([ref]$any) } catch {}
if (-not $r -or $r[2] -ne 0x0d) { Say '[FAIL] 서버 찾기 실패'; exit 1 }
Say '서버 찾음'

# ② 접속 시도
$tryJoin = New-Object byte[] 100; $tryJoin[2] = 0x0e
[void]$udp.Send($tryJoin, 100, $ep)
$r2 = $null; try { $r2 = $udp.Receive([ref]$any) } catch {}
if (-not $r2 -or $r2[2] -ne 0x14) { Say '[FAIL] 접속 시도 거절'; exit 1 }

# ③ 접속 — 여기서 XOR 키를 받는다. **이 뒤로는 보내는 것도 뒤집어야** 한다
[void]$udp.Send(([byte[]](0xff,0x00,0x00)), 3, $ep)
$r3 = $null; try { $r3 = $udp.Receive([ref]$any) } catch {}
if (-not $r3 -or $r3.Length -ne 10) { Say '[FAIL] 수락 없음'; exit 1 }
$k = $r3[9]
Say ('접속됨 · XOR 키 0x{0:x2}' -f $k)

function SendX([byte[]]$plain) {
  $x = New-Object byte[] $plain.Length
  for ($i = 0; $i -lt $plain.Length; $i++) { $x[$i] = [byte]($plain[$i] -bxor $k) }
  [void]$udp.Send($x, $x.Length, $ep)
}

# ④ 준비 완료 — 이걸 보내야 서버가 게임에 넣어 준다
$acc = New-Object byte[] 156
$acc[0] = 0x01; $acc[1] = 0x05; $acc[2] = 0x20
SendX $acc
Say '게임 진입 알림 보냄'

# ⑤ 살아 있기 — ★ 조용하면 서버가 걷어낸다(kSessionTimeoutTicks).
#   방향(코드 1) 입력을 꾸준히 보내 「살아 있고 가만히 서 있는 사람」이 된다.
$t0 = Get-Date
$sent = 0; $got = 0
$stream0 = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  # 0x3f PLAYER_INPUT — 길이 1 + [코드 4] + 데이터 1. 코드 1 = 바라보는 쪽
  SendX ([byte[]](0x00, $stream0, 0x3f, 0x05, 0x01,0x00,0x00,0x00, 0x82))
  $sent++
  # 받은 것은 버린다. 안 비우면 소켓 버퍼가 찬다
  $udp.Client.ReceiveTimeout = 40
  for ($j = 0; $j -lt 20; $j++) {
    try { $null = $udp.Receive([ref]$any); $got++ } catch { break }
  }
  Start-Sleep -Milliseconds 33
  if ($sent % 300 -eq 0) { Say ("살아 있음 — 보냄 {0} · 받음 {1}" -f $sent, $got) }
}
Say ("끝 — 보냄 {0} · 받음 {1}" -f $sent, $got)
$udp.Close()