@echo off
rem Backward-compatible wrapper. Prefer windows_nuitka.bat.
call "%~dp0windows_nuitka.bat" onefile
exit /b %errorlevel%
