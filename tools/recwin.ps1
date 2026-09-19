# recwin.ps1 - record a window with ffmpeg gdigrab, then split it into frames.
#
# Why this exists: capwin.ps1 takes ~0.3s per shot, which is too coarse for
# motions that finish inside a second (a sword swing is ~0.6s, an arrow lives
# ~0.3s). Those judgements kept failing because the sampling was slower than
# the thing being sampled. Recording at 30fps gives a 33ms step instead.
#
#   .\tools\recwin.ps1 -Pid_ <pid> -Seconds 6 -Out <folder>
#
# Leaves <folder>\rec.mp4 and <folder>\f_0001.png ... one PNG per frame.
param(
  [Parameter(Mandatory=$true)][int]$Pid_,
  [Parameter(Mandatory=$true)][string]$Out,
  [int]$Seconds = 6,
  [int]$Fps = 30
)
$ErrorActionPreference = 'Stop'

$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue)
if (-not $ff) { Write-Output "FAIL no-ffmpeg"; exit 1 }

$p = Get-Process -Id $Pid_ -ErrorAction SilentlyContinue
if (-not $p -or $p.MainWindowHandle -eq 0) { Write-Output "FAIL no-window"; exit 1 }
$title = $p.MainWindowTitle
if (-not $title) { Write-Output "FAIL no-title"; exit 1 }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$mp4 = Join-Path $Out 'rec.mp4'

# gdigrab takes the window by title. It keeps capturing even when the window
# is not focused, which is the whole point - we must not steal the screen.
& $ff.Source -y -loglevel error -f gdigrab -framerate $Fps -t $Seconds `
    -i "title=$title" -pix_fmt yuv420p $mp4 2>&1 | Out-Null
if (-not (Test-Path $mp4)) { Write-Output "FAIL no-recording"; exit 1 }

& $ff.Source -y -loglevel error -i $mp4 (Join-Path $Out 'f_%04d.png') 2>&1 | Out-Null
$n = (Get-ChildItem $Out -Filter 'f_*.png' -ErrorAction SilentlyContinue).Count
Write-Output ("OK title='{0}' seconds={1} fps={2} frames={3} dir={4}" -f $title, $Seconds, $Fps, $n, $Out)
