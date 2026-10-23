@echo off
rem Thin wrapper: deploy.bat [-Build] [-GameDir <dir>] [-WaitForExit] [-NoSaveBackup] [-Uninstall [-Purge [-Force]]]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
exit /b %ERRORLEVEL%
