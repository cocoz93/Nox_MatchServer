@echo off
rem selftest: proxy wsock32.dll must behave like the real one and log the hooked calls
rem   1) export table (ordinal, name) == real SysWOW64 wsock32.dll
rem   2) selftest.exe next to the proxy, logging on  -> behavior + log lines
rem   3) selftest.exe next to the proxy, logging off -> behavior + no log file
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
if not exist wsock32.dll ( echo wsock32.dll missing - run build.bat & popd & exit /b 1 )
cl /nologo /utf-8 /O2 /W3 /EHsc /MT selftest.cpp /Fe:selftest.exe /link wsock32.lib >nul
if errorlevel 1 ( echo selftest build failed & popd & exit /b 1 )
set RC=0
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_exports.ps1"
if errorlevel 1 set RC=1
set "LOGDIR=%TEMP%\wsock32_proxy_test"
if exist "%LOGDIR%" rmdir /s /q "%LOGDIR%"
mkdir "%LOGDIR%"
set "NOX_HOOK_LOG_DIR=%LOGDIR%"
"%~dp0selftest.exe" log "%LOGDIR%"
if errorlevel 1 set RC=1
set "NOX_HOOK_LOG_DIR="
"%~dp0selftest.exe" nolog "%LOGDIR%"
if errorlevel 1 set RC=1
popd
if %RC%==0 ( echo TEST PASS ) else ( echo TEST FAIL )
exit /b %RC%