@echo off
rem 녹스 - 서버클라_실행.bat 과 같되 **중계기(nox_proxy)를 끼워 와이어를 녹음**한다(09-30 - 정품 클라가 로비로 튕기는 판의 원인 가르기용)
rem   정품 클라 -> 중계기 18590(녹음) -> 우리 서버 18591 · 녹음 = _evidence\_runs\사람판_<시각>.cap.txt
rem   끝나면 서버 창과 중계기 창을 닫는다
rem   서버클라_실행.bat            Estate 아레나
rem   서버클라_실행.bat G_Lava     다른 맵 (게임 폴더 maps 아래 폴더 이름)
rem   서버클라_실행.bat Estate --spawn-at-item GreatSword   맵 뒤의 인자는 서버 옵션으로 그대로 넘긴다
rem 서버를 끄려면 서버 창을 닫는다 · 중계기 창도 닫는다
rem 전 판의 서버·정품 클라가 떠 있으면 먼저 닫고 새로 띄운다
rem Game2.exe 는 다중 실행 막이를 푼 정품 사본이다(원본 Game.exe 는 그대로)
setlocal
set "ROOT=%~dp0"
set "GAME=C:\Program Files\GOG Galaxy\Games\Nox"
set "SERVER=%ROOT%build\Release\nox_server.exe"
set "MAP=%~1"
if "%MAP%"=="" set "MAP=Estate"
set "EXTRA="
:nextarg
shift
if "%~1"=="" goto argsdone
set "EXTRA=%EXTRA% %1"
goto nextarg
:argsdone
set "MAPFILE=%GAME%\maps\%MAP%\%MAP%.map"

if not exist "%SERVER%" (
  echo [멈춤] 서버 실행 파일이 없습니다 - 먼저 빌드: tools\build.ps1
  pause
  exit /b 1
)
if not exist "%GAME%\Game2.exe" (
  echo [멈춤] Game2.exe 가 없습니다 - 둘째 정품 클라용 사본
  pause
  exit /b 1
)
if not exist "%MAPFILE%" (
  echo [멈춤] 맵이 없습니다: %MAPFILE%
  pause
  exit /b 1
)

rem 전 판 닫기 - 클라는 창 닫기 신호 먼저, 10초 안에 안 꺼지면 강제로
call :closeapp Game.exe
call :closeapp Game2.exe
rem 서버는 콘솔 프로그램이라 창 닫기 신호를 못 받는다. 저장할 상태도 없어 바로 끈다
tasklist /fi "imagename eq nox_server.exe" | find /i "nox_server.exe" >nul
if not errorlevel 1 (
  echo  전 판 서버 끄는 중
  taskkill /f /im nox_server.exe >nul 2>&1
  timeout /t 1 /nobreak >nul
)

tasklist /fi "imagename eq nox_proxy.exe" | find /i "nox_proxy.exe" >nul
if not errorlevel 1 (
  echo  전 판 중계기 끄는 중
  taskkill /f /im nox_proxy.exe >nul 2>&1
  timeout /t 1 /nobreak >nul
)

rem OpenNox 가 nox.cfg 를 자기 꼴[Version 65540]로 덮어쓰면 정품이 캐릭터 로드에서 꺼진다
findstr /b /c:"Version = 65538" "%GAME%\nox.cfg" >nul
if errorlevel 1 (
  echo [멈춤] nox.cfg 가 정품 꼴[Version = 65538]이 아닙니다 - OpenNox 가 덮어쓴 것
  echo        게임 폴더의 nox.cfg.keep 를 nox.cfg 로 되돌린 뒤 다시 실행하세요
  pause
  exit /b 1
)

rem --aoi(걸어가면 바닥 물건 알림) --friends(내 소환수 파랑) - 10-01 사람 판 3 에서 정품으로 확인해 기본으로 붙임
start "Nox 서버 - %MAP%" "%SERVER%" --port 18591 --standalone --map "%MAPFILE%" --mode arena --quiet --aoi --friends%EXTRA%
timeout /t 3 /nobreak >nul
tasklist /fi "imagename eq nox_server.exe" | find /i "nox_server.exe" >nul
if errorlevel 1 (
  echo [멈춤] 서버가 바로 꺼졌습니다 - 서버 창의 글을 보세요
  pause
  exit /b 1
)

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%t"
if not exist "%ROOT%_evidence\_runs" mkdir "%ROOT%_evidence\_runs"
set "CAP=%ROOT%_evidence\_runs\사람판_%TS%.cap.txt"
start "Nox 중계기 - 녹음" "%ROOT%build\Release\nox_proxy.exe" --listen 18590 --host-port 18591 --dump "%CAP%" --quiet
timeout /t 1 /nobreak >nul

start "" /d "%GAME%" "%GAME%\Game.exe"
timeout /t 3 /nobreak >nul
start "" /d "%GAME%" "%GAME%\Game2.exe"

echo.
echo  서버: %MAP% 아레나, UDP 18591 - 중계기 18590 이 녹음 - 끄려면 서버 창과 중계기 창을 닫을 것
echo  녹음: %CAP%
if defined EXTRA echo  서버 옵션:%EXTRA%
echo  클라 둘: 각 창에서 Multiplay - Network[LAN] - 목록에서 MY_SERVER
echo  들어가면 왼클릭 한 번 - 관전 상태 풀기
echo  조작: 오른버튼 걷기 / 왼버튼 때리기 / A S D F G 칸 / B 주문책
echo.
pause
exit /b 0

:closeapp
tasklist /fi "imagename eq %~1" | find /i "%~1" >nul
if errorlevel 1 exit /b 0
echo  전 판 %~1 닫는 중
taskkill /im %~1 >nul 2>&1
for /l %%i in (1,1,10) do (
  timeout /t 1 /nobreak >nul
  tasklist /fi "imagename eq %~1" | find /i "%~1" >nul
  if errorlevel 1 exit /b 0
)
echo  %~1 가 10초 안에 안 꺼져 강제로 끔
taskkill /f /im %~1 >nul 2>&1
timeout /t 1 /nobreak >nul
exit /b 0