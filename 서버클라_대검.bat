@echo off
rem 녹스 - 대검 확인용: 서버클라_실행.bat 를 Estate + 대검 자리 시작으로 부른다
rem 두 클라가 Estate 바닥 대검(GreatSword) 자리에서 시작한다(처음 들어올 때만 - 죽고 살아나면 맵 시작 자리)
call "%~dp0서버클라_실행.bat" Estate --spawn-at-item GreatSword