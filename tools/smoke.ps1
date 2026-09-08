# smoke.ps1 — 진짜 소켓으로 접속 절차를 왕복해 본다.
#
# 대조 장치(nox_replay)는 덤프를 재생할 뿐 **소켓을 타지 않는다.**
# 여기서만 확인되는 것 = bind·recvfrom·sendto 가 실제로 이어져 있는가.
#
#   .\tools\smoke.ps1              기본 포트 18599
#   .\tools\smoke.ps1 -Port 18590
param([int]$Port = 18599)
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\Release\nox_server.exe'
if (-not (Test-Path $exe)) { Write-Host "[FAIL] 서버가 없다: $exe"; exit 1 }

$srv = Start-Process -FilePath $exe -ArgumentList '--port', $Port, '--name', 'SMOKE', '--quiet' `
                     -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 700

$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = 2000
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse('127.0.0.1'), $Port)
$any = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)

$fail = 0
function Check($cond, $what) {
  if ($cond) { Write-Host "  [OK]   $what" }
  else { Write-Host "  [FAIL] $what"; $script:fail++ }
}

# ── 1. 서버 찾기 — 토큰을 그대로 되돌려야 클라가 자기 요청으로 알아본다
[void]$udp.Send(([byte[]](0x00,0x00,0x0c, 0,0,0,0,0, 0xEF,0xBE,0xAD,0xDE)), 12, $ep)
$r = $null
try { $r = $udp.Receive([ref]$any) } catch { }
Write-Host "1) 서버 찾기"
Check ($null -ne $r) "응답이 온다"
if ($r) {
  Check ($r[2] -eq 0x0d) ("오프코드가 0x0d SERVER_INFO ({0:x2})" -f $r[2])
  Check ($r[0] -eq 0x00 -and $r[1] -eq 0x00) "머리가 비신뢰·스트림0·순번0"
  $tokOk = ($r[44] -eq 0xEF -and $r[45] -eq 0xBE -and $r[46] -eq 0xAD -and $r[47] -eq 0xDE)
  Check $tokOk "토큰이 본문 41번(패킷 44번)에 그대로 돌아왔다"
  $name = [Text.Encoding]::ASCII.GetString($r[72..($r.Length-2)])
  Check ($name -eq 'SMOKE') "서버 이름이 인자대로 '$name'"
}

# ── 2. 접속 시도 — 지금은 조건 없이 받아준다
$tryJoin = New-Object byte[] 100
$tryJoin[2] = 0x0e
[void]$udp.Send($tryJoin, 100, $ep)
$r2 = $null
try { $r2 = $udp.Receive([ref]$any) } catch { }
Write-Host "2) 접속 시도"
Check ($null -ne $r2 -and $r2.Length -eq 3 -and $r2[2] -eq 0x14) "0x14 SERVER_JOIN_OK 3바이트"

# ── 3. 접속 — 수락 한 조각에 메시지 둘이 실린다
[void]$udp.Send(([byte[]](0xff,0x00,0x00)), 3, $ep)
$r3 = $null
try { $r3 = $udp.Receive([ref]$any) } catch { }
Write-Host "3) 접속 수락"
Check ($null -ne $r3 -and $r3.Length -eq 10) "10바이트"
if ($r3 -and $r3.Length -eq 10) {
  Check ($r3[0] -eq 0x80) "신뢰 전송(0x80)"
  Check ($r3[2] -eq 0x1f) "① 0x1f ACCEPTED"
  Check ($r3[4] -eq 0x01) "② 0x01 SERVER_ACCEPT"
  Check ($r3[9] -ne 0)    ("XOR 키가 실렸다 (0x{0:x2})" -f $r3[9])
}

# ── 4. 핑 — 받은 값을 그대로 되돌린다. 스트림 1 을 쓴다
#
# ⚠ 키를 준 뒤로는 **보내는 쪽도 뒤집어서 보낸다.** 평문으로 보내면 서버가
#   복호화하다 쓰레기가 되어 아무 답도 안 나온다(그게 맞는 동작이다).
$k = if ($r3 -and $r3.Length -eq 10) { $r3[9] } else { 0 }
$ping = @(); foreach ($b in @(0x01,0x04,0x06, 0x11,0x22,0x33,0x44)) { $ping += [byte]($b -bxor $k) }
[void]$udp.Send([byte[]]$ping, 7, $ep)
$r4 = $null
try { $r4 = $udp.Receive([ref]$any) } catch { }
Write-Host "4) 핑"
if ($null -ne $r4 -and $r4.Length -eq 7) {
  $d = @(); foreach ($b in $r4) { $d += ($b -bxor $k) }
  Check ($d[2] -eq 0x08) "0x08 CLIENT_PONG"
  Check ($d[0] -eq 0x01) "스트림 1"
  Check ($d[3] -eq 0x11 -and $d[6] -eq 0x44) "받은 시각 4바이트를 그대로 되돌렸다"
} else {
  Check $false "7바이트 응답"
}

# ── 4.5 게임 진입 — 클라가 「받았다」고 알리면 서버가 게임에 넣어 준다
#
# 본문 153바이트. 앞 97바이트가 플레이어 정보이고, 서버는 그것을 NEW_PLAYER 에
# 그대로 되돌린다(실측). 여기서는 자리만 채운다.
$acc = New-Object byte[] 156
$acc[0] = 0x01; $acc[1] = 0x05; $acc[2] = 0x20
$accX = @(); foreach ($b in $acc) { $accX += [byte]($b -bxor $k) }
[void]$udp.Send([byte[]]$accX, $accX.Length, $ep)
Start-Sleep -Milliseconds 200

# ── 5. 틱 — 접속이 끝나면 서버가 스스로 매 틱 상태를 뿌린다
#
# 여기서 확인하는 것 = 30Hz 루프가 실제로 돌고, 시각과 상태 갱신이 짝지어 나가는가.
# 클라가 아무것도 안 보내도 와야 한다.
Write-Host "5) 틱 방송"
$udp.Client.ReceiveTimeout = 1500
$tick = 0; $upd = 0
# ★ 접속 뒤 첫 몇 틱은 **시각만** 나간다 — 첫 세계 스냅샷을 기다리는 구간이다
#   (Server.h 의 kSnapshotDelayTicks). 그만큼 더 받아야 갱신이 잡힌다.
for ($i = 0; $i -lt 30; $i++) {
  try {
    $g = $udp.Receive([ref]$any)
    $d = @(); foreach ($b in $g) { $d += ($b -bxor $k) }
    if ($d[2] -eq 0x27) { $tick++ }
    if ($d[2] -eq 0xa4) { $upd++ }
  } catch { break }
}
Check ($tick -ge 3) "시각(0x27)이 스스로 온다 — $tick 건"
Check ($upd -ge 3)  "상태 갱신(0xa4)이 스스로 온다 — $upd 건"

# ── 6. 이동 — 입력을 보내면 상태 갱신의 좌표가 움직여야 한다
#
# 본문 = 길이 1B + [코드 4B + 데이터]. 방향(1) 뒤에 앞으로(2)를 넣는다.
Write-Host "6) 이동"
# 쌓인 것을 버린다 — 서버가 30Hz 로 계속 뿌려 큐에 옛 패킷이 남는다.
# 이걸 안 하면 지나간 자리를 읽어 「안 움직였다」로 잘못 본다.
function Clear-Queue {
  while ($udp.Available -gt 0) {
    try { [void]$udp.Receive([ref]$any) } catch { break }
  }
}
function Get-Pos {
  Clear-Queue
  for ($i = 0; $i -lt 20; $i++) {
    try {
      $g = $udp.Receive([ref]$any)
      $d = @(); foreach ($b in $g) { $d += ($b -bxor $k) }
      # 0xa4 상태 갱신의 머리는 두 꼴이다 (정품과 같게 맞춰 둔 규약):
      #   첫 갱신  [op][ff][번호2][종류2][x2][y2]...   ← 전체 번호로 소개
      #   그 뒤    [op][01][x2][y2]...                 ← 별칭 한 바이트
      if ($d[2] -eq 0xa4 -and $d[3] -eq 0xff) {
        return @{ x = ($d[8] + $d[9] * 256); y = ($d[10] + $d[11] * 256) }
      }
      if ($d[2] -eq 0xa4 -and $d[3] -eq 0x01) {
        return @{ x = ($d[4] + $d[5] * 256); y = ($d[6] + $d[7] * 256) }
      }
    } catch { return $null }
  }
  return $null
}
$before = Get-Pos
$inp = @(0x01, 0x05, 0x3f, 10,  1,0,0,0, 0,  2,0,0,0, 1)   # 방향 0, 앞으로
$inpX = @(); foreach ($b in $inp) { $inpX += [byte]($b -bxor $k) }
for ($i = 0; $i -lt 6; $i++) { [void]$udp.Send([byte[]]$inpX, $inpX.Length, $ep); Start-Sleep -Milliseconds 60 }
Start-Sleep -Milliseconds 150
$after = Get-Pos
if ($before -and $after) {
  Write-Host ("     자리: ({0},{1}) -> ({2},{3})" -f $before.x, $before.y, $after.x, $after.y)
  Check ($after.x -ne $before.x -or $after.y -ne $before.y) "입력을 받아 자리가 움직였다"
} else {
  Check $false "상태 갱신에서 자리를 읽었다"
}

# ── 7. 둘째 사람 — 서로가 서로의 상태 갱신에 실려야 한다
#
# M 의 「서로를 본다」가 이것이다. 둘째를 붙이고, 첫째가 받는 상태 갱신에
# 오브젝트가 둘로 늘어나는지 본다.
Write-Host "7) 둘이 서로 보임"
$udp2 = New-Object System.Net.Sockets.UdpClient
$udp2.Client.ReceiveTimeout = 1500
[void]$udp2.Send(([byte[]](0xff,0x00,0x00)), 3, $ep)
$k2 = 0
try {
  $r5 = $udp2.Receive([ref]$any)
  if ($r5.Length -eq 10) { $k2 = $r5[9] }
} catch { }
if ($k2 -ne 0) {
  $acc2 = New-Object byte[] 156
  $acc2[0] = 0x01; $acc2[1] = 0x05; $acc2[2] = 0x20
  $a2 = @(); foreach ($b in $acc2) { $a2 += [byte]($b -bxor $k2) }
  [void]$udp2.Send([byte[]]$a2, $a2.Length, $ep)
  Start-Sleep -Milliseconds 300
}
# 첫째가 받는 상태 갱신에서 오브젝트 수를 센다.
# 항목은 절대 좌표라 [00][별칭][x2][y2][덧붙이1] = 7바이트씩이고 끝은 00 00 00.
$objs = 0
for ($i = 0; $i -lt 25; $i++) {
  try {
    $g = $udp.Receive([ref]$any)
    $d = @(); foreach ($b in $g) { $d += ($b -bxor $k) }
    if ($d[2] -ne 0xa4) { continue }
    # 머리 길이는 별칭 꼴에 따라 다르다 (Get-Pos 의 주석 참조)
    #   ff : op1 + ff1 + 번호2 + 종류2 + x2 + y2 + 깃발1 + 애니1 = 12, 머리 앞 2바이트 더해 14
    #   01 : op1 + 01 1 + x2 + y2 + 깃발1 + 애니1              =  8, 머리 앞 2바이트 더해 10
    if     ($d[3] -eq 0xff) { $j = 14 }
    elseif ($d[3] -eq 0x01) { $j = 10 }
    else                    { continue }
    $n = 0
    while ($j + 2 -lt $d.Length) {
      if ($d[$j] -eq 0 -and $d[$j+1] -eq 0 -and $d[$j+2] -eq 0) { break }
      $n++; $j += 8      # 00 + 별칭 + x2 + y2 + 덧붙이1 + (다음 00)
    }
    if ($n -gt $objs) { $objs = $n }
  } catch { break }
}
Check ($k2 -ne 0) "둘째가 접속했다"
Check ($objs -ge 2) "첫째의 상태 갱신에 사람이 둘 실린다 — $objs 명"
$udp2.Close()

# ── 8. 주문 시전 — 한 조각에 주문과 이동을 이어 붙여 보낸다
#
# 여기서 확인하는 것 = 서버가 주문을 **정확히 22바이트로 먹는가**.
# 길이를 틀리면 뒤에 붙인 이동이 밀려 읽히지 않아 자리가 안 움직인다.
# 즉 자리가 움직이면 그 자체가 「경계를 맞게 잡았다」는 증거다.
#
#   79 | 주문번호 1 | 0 으로 채운 19 | 대상 플래그 1        = 22바이트
#   파이어볼 27 은 남에게 쏘는 것이라 마지막 자리가 0 이다(캡처 18건 전수).
Write-Host "8) 주문 시전 (파이어볼)"
$move = @(0x3f, 10,  1,0,0,0, 0,  2,0,0,0, 1)     # 방향 0, 앞으로
$spell = @(0x79, 27)
for ($i = 0; $i -lt 19; $i++) { $spell += 0 }
$spell += 0
Check ($spell.Length -eq 22) "주문 조각이 22바이트다 — $($spell.Length)"

function Send-And-Move($bytes, $times) {
  $x = @(); foreach ($b in $bytes) { $x += [byte]($b -bxor $k) }
  $a = Get-Pos
  for ($i = 0; $i -lt $times; $i++) { [void]$udp.Send([byte[]]$x, $x.Length, $ep); Start-Sleep -Milliseconds 60 }
  Start-Sleep -Milliseconds 150
  $b2 = Get-Pos
  return @{ a = $a; b = $b2 }
}

# (a) 먼저 이동만 — 이 자리에서 원래 움직이는지 기준을 잡는다.
$r = Send-And-Move (@(0x01, 0x05) + $move) 6
if ($r.a -and $r.b) {
  Write-Host ("     이동만: ({0},{1}) -> ({2},{3})" -f $r.a.x, $r.a.y, $r.b.x, $r.b.y)
  Check ($r.b.x -ne $r.a.x -or $r.b.y -ne $r.a.y) "(기준) 이동만 보내면 움직인다"
} else { Check $false "(기준) 상태 갱신을 읽었다" }

# (b) 주문 + 이동을 한 조각에 — 주문을 22바이트로 정확히 먹어야 뒤 이동이 읽힌다.
$r2 = Send-And-Move (@(0x01, 0x05) + $spell + $move) 6
if ($r2.a -and $r2.b) {
  Write-Host ("     주문+이동: ({0},{1}) -> ({2},{3})" -f $r2.a.x, $r2.a.y, $r2.b.x, $r2.b.y)
  Check ($r2.b.x -ne $r2.a.x -or $r2.b.y -ne $r2.a.y) "주문 뒤에 붙인 이동까지 읽었다 — 경계가 맞다"
} else { Check $false "주문을 보낸 뒤에도 상태 갱신이 온다" }

$udp.Close()
Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue

Write-Host ""
if ($fail -eq 0) { Write-Host "[OK] 소켓 왕복 전부 통과"; exit 0 }
else { Write-Host "[FAIL] $fail 건 어긋남"; exit 1 }
