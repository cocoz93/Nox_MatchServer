@echo off
rem Close the MO servers: stop the lobby + remove the NoxMO firewall rules (UAC once). The game server window stays.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\mo_host.ps1" -Mode Close
pause
