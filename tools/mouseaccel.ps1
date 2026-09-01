param([switch]$Off, [switch]$Restore)
$sig = @'
[DllImport("user32.dll", SetLastError=true)] public static extern bool SystemParametersInfo(uint a, uint b, int[] pv, uint f);
'@
Add-Type -MemberDefinition $sig -Name SPI -Namespace MS
$save = "$env:TEMP\nox_mouse_accel_backup.txt"
$cur = New-Object int[] 3
[void][MS.SPI]::SystemParametersInfo(0x0003, 0, $cur, 0)   # SPI_GETMOUSE
"현재 마우스 설정: $($cur -join ',')"
if ($Off) {
  if (-not (Test-Path $save)) { ($cur -join ',') | Set-Content $save -Encoding ascii; "원래 값 저장: $save" }
  $new = [int[]](0,0,0)
  [void][MS.SPI]::SystemParametersInfo(0x0004, 0, $new, 0)  # SPI_SETMOUSE
  $chk = New-Object int[] 3
  [void][MS.SPI]::SystemParametersInfo(0x0003, 0, $chk, 0)
  "가속 끔 -> $($chk -join ',')"
}
if ($Restore) {
  if (Test-Path $save) {
    $old = [int[]]((Get-Content $save) -split ',')
    [void][MS.SPI]::SystemParametersInfo(0x0004, 0, $old, 0)
    $chk = New-Object int[] 3
    [void][MS.SPI]::SystemParametersInfo(0x0003, 0, $chk, 0)
    "복원 -> $($chk -join ',')"
    Remove-Item $save -Force
  } else { "저장된 백업 없음" }
}
