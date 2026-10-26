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
rem line filter fuzz (same header as the DLL): every TCP split x every recv size
cl /nologo /utf-8 /O2 /W3 /EHsc /MT linefilter_test.cpp /Fe:linefilter_test.exe >nul
if errorlevel 1 ( echo linefilter_test build failed & popd & exit /b 1 )
"%~dp0linefilter_test.exe"
if errorlevel 1 set RC=1
rem host game entry/channel builder vs the bytes dumped from the genuine client (10-16 pilot)
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /I ..\..\server\include hostgame_test.cpp ..\..\server\src\lobby\WolGameInfo.cpp /Fe:hostgame_test.exe >nul
if errorlevel 1 ( echo hostgame_test build failed & popd & exit /b 1 )
"%~dp0hostgame_test.exe"
if errorlevel 1 set RC=1
rem login line rewriter (same header as the DLL): every TCP split x every send acceptance pattern (10-26 login auth piece 5)
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /I ..\..\server\include login_rewrite_test.cpp /Fe:login_rewrite_test.exe >nul
if errorlevel 1 ( echo login_rewrite_test build failed & popd & exit /b 1 )
"%~dp0login_rewrite_test.exe"
if errorlevel 1 set RC=1
rem login server client (WinHTTP) vs a fake HTTP server: request encoding, reply classes, unreachable cases, total deadline (10-26 login auth piece 5)
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /I ..\..\server\include auth_client_test.cpp /Fe:auth_client_test.exe >nul
if errorlevel 1 ( echo auth_client_test build failed & popd & exit /b 1 )
"%~dp0auth_client_test.exe"
if errorlevel 1 set RC=1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_exports.ps1"
if errorlevel 1 set RC=1
set "LOGDIR=%TEMP%\wsock32_proxy_test"
if exist "%LOGDIR%" rmdir /s /q "%LOGDIR%"
mkdir "%LOGDIR%"
set "NOX_MYROOM_LINE="
set "NOX_NO_HOSTGAME="
set "NOX_HOOK_LOG_DIR=%LOGDIR%"
"%~dp0selftest.exe" log "%LOGDIR%"
if errorlevel 1 set RC=1
set "NOX_HOOK_LOG_DIR="
"%~dp0selftest.exe" nolog "%LOGDIR%"
if errorlevel 1 set RC=1
rem 4) my-room line forced on (test switch - this exe is not Game.exe, so only the switch turns it on)
set "NOX_MYROOM_LINE=1"
"%~dp0selftest.exe" nolog "%LOGDIR%"
if errorlevel 1 set RC=1
set "NOX_MYROOM_LINE="
popd
if %RC%==0 ( echo TEST PASS ) else ( echo TEST FAIL )
exit /b %RC%