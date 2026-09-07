# 프록시 DLL 에 명령을 보낸다.  쓰는 법:  di.ps1 -Target <pid> -Cmd "inject 1","m 100 50","mb 0 1","mb 0 0"
param([Parameter(Mandatory=$true)][int]$Target, [Parameter(Mandatory=$true)][string[]]$Cmd, [int]$GapMs = 12)
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "noxdi_$Target", [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(4000) } catch { "연결 실패: $($_.Exception.Message)"; exit 1 }
$sw = New-Object System.IO.StreamWriter($pipe); $sw.AutoFlush = $true
$sr = New-Object System.IO.StreamReader($pipe)
foreach ($c in $Cmd) {
  if ($c -match '^sleep\s+(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); "  (쉼 $($Matches[1])ms)"; continue }
  $sw.WriteLine($c)
  $r = $sr.ReadLine()
  "$c  ->  $r"
  if ($GapMs -gt 0) { Start-Sleep -Milliseconds $GapMs }
}
$pipe.Dispose()