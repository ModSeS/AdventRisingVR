@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Launcher.ps1" -Action Install
set "result=%errorlevel%"
echo.
pause
exit /b %result%
