# ui_apply.ps1 — 게임 목록 화면 여섯째 칸(10007) 글자를 「랭크 매칭」으로 (랭크 매칭 조각 1 · Docs/RANK_MATCH_PLAN.md)
#
#   .\tools\ui_apply.ps1                  → 적용(이미 적용이면 그대로)
#   .\tools\ui_apply.ps1 -Mode Restore    → 되돌림(적용 전 사본과 바이트 대조)
#   .\tools\ui_apply.ps1 -Mode Check      → 상태만(원본 · 적용됨 · 모름)
#   -GameDir <폴더>                        → 다른 게임 폴더(다른 PC 로 옮길 꾸러미 쪽에 넣을 때)
#
# 고치는 것 둘 — 정품 파일이라 리포엔 안 싣고 이 스크립트가 그 자리에서 바꾼다(공개 리포에 게임 파일을 넣지 않게)
#   window\noxworld.wnd  10007(원래 「찾기」) 버튼의 글자 열쇠 Noxworld.wnd:Find → Noxworld.wnd:RankMatch
#   nox.csf              새 열쇠 Noxworld.wnd:RankMatch = 「랭크 매칭」 한 묶음을 끝에 더하고 머리의 열쇠 수 · 글 수를 +1
# 왜 이 꼴인가
#   · 정품은 창 파일 · 글자 파일을 디스크에서 읽는다(10-30 파일럿 — mainmenu.wnd 첫 버튼 열쇠를 바꾸니 화면 글자가 바뀜 · 원본으로 되돌리니 그대로)
#   · 글자 파일 열쇠는 읽은 뒤 정렬하고(Game.exe 0x40f3c1 qsort) 이진 탐색으로 찾는다(0x40f160) → 끝에 더해도 된다(파일 안 차례는 원래도 정렬 안 됨)
#   · 숨김은 안 푼다 — 정품이 웹(WOL) 모드 게임 목록을 만들 때 10007 을 스스로 보이게 한다(0x437948 · 숨김 비트 0x10 끔).
#     창 파일에서 풀면 LAN 모드에도 나와 원래 「사용자 찾기」 창이 뜬다. 우리 로비에서 누름을 랭크 매칭으로 돌리는 것은 DLL(조각 2)
# 되돌리기 — 적용할 때 원본을 _out\ui_backup\<이름>.<SHA256 앞 12자>.orig 로 둔다(같은 원본이면 한 번만 — 날짜 이름이면 두 번째 사본이 첫 사본을 덮는다).
#   되돌림은 거꾸로 고친 뒤 그 사본과 바이트를 대조한다(사본을 그대로 덮지 않는다 — 그 사이 GOG 가 바꾼 다른 칸이 있으면 대조에서 걸린다)
# GOG 업데이트가 두 파일을 덮으면(ddraw.dll 과 같은 처지) 이 스크립트를 다시 돌리면 된다
param(
  [ValidateSet('Apply', 'Restore', 'Check')][string]$Mode = 'Apply',
  [string]$GameDir = 'C:\Program Files\GOG Galaxy\Games\Nox')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$backupDir = Join-Path $repo '_out\ui_backup'
$latin1 = [Text.Encoding]::GetEncoding(28591)   # 바이트 그대로 글자로(창 파일은 ASCII)

$wndPath = Join-Path $GameDir 'window\noxworld.wnd'
$csfPath = Join-Path $GameDir 'nox.csf'
$blockHead = '10007 40 349 150 32 PUSHBUTTON (Find);'
$textOrig = 'TEXT = Noxworld.wnd:Find;'
$textNew = 'TEXT = Noxworld.wnd:RankMatch;'
$label = 'Noxworld.wnd:RankMatch'
$labelText = '랭크 매칭'

function Count-Of([string]$hay, [string]$needle) { ([regex]::Matches($hay, [regex]::Escape($needle))).Count }

# 창 파일 — 10007 칸 안의 글자 줄이 원래 열쇠면 원본 · 새 열쇠면 적용됨. 파일 전체에서 각 줄이 몇 번인지도 본다(다른 칸에 같은 줄이 생기면 모름)
function Wnd-State([byte[]]$b) {
  $t = $latin1.GetString($b)
  $h = $t.IndexOf($blockHead, [StringComparison]::Ordinal)
  if ($h -lt 0 -or (Count-Of $t $blockHead) -ne 1) { return @{ state = '모름'; why = '10007 칸 머리 줄이 하나가 아님' } }
  $end = $t.IndexOf('END', $h, [StringComparison]::Ordinal)
  $nOrig = Count-Of $t $textOrig; $nNew = Count-Of $t $textNew
  $inOrig = $t.IndexOf($textOrig, $h, [StringComparison]::Ordinal); $inNew = $t.IndexOf($textNew, $h, [StringComparison]::Ordinal)
  if ($nOrig -eq 1 -and $nNew -eq 0 -and $inOrig -gt $h -and $inOrig -lt $end) { return @{ state = '원본'; text = $t } }
  if ($nOrig -eq 0 -and $nNew -eq 1 -and $inNew -gt $h -and $inNew -lt $end) { return @{ state = '적용됨'; text = $t } }
  return @{ state = '모름'; why = "원래 열쇠 줄 $nOrig · 새 열쇠 줄 $nNew(10007 칸 안이어야 함)" }
}

# 글자 파일 — 머리 24바이트(' FSC' · 판 · 열쇠 수 · 글 수 · ? · 언어) 뒤 열쇠 묶음(' LBL' · 글 수 · 이름 길이 · 이름 · 글 묶음들)을 끝까지 걷는다.
#   글 묶음 = ' rtS'(또는 'WrtS' — 뒤에 소리 이름이 하나 더) · 글자 수 · 비트 뒤집은 UTF-16. 끝에서 정확히 멈춰야 읽은 것
function Label-Bytes() {
  $name = [Text.Encoding]::ASCII.GetBytes($label)
  $u = [Text.Encoding]::Unicode.GetBytes($labelText)
  $inv = [byte[]]($u | ForEach-Object { (-bnot $_) -band 0xFF })
  $ms = New-Object IO.MemoryStream
  $w = New-Object IO.BinaryWriter($ms)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' LBL')); $w.Write([uint32]1); $w.Write([uint32]$name.Length); $w.Write($name)
  $w.Write([Text.Encoding]::ASCII.GetBytes(' rtS')); $w.Write([uint32]($u.Length / 2)); $w.Write($inv)
  $w.Flush()
  return ,$ms.ToArray()
}

function Csf-State([byte[]]$b) {
  if ($b.Length -lt 24 -or $latin1.GetString($b, 0, 4) -ne ' FSC') { return @{ state = '모름'; why = '머리가 FSC 가 아님' } }
  $nLabels = [BitConverter]::ToUInt32($b, 8); $nStrings = [BitConverter]::ToUInt32($b, 12)
  $pos = 24; $seenL = 0; $seenS = 0; $mine = -1; $mineAt = -1
  while ($pos -lt $b.Length) {
    if ($latin1.GetString($b, $pos, 4) -ne ' LBL') { return @{ state = '모름'; why = "열쇠 머리가 아닌 자리 $pos(열쇠 $seenL 개 읽음)" } }
    $cnt = [BitConverter]::ToUInt32($b, $pos + 4); $nlen = [BitConverter]::ToUInt32($b, $pos + 8)
    $name = $latin1.GetString($b, $pos + 12, $nlen)
    if ($name -ieq $label) { $mine++; $mineAt = $pos }
    $pos += 12 + $nlen; $seenL++
    for ($i = 0; $i -lt $cnt; $i++) {
      $tag = $latin1.GetString($b, $pos, 4).ToUpper()
      $slen = [BitConverter]::ToUInt32($b, $pos + 4)
      $pos += 8 + 2 * $slen; $seenS++
      if ($tag -eq 'WRTS') { $pos += 4 + [BitConverter]::ToUInt32($b, $pos) }
    }
  }
  if ($pos -ne $b.Length) { return @{ state = '모름'; why = "끝을 넘음($pos / $($b.Length))" } }
  if ($seenL -ne $nLabels -or $seenS -ne $nStrings) { return @{ state = '모름'; why = "머리 수(열쇠 $nLabels · 글 $nStrings)와 읽은 수(열쇠 $seenL · 글 $seenS)가 다름" } }
  if ($mine -lt 0) { return @{ state = '원본'; labels = $seenL; strings = $seenS } }
  $lb = Label-Bytes
  $tailOk = $mine -eq 0 -and $mineAt -eq $b.Length - $lb.Length
  for ($i = 0; $tailOk -and $i -lt $lb.Length; $i++) { if ($b[$mineAt + $i] -ne $lb[$i]) { $tailOk = $false } }
  if ($tailOk) { return @{ state = '적용됨'; labels = $seenL; strings = $seenS } }
  return @{ state = '모름'; why = "$label 이 맨 끝의 이 스크립트 꼴이 아님(되돌림 못 함)" }
}

function Set-Counts([byte[]]$b, [int]$delta) {
  foreach ($off in 8, 12) {
    $v = [BitConverter]::GetBytes([uint32]([BitConverter]::ToUInt32($b, $off) + $delta))
    [Array]::Copy($v, 0, $b, $off, 4)
  }
}

function Sha([byte[]]$b) {
  $s = [Security.Cryptography.SHA256]::Create()
  return ([BitConverter]::ToString($s.ComputeHash($b))).Replace('-', '')
}

# 원본 사본 — 같은 바이트면 다시 안 씀(SHA256 앞 12자가 이름에 듦)
function Save-Backup([string]$name, [byte[]]$b) {
  New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
  $p = Join-Path $backupDir ("{0}.{1}.orig" -f $name, (Sha $b).Substring(0, 12))
  if (-not (Test-Path -LiteralPath $p)) { [IO.File]::WriteAllBytes($p, $b) }
  return $p
}

function Find-Backup([string]$name, [byte[]]$restored) {
  $p = Join-Path $backupDir ("{0}.{1}.orig" -f $name, (Sha $restored).Substring(0, 12))
  if (-not (Test-Path -LiteralPath $p)) { return $null }
  $o = [IO.File]::ReadAllBytes($p)
  if ($o.Length -ne $restored.Length) { return $null }
  for ($i = 0; $i -lt $o.Length; $i++) { if ($o[$i] -ne $restored[$i]) { return $null } }
  return $p
}

foreach ($p in $wndPath, $csfPath) { if (-not (Test-Path -LiteralPath $p)) { throw "없는 파일: $p" } }
$wndB = [IO.File]::ReadAllBytes($wndPath)
$csfB = [IO.File]::ReadAllBytes($csfPath)
$ws = Wnd-State $wndB
$cs = Csf-State $csfB
Write-Host ("[{0}] noxworld.wnd = {1}{2} · nox.csf = {3}{4}" -f $Mode, $ws.state, $(if ($ws.why) { " ($($ws.why))" }), $cs.state,
  $(if ($cs.why) { " ($($cs.why))" } else { " (열쇠 $($cs.labels) · 글 $($cs.strings))" }))
if ($Mode -eq 'Check') {
  if ($ws.state -eq '적용됨' -and $cs.state -eq '적용됨') { exit 0 }
  exit 1
}
if ($ws.state -eq '모름' -or $cs.state -eq '모름') { throw '모르는 상태 — 손대지 않음' }

# 두 파일의 새 바이트를 다 지은 뒤에 쓴다 — 하나만 바뀐 채 멈추지 않게(10-30 시험: 글자 파일 사본이 없을 때 창 파일만 되돌아갔다)
$wndOut = $null; $csfOut = $null; $wndNote = ''; $csfNote = ''
if ($Mode -eq 'Apply') {
  if ($ws.state -eq '원본') {
    $t = $ws.text
    $h = $t.IndexOf($blockHead, [StringComparison]::Ordinal); $i = $t.IndexOf($textOrig, $h, [StringComparison]::Ordinal)
    $wndOut = $latin1.GetBytes($t.Substring(0, $i) + $textNew + $t.Substring($i + $textOrig.Length))
  }
  if ($cs.state -eq '원본') {
    $lb = Label-Bytes
    $csfOut = New-Object byte[] ($csfB.Length + $lb.Length)
    [Array]::Copy($csfB, $csfOut, $csfB.Length)
    [Array]::Copy($lb, 0, $csfOut, $csfB.Length, $lb.Length)
    Set-Counts $csfOut 1
  }
  if ($wndOut) { $wndNote = "적용 — 원본 사본 $(Save-Backup 'noxworld.wnd' $wndB)" }
  if ($csfOut) { $csfNote = "적용 — 원본 사본 $(Save-Backup 'nox.csf' $csfB) (+$($csfOut.Length - $csfB.Length) 바이트)" }
} else {
  if ($ws.state -eq '적용됨') {
    $t = $ws.text
    $h = $t.IndexOf($blockHead, [StringComparison]::Ordinal); $i = $t.IndexOf($textNew, $h, [StringComparison]::Ordinal)
    $wndOut = $latin1.GetBytes($t.Substring(0, $i) + $textOrig + $t.Substring($i + $textNew.Length))
    $bk = Find-Backup 'noxworld.wnd' $wndOut
    if (-not $bk) { throw 'noxworld.wnd — 거꾸로 고친 것과 같은 원본 사본이 없다(두 파일 다 손대지 않음)' }
    $wndNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
  if ($cs.state -eq '적용됨') {
    $lb = Label-Bytes
    $csfOut = New-Object byte[] ($csfB.Length - $lb.Length)
    [Array]::Copy($csfB, $csfOut, $csfOut.Length)
    Set-Counts $csfOut -1
    $bk = Find-Backup 'nox.csf' $csfOut
    if (-not $bk) { throw 'nox.csf — 거꾸로 고친 것과 같은 원본 사본이 없다(두 파일 다 손대지 않음)' }
    $csfNote = "되돌림 — 사본과 바이트 같음 $bk"
  }
}
if ($wndOut) { [IO.File]::WriteAllBytes($wndPath, $wndOut); Write-Host "  noxworld.wnd $wndNote" }
if ($csfOut) { [IO.File]::WriteAllBytes($csfPath, $csfOut); Write-Host "  nox.csf $csfNote" }
$ws2 = Wnd-State ([IO.File]::ReadAllBytes($wndPath))
$cs2 = Csf-State ([IO.File]::ReadAllBytes($csfPath))
Write-Host ("  지금 noxworld.wnd = {0} · nox.csf = {1}" -f $ws2.state, $cs2.state)
$want = if ($Mode -eq 'Apply') { '적용됨' } else { '원본' }
if ($ws2.state -ne $want -or $cs2.state -ne $want) { throw "끝 상태가 $want 이 아님" }
Write-Host "[OK] $Mode"
