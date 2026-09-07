# 녹스를 "자동(주입)" 과 "수동(사람이 직접)" 사이에서 바꾼다.
#
#   auto    ddraw.ini 의 no_dinput_hook=true  → cnc-ddraw 가 DirectInput 에서 손을 뗀다.
#           내 dinput.dll 프록시가 마우스를 쥐고, 파이프로 넣은 입력이 게임에 들어간다.
#           대신 cnc-ddraw 의 마우스 보정(adjmouse)이 빠지므로 사람이 직접 하기엔 어색하다.
#   manual  no_dinput_hook=false → 원래대로. cnc-ddraw 가 마우스를 보정한다.
#           이 상태에서는 주입이 안 먹는다(cnc-ddraw 가 내 값을 자기 값으로 덮는다).
#
# 쓰는 법:  nox_mode.ps1 -Mode auto     /     nox_mode.ps1 -Mode manual     /     nox_mode.ps1
param([ValidateSet('auto','manual','show')][string]$Mode = 'show')
$ini = "C:\Program Files\GOG Galaxy\Games\Nox\ddraw.ini"
$enc = [Text.Encoding]::GetEncoding(949)
$t = $enc.GetString([IO.File]::ReadAllBytes($ini))
$cur = if ($t -match '(?m)^no_dinput_hook=(\w+)') { $Matches[1] } else { '(없음)' }
if ($Mode -eq 'show') {
  $label = if ($cur -eq 'true') { 'auto (주입 됨)' } else { 'manual (사람이 직접)' }
  "지금: no_dinput_hook=$cur  →  $label"
  return
}
$want = if ($Mode -eq 'auto') { 'true' } else { 'false' }
if ($cur -eq $want) { "이미 $Mode 입니다 (no_dinput_hook=$cur)"; return }
$t2 = $t -replace '(?m)^no_dinput_hook=\w+', "no_dinput_hook=$want"
[IO.File]::WriteAllBytes($ini, $enc.GetBytes($t2))
"$cur -> $want  ($Mode). ★ 게임을 다시 띄워야 반영됩니다"