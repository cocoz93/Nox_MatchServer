@echo off
rem x86 build - Game.exe is 32-bit
rem import lib gets another name so selftest links the SDK wsock32.lib, not this proxy
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
rem WolGameInfo.cpp = room info packing shared with the server (my-room list line, NOX_MYROOM_LINE)
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /LD /MT /I ..\..\server\include wsock32_proxy.cpp ..\..\server\src\lobby\WolGameInfo.cpp /Fe:wsock32.dll /link /DEF:wsock32.def /IMPLIB:wsock32_proxy.lib
set RC=%errorlevel%
popd
exit /b %RC%