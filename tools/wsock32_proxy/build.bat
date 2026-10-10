@echo off
rem x86 build - Game.exe is 32-bit
rem import lib gets another name so selftest links the SDK wsock32.lib, not this proxy
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
cl /nologo /utf-8 /O2 /W3 /EHsc /LD /MT wsock32_proxy.cpp /Fe:wsock32.dll /link /DEF:wsock32.def /IMPLIB:wsock32_proxy.lib
set RC=%errorlevel%
popd
exit /b %RC%