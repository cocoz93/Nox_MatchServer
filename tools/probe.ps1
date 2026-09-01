param([string[]]$Targets = @('127.0.0.1'), [int]$Port = 18590, [int]$WaitMs = 3000)
$sock = New-Object System.Net.Sockets.Socket('InterNetwork','Dgram','Udp')
$sock.SetSocketOption('Socket','ReuseAddress',$true)
$sock.EnableBroadcast = $true
$sock.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)))
$sock.ReceiveTimeout = 400
$local = $sock.LocalEndPoint
"probe from $local"
foreach ($t in $Targets) {
  $tick = [System.Environment]::TickCount
  $pkt = New-Object byte[] 12
  $pkt[0] = 0x00; $pkt[1] = 0x00; $pkt[2] = 0x0C
  [Array]::Copy([BitConverter]::GetBytes([int]$tick), 0, $pkt, 8, 4)
  $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($t), $Port)
  [void]$sock.SendTo($pkt, $ep)
  "-> sent 12B to ${t}:$Port  tick=$tick  [$((($pkt | ForEach-Object { $_.ToString('x2') }) -join ' '))]"
}
$buf = New-Object byte[] 8192
$deadline = (Get-Date).AddMilliseconds($WaitMs)
$n = 0
while ((Get-Date) -lt $deadline) {
  $remote = [System.Net.EndPoint](New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0))
  try { $len = $sock.ReceiveFrom($buf, [ref]$remote) } catch { continue }
  $n++
  $b = $buf[0..($len-1)]
  $hex = ($b | ForEach-Object { $_.ToString('x2') }) -join ' '
  $asc = -join ($b | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
  "<= REPLY #$n from $remote  len=$len"
  "   HEX: $hex"
  "   ASC: $asc"
}
$sock.Close()
"total replies = $n"
