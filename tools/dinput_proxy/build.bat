@echo off
rem x86 build - Game.exe is 32-bit
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
cl /nologo /utf-8 /O2 /W3 /EHsc /LD /MT dinput_proxy.cpp /Fe:dinput.dll /link /DEF:dinput.def user32.lib ole32.lib
set RC=%errorlevel%
popd
exit /b %RC%