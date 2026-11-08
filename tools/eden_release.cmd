@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0eden_release.ps1" %*
echo.
pause
