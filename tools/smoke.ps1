# smoke.ps1 — 진짜 소켓으로 접속 절차를 왕복해 본다.
#
# 대조 장치(nox_replay)는 덤프를 재생할 뿐 **소켓을 타지 않는다.**
# 여기서만 확인되는 것 = bind·recvfrom·sendto 가 실제로 이어져 있는가.
#
#   .\tools\smoke.ps1              기본 포트 18599
#   .\tools\smoke.ps1 -Port 18590
#   .\tools\smoke.ps1 -Map "C:\...\maps\estate\Estate.map"   독립 모드(사람 확인과 같은 실행 조건)
#
# ★ 10-06 — `-Map` 이 없으면 **녹음 기반 모드**(스냅샷이 녹음본)로 뜬다. 사람이 정품으로 붙는 것은
#   독립 모드(`--standalone --map … --mode arena`, `Docs/HOW_TO_PLAY.md`)라 둘을 따로 건다(CMakeLists.txt).
param([int]$Port = 18599, [string]$Map = '')
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'build\Release\nox_server.exe'
if (-not (Test-Path $exe)) { Write-Host "[FAIL] 서버가 없다: $exe"; exit 1 }

$srvArgs = @('--port', $Port, '--name', 'SMOKE', '--quiet', '--no-join-token')   # 10-10 토큰 작업 2 — 시험 도구는 토큰 검사 끄기
if ($Map) {
  if (-not (Test-Path -LiteralPath $Map)) { Write-Host "[FAIL] 맵이 없다: $Map"; exit 1 }
  # Start-Process 는 인자를 빈칸으로 잇기만 한다 — 빈칸 든 경로는 따옴표로 싸서 넘긴다.
  $srvArgs += @('--standalone', '--map', "`"$Map`"", '--mode', 'arena')
}
Write-Host ("모드: " + $(if ($Map) { "독립 — $Map" } else { "녹음 기반" }))
$srv = Start-Process -FilePath $exe -ArgumentList $srvArgs -PassThru -WindowStyle Hidden
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
  # ★ 10-12 플랜 B 작업 7a — 맵 이름 칸(본문 7번 · 패킷 10번 · 9바이트). 방 차리기가 정하고 프로세스 몫(`ConfigureNetwork`)이 틀에 넣는다.
  #   독립 모드는 그 맵, 녹음 모드는 기본 덤프(`CAP_arena_0907_ok`)의 서두 맵 — 소개 원본 녹음의 맵(so_druid)이 아니어야 한다
  $expMap = if ($Map) { [IO.Path]::GetFileNameWithoutExtension($Map).ToLower() } else { 'estate' }
  $mapName = [Text.Encoding]::ASCII.GetString($r[10..18]).TrimEnd([char]0)
  Check ($mapName -eq $expMap) "맵 이름이 '$expMap' ('$mapName')"
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
# ★ 준비 — 진짜 클라는 맵을 읽고 `c0 CLIENT_READY` 를 보낸다. 서버는 이걸 받아야 첫 스냅샷과
#   중요 메시지를 싣는다(2026-09-13, 정답지 `GAME3_2.c:5602` → `GAME3_3.c:1966`).
$rdy = @(); foreach ($b in @(0x01,0x06,0xc0)) { $rdy += [byte]($b -bxor $k) }
[void]$udp.Send([byte[]]$rdy, $rdy.Length, $ep)
Start-Sleep -Milliseconds 200

# ── 5. 틱 — 접속이 끝나면 서버가 스스로 매 틱 상태를 뿌린다
#
# 여기서 확인하는 것 = 30Hz 루프가 실제로 돌고, 시각과 상태 갱신이 짝지어 나가는가.
# 클라가 아무것도 안 보내도 와야 한다.
#
# ★ 09-30 부터 한 틱이 한 조각이다 — 시각 `27` → 갱신 `a4` → 나머지 순으로 묶여 온다
#   (`Server.cpp` `FlushBundles`). 맨 앞(`$d[2]`)만 보면 `a4` 를 영영 못 센다(10-06 확인).
#   앞의 시각만 건너뛰고 `a4` 자리를 찾는다. 다른 것이 앞을 막으면 -1 — 못 찾은 쪽으로 틀린다.
function Find-Upd($d) {
  $p = 2
  while ($p -lt $d.Length) {
    if ($d[$p] -eq 0xa4) { return $p }
    if     ($d[$p] -eq 0x27) { $p += 3 }   # TIMESTAMP      op1 + 2
    elseif ($d[$p] -eq 0x28) { $p += 5 }   # FULL_TIMESTAMP op1 + 4
    else { return -1 }
  }
  return -1
}
# 갱신 머리의 자리 = 이 조각을 받는 사람 자신의 자리. 없으면 $null.
# 0xa4 상태 갱신의 머리는 두 꼴이다 (정품과 같게 맞춰 둔 규약):
#   첫 갱신  [op][ff][번호2][종류2][x2][y2]...   ← 전체 번호로 소개
#   그 뒤    [op][01][x2][y2]...                 ← 별칭 한 바이트
# 자리는 `a4` 기준(`$u`) — 앞에 시각이 묶여 온다(위 Find-Upd).
function Get-HeadPos($d) {
  $u = Find-Upd $d
  if ($u -lt 0) { return $null }
  if ($d[$u+1] -eq 0xff) {
    return @{ x = ($d[$u+6] + $d[$u+7] * 256); y = ($d[$u+8] + $d[$u+9] * 256) }
  }
  if ($d[$u+1] -eq 0x01) {
    return @{ x = ($d[$u+2] + $d[$u+3] * 256); y = ($d[$u+4] + $d[$u+5] * 256) }
  }
  return $null
}
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
    if ((Find-Upd $d) -ge 0) { $upd++ }
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
      $hp = Get-HeadPos $d   # 머리 꼴은 Get-HeadPos 주석
      if ($hp) { return $hp }
    } catch { return $null }
  }
  return $null
}
# ★ 먼저 왼클릭(코드 6) 한 번 — 접속 직후는 관전이라 **걸음이 안 먹는다**(정답지 ImplB `Field3680&3`, 2026-09-13).
#   진짜 클라도 첫 클릭이 관전을 푼다. 서버 걷기가 입력마다 옮기던 때는 관전이어도 걸어져 이 줄이 없었다.
$click = @(0x01, 0x05, 0x3f, 4,  6,0,0,0)
$clickX = @(); foreach ($b in $click) { $clickX += [byte]($b -bxor $k) }
[void]$udp.Send([byte[]]$clickX, $clickX.Length, $ep)
Start-Sleep -Milliseconds 100
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
# 둘째가 실리는지 본다.
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
  $r2x = @(); foreach ($b in @(0x01,0x06,0xc0)) { $r2x += [byte]($b -bxor $k2) }   # 준비 — 위 4.5 주석
  [void]$udp2.Send([byte[]]$r2x, $r2x.Length, $ep)
  Start-Sleep -Milliseconds 300
}
# 둘째가 받는 갱신의 머리 = 둘째 자리. 첫째의 갱신 항목 중 **그 자리에 선 것**이 있으면 첫째가 둘째를 본다.
#
# ⚠ 10-06 전에는 항목을 8바이트씩 잘라 셌다. 항목은 `[00][ff 번호2 종류2][x2 y2]` 뒤에 복합 오브젝트
#   덧바이트가 가변으로 붙어(`UpdateStream.h` 짜임) 둘째를 게임에 안 넣어도 「4명」으로 통과했다.
#   그래서 개수가 아니라 둘째의 자리를 찾는다. 항목은 절대 좌표·전체 번호 꼴(`00 ff`)만 찾으므로
#   서버가 별칭·상대 좌표로 싣기 시작하면 못 찾은 쪽(실패)으로 틀린다.
$pos2 = $null
if ($k2 -ne 0) {
  for ($i = 0; $i -lt 25 -and -not $pos2; $i++) {
    try {
      $g = $udp2.Receive([ref]$any)
      $d = @(); foreach ($b in $g) { $d += ($b -bxor $k2) }
      $pos2 = Get-HeadPos $d
    } catch { break }
  }
}
$seen = $null
if ($pos2) {
  for ($i = 0; $i -lt 25 -and -not $seen; $i++) {
    try {
      $g = $udp.Receive([ref]$any)
      $d = @(); foreach ($b in $g) { $d += ($b -bxor $k) }
      $me = Get-HeadPos $d
      if (-not $me) { continue }
      if ($me.x -eq $pos2.x -and $me.y -eq $pos2.y) { continue }   # 겹쳐 서면 누가 누군지 못 가른다
      $u = Find-Upd $d
      for ($q = $u + 2; $q + 9 -lt $d.Length; $q++) {
        if ($d[$q] -ne 0 -or $d[$q+1] -ne 0xff) { continue }
        $ex = $d[$q+6] + $d[$q+7] * 256; $ey = $d[$q+8] + $d[$q+9] * 256
        if ($ex -eq $pos2.x -and $ey -eq $pos2.y) { $seen = @{ id = ($d[$q+2] + $d[$q+3] * 256) }; break }
      }
    } catch { break }
  }
}
Check ($k2 -ne 0) "둘째가 접속했다"
if ($pos2) { Write-Host ("     둘째 자리: ({0},{1})" -f $pos2.x, $pos2.y) }
$seenTxt = if ($seen) { " — 번호 0x{0:x4}" -f $seen.id } else { "" }
Check ($null -ne $seen) "첫째의 상태 갱신에 둘째가 그 자리로 실린다$seenTxt"
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
