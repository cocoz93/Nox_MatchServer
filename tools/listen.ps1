$ErrorActionPreference = 'Continue'
$sock = New-Object System.Net.Sockets.Socket('InterNetwork','Dgram','Udp')
$sock.SetSocketOption('Socket','ReuseAddress',$true)
$sock.EnableBroadcast = $true
$sock.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 18590)))
$sock.ReceiveTimeout = 500
$buf = New-Object byte[] 8192
$end = (Get-Date).AddSeconds(120)
"LISTENING on 0.0.0.0:18590 until $($end.ToString('HH:mm:ss'))"
$count = 0
while ((Get-Date) -lt $end) {
  $remote = [System.Net.EndPoint](New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any,0))
  try { $n = $sock.ReceiveFrom($buf, [ref]$remote) } catch { continue }
  $count++
  $b = $buf[0..($n-1)]
  $hex = ($b | ForEach-Object { $_.ToString('x2') }) -join ' '
  $asc = -join ($b | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
  "--- PKT #$count  $(Get-Date -Format HH:mm:ss.fff)  from=$remote  len=$n"
  "HEX: $hex"
  "ASC: $asc"
}
$sock.Close()
"DONE. total packets = $count"
