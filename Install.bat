@echo off
REM Double-click me. Runs Install.ps1 without changing your PowerShell settings.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*
echo.
pause
