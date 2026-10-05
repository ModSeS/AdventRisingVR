@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launcher.ps1" -Action Check
set "result=%errorlevel%"
echo.
pause
exit /b %result%
