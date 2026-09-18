# imgdiff.ps1 - compare consecutive screenshots and report how much changed.
# Used to judge "does it flicker?" objectively instead of by eye.
#   .\tools\imgdiff.ps1 -Dir <folder> -Prefix e10_plate
param(
  [Parameter(Mandatory=$true)][string]$Dir,
  [Parameter(Mandatory=$true)][string]$Prefix,
  [int]$Step = 4,          # sample every Nth pixel
  [int]$Thresh = 24        # per-channel difference that counts as "changed"
)
Add-Type -AssemblyName System.Drawing

$files = Get-ChildItem $Dir -Filter "$Prefix*.png" | Sort-Object Name
if ($files.Count -lt 2) { Write-Output "need at least 2 files"; exit 1 }

$prev = $null
$prevName = ''
foreach ($f in $files) {
  $bmp = [Drawing.Bitmap]::FromFile($f.FullName)
  if ($prev -ne $null) {
    $changed = 0; $total = 0
    $w = [Math]::Min($bmp.Width, $prev.Width)
    $h = [Math]::Min($bmp.Height, $prev.Height)
    # game area only - skip the HUD strip at the bottom and the window title bar
    for ($y = 40; $y -lt ($h - 180); $y += $Step) {
      for ($x = 0; $x -lt $w; $x += $Step) {
        $a = $prev.GetPixel($x, $y); $b = $bmp.GetPixel($x, $y)
        $total++
        if ([Math]::Abs($a.R - $b.R) -gt $Thresh -or
            [Math]::Abs($a.G - $b.G) -gt $Thresh -or
            [Math]::Abs($a.B - $b.B) -gt $Thresh) { $changed++ }
      }
    }
    $pct = if ($total -gt 0) { [math]::Round(100.0 * $changed / $total, 2) } else { 0 }
    Write-Output ("{0} -> {1} : changed {2}% ({3}/{4})" -f $prevName, $f.Name, $pct, $changed, $total)
    $prev.Dispose()
  }
  $prev = $bmp
  $prevName = $f.Name
}
if ($prev -ne $null) { $prev.Dispose() }
