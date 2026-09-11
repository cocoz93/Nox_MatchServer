# class_check.ps1 — **서버가 직업을 제대로 읽는가**만 빠르게 본다.
#
# 왜 (2026-09-11) — 캐릭터 목록 캡처에는 `Jack the Conjurer` 가 골라져 있는데
#   서버 `[자리]` 줄은 `체력150/150 마나0/0`(워리어 값)이었다. 그래서 주문이
#   `cost > s.mana` 에서 전부 조용히 막혔다 — 「마나칸 0」과 「주문이 안 나간다」의 뿌리다.
#
#   .\tools\class_check.ps1 -Row 1
param(
  [int]$Row = 1,
  [string]$Map = 'C:\Program Files\GOG Galaxy\Games\Nox\maps\estate\Estate.map'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$slog = Join-Path $root '_evidence\srv_cls.log'

Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (Test-Path $slog) { [IO.File]::Delete($slog) }

$exe = Join-Path $root 'build\Release\nox_server.exe'
$cmd = 'cmd /c ""' + $exe + '" --port 18590 --standalone --map "' + $Map + '" --mode arena --quiet > "' + $slog + '" 2>&1"'
Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $cmd) -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 3

& (Join-Path $PSScriptRoot 'join_my_server.ps1') -Exe 'Game3.exe' -Tag ("cls{0}" -f $Row) -CharRow $Row |
  Select-String -Pattern '픽 확인' | ForEach-Object { Write-Host ("  " + $_.Line.Trim()) }
Start-Sleep -Seconds 8

Write-Host ''
Write-Host '=== 서버가 본 것 ==='
$f = New-Object IO.FileStream($slog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try { $t = (New-Object IO.StreamReader($f, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $f.Dispose() }
($t -split "`r?`n") | Where-Object { $_ -match '\[직업\]' } | Select-Object -First 2 |
  ForEach-Object { Write-Host ("  " + $_) }
($t -split "`r?`n") | Where-Object { $_ -match '체력\d+/\d+ 마나\d+/\d+' } | Select-Object -First 1 |
  ForEach-Object { if ($_ -match '(체력\d+/\d+ 마나\d+/\d+)') { Write-Host ("  " + $Matches[1]) } }

Get-Process nox_server, nox_proxy, deskhost -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name Game, Game2, Game3, Game4, Game5 -ErrorAction SilentlyContinue |
  Stop-Process -Force -ErrorAction SilentlyContinue
