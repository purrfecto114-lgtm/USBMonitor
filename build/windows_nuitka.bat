@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.." || exit /b 1

set "APP_NAME=USBMonitor"
set "APP_VERSION=1.0"
set "ENTRY=USBMonitor.pyw"
set "PYTHON=python"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=onefile"

if /I not "%MODE%"=="onefile" if /I not "%MODE%"=="standalone" (
  echo [ERROR] Usage: build\windows_nuitka.bat [onefile^|standalone]
  exit /b 2
)

if not exist "%ENTRY%" (
  echo [ERROR] Missing entry point: %ENTRY%
  exit /b 3
)

%PYTHON% -c "import sys; assert sys.version_info[:2] >= (3,11); import PySide6, win32api, nuitka" >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Build environment is incomplete.
  echo Run: python -m pip install -r requirements-build.txt
  exit /b 4
)

if not exist "dist" mkdir "dist"
if not exist "build\nuitka" mkdir "build\nuitka"

set "MODE_ARGS=--onefile"
if /I "%MODE%"=="standalone" set "MODE_ARGS=--standalone"

rem Nuitka 4.x uses built-in zstandard compression for onefile mode.
rem The bundled upx.exe can be used as a post-processing step if desired:
rem   upx\upx.exe --best dist\%APP_NAME%.exe

set "CONSOLE_MODE=disable"
if /I "%USBMONITOR_CONSOLE%"=="1" set "CONSOLE_MODE=force"

set "EXTRA_ARGS=%NUITKA_EXTRA_ARGS%"

%PYTHON% -m nuitka ^
  %MODE_ARGS% ^
  --assume-yes-for-downloads ^
  --enable-plugin=pyside6 ^
  --windows-console-mode=%CONSOLE_MODE% ^
  --windows-company-name="USB Monitor" ^
  --windows-product-name="USB Monitor" ^
  --windows-file-description="USB storage tray monitor" ^
  --windows-file-version=%APP_VERSION%.0 ^
  --windows-product-version=%APP_VERSION%.0 ^
  --output-filename=%APP_NAME%.exe ^
  --output-dir=dist ^
  --remove-output ^
  --nofollow-import-to=tkinter ^
  --nofollow-import-to=pytest ^
  --nofollow-import-to=unittest ^
  --report=build\nuitka\nuitka-report.xml ^
  --report-diffable ^
  %EXTRA_ARGS% "%ENTRY%"

if errorlevel 1 (
  echo [ERROR] Nuitka build failed with exit code %errorlevel%.
  exit /b %errorlevel%
)

if /I "%MODE%"=="onefile" (
  if not exist "dist\%APP_NAME%.exe" (
    echo [ERROR] Build completed but dist\%APP_NAME%.exe was not found.
    exit /b 5
  )
  echo [OK] Built dist\%APP_NAME%.exe
) else (
  echo [OK] Built standalone directory under dist\
)
exit /b 0
