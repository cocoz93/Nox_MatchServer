@echo off
rem launcher_test: LauncherCore.h pieces without a window (config, state file, game env block, game path)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
pushd "%~dp0"
if not exist out mkdir out
cl /nologo /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /I ..\..\server\include launcher_test.cpp /Fe:out\launcher_test.exe /Fo:out\ >nul
if errorlevel 1 ( echo launcher_test build failed & popd & exit /b 1 )
out\launcher_test.exe
set RC=%errorlevel%
popd
exit /b %RC%
