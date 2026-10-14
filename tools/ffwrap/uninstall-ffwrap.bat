@echo off
setlocal
title ffwrap for Jellyfin - uninstall
rem Removes ffwrap (see README.txt).  Asks Windows for administrator rights, runs
rem ffwrap-setup.ps1, and keeps the window open with SUCCESS or FAILED.

if "%FFWRAP_TEST%"=="1" goto run
if /i "%~1"=="elevated" goto run
fltmc >nul 2>&1
if errorlevel 1 goto elevate

:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ffwrap-setup.ps1" uninstall
if errorlevel 9009 echo  FAILED: Windows PowerShell was not found on this PC.
echo.
pause
exit /b

:elevate
echo Asking Windows for administrator rights - click Yes on the prompt.
set "FFWRAP_SELF=%~f0"
powershell -NoProfile -Command "try { Start-Process -FilePath $env:ComSpec -ArgumentList ('/c \"\"' + $env:FFWRAP_SELF + '\" elevated\"') -Verb RunAs -ErrorAction Stop } catch { exit 1 }"
if errorlevel 1 (
  echo.
  echo  FAILED: administrator rights were not given, so nothing was changed.
  echo.
  pause
)
exit /b
