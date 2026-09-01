# build.ps1 — Release 로 빌드하고 자체검사까지 돌린다.
#   .\tools\build.ps1              Release 빌드 + 테스트
#   .\tools\build.ps1 -Config Debug
#   .\tools\build.ps1 -Clean       build/ 를 지우고 처음부터
param([string]$Config = 'Release', [switch]$Clean)
$ErrorActionPreference = 'Continue'

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build'

# CMake 찾기 — PATH 에 없으면 Visual Studio 에 딸려온 것을 쓴다
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
  $vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
  $cand = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if (Test-Path $cand) { $cmake = $cand }
}
if (-not $cmake) { Write-Host "[FAIL] cmake 를 찾지 못했다"; exit 1 }
Write-Host "cmake: $cmake"

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }

& $cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64 | Out-String | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] 설정 단계 실패"; exit 1 }

& $cmake --build $build --config $Config | Out-String | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] 빌드 실패"; exit 1 }

$exe = Join-Path $build "$Config\nox_server.exe"
if (-not (Test-Path $exe)) { Write-Host "[FAIL] 산출물 없음: $exe"; exit 1 }
$fi = Get-Item $exe
Write-Host ""
Write-Host ("산출물: {0}  {1:N0} 바이트  {2}" -f $exe, $fi.Length, $fi.LastWriteTime)

Write-Host ""
Write-Host "=== 자체검사 ==="
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $ctest --test-dir $build -C $Config --output-on-failure | Out-String | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] 테스트 실패"; exit 1 }
Write-Host "[OK] 빌드·테스트 통과 ($Config)"
