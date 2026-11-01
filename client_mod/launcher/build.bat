@echo off
rem NoxLauncher.exe build (x86 - same toolchain as the client DLL). Output: out\NoxLauncher.exe + out\launcher_ui\
rem WebView2 SDK (headers + static loader) is fetched into third_party\webview2 (git-ignored, not in the repo)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
set "WV2=..\..\third_party\webview2\pkg\build\native"
if not exist "%WV2%\include\WebView2.h" (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='..\..\third_party\webview2'; New-Item -ItemType Directory -Force $d | Out-Null; $p=Join-Path $d 'wv2.nupkg'; Invoke-WebRequest -UseBasicParsing -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40' -OutFile $p; Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::ExtractToDirectory($p, (Join-Path $d 'pkg'))"
)
if not exist "%WV2%\include\WebView2.h" ( echo WebView2 SDK missing & popd & exit /b 1 )
if not exist out mkdir out
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /I ..\..\server\include /I "%WV2%\include" launcher_main.cpp /Fe:out\NoxLauncher.exe /Fo:out\ /link /SUBSYSTEM:WINDOWS /LIBPATH:"%WV2%\x86"
set RC=%errorlevel%
if %RC%==0 robocopy ui out\launcher_ui /MIR /NJH /NJS /NFL /NDL >nul
if %RC%==0 if not exist out\launcher_ui\index.html set RC=1
popd
exit /b %RC%
