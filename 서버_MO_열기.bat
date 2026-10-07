@echo off
rem Open the MO servers to other PCs: firewall + game server + lobby on 0.0.0.0 (UAC once). Close with the close bat.
rem Optional: -AllowIp a.b.c.d to accept only that address.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\mo_host.ps1" -Mode Open %*
pause
