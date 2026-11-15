@echo off
setlocal
if not exist "%~dp0scripts\install.ps1" (
  echo WatchKit for VLC setup cannot find its files.
  echo Right-click the release ZIP, select Extract All, then run this file from the extracted folder.
  echo Download: https://github.com/smilekritik/vlc-watchkit/releases/latest
  pause
  exit /b 1
)
title WatchKit for VLC Setup
set "PSModulePath="
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" %*
set "install_result=%ERRORLEVEL%"
echo.
if "%install_result%"=="0" (
  echo Open VLC, then choose View ^> WatchKit Settings.
  echo To share playback on Discord, save your Discord Application ID there.
) else (
  echo Setup failed. Follow the instructions shown by the installer and run this file again.
)
pause
exit /b %install_result%
