@echo off
rem Close the MO servers: stop the router + remove the NoxMO firewall rules (UAC once). The lobby and game server windows stay.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\mo_host.ps1" -Mode Close
pause
