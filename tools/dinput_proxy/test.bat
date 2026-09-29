@echo off
rem bgtest: background window must drop mouse moves (proxy) - control = real SysWOW64 dinput.dll
rem WARNING: sends real mouse input (small left-right move, returns to place) and one left-button-up
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
cl /nologo /utf-8 /O2 /W3 /EHsc /MT bgtest.cpp /Fe:bgtest.exe /link user32.lib gdi32.lib >nul
if errorlevel 1 ( echo bgtest build failed & popd & exit /b 1 )
set RC=0
"%~dp0bgtest.exe" C:\Windows\SysWOW64\dinput.dll raw
if errorlevel 1 set RC=1
"%~dp0bgtest.exe" "%~dp0dinput.dll" filter
if errorlevel 1 set RC=1
popd
exit /b %RC%
