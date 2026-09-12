@echo off
REM Double-click me. Runs Uninstall.ps1 without changing your PowerShell settings.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall.ps1" %*
echo.
pause
