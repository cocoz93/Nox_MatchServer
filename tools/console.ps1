# console.ps1 — 정품 호스트의 telnet 콘솔에 붙어 명령을 보낸다.
#   .\tools\console.ps1 -Cmds 'show game','load bludeath'
# ⚠ 함수 이름을 rd/wr 로 지으면 Remove-Item 별칭과 부딪힌다.
param([string]$Server = '127.0.0.1', [int]$Port = 7777,
      [string]$Pass = 'CHANGE_ME', [string[]]$Cmds = @(), [int]$Wait = 900)
$c = New-Object System.Net.Sockets.TcpClient
$c.Connect($Server, $Port)
$s = $c.GetStream()
$enc = [System.Text.Encoding]::ASCII
function ReadOut {
  Start-Sleep -Milliseconds $Wait
  $out = ''
  while ($s.DataAvailable) {
    $b = New-Object byte[] 4096
    $n = $s.Read($b, 0, $b.Length)
    if ($n -le 0) { break }
    $out += $enc.GetString($b, 0, $n)
    Start-Sleep -Milliseconds 120
  }
  return $out
}
function WriteLn($t) {
  $b = $enc.GetBytes($t + "`r`n")
  $s.Write($b, 0, $b.Length); $s.Flush()
}
Write-Host "--- 접속 ---"; ReadOut | Write-Host
WriteLn $Pass
Write-Host "--- 인증 ---"; ReadOut | Write-Host
foreach ($cmd in $Cmds) {
  Write-Host "--- > $cmd ---"
  WriteLn $cmd
  ReadOut | Write-Host
}
$c.Close()