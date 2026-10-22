@echo off
rem Thin wrapper: build.bat [-Config Debug] [-Clean] [-Regen] [-NoVerify]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
