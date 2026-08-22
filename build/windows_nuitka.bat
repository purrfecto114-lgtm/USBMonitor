@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem USB Monitor — Nuitka build script for Windows.
rem
rem Usage:
rem   build\windows_nuitka.bat              -^> onefile (default)
rem   build\windows_nuitka.bat onefile      -^> single-file EXE
rem   build\windows_nuitka.bat standalone   -^> self-contained directory
rem
rem Environment overrides:
rem   USBMONITOR_CONSOLE=1     force console window (for debugging)
rem   USBMONITOR_NO_UPX=1      skip UPX post-compression
rem   NUITKA_EXTRA_ARGS=...    append extra Nuitka arguments

cd /d "%~dp0\.." || exit /b 1

set "APP_NAME=USBMonitor"
set "APP_VERSION=1.1.0"
set "ENTRY=USBMonitor.pyw"
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

rem CI may pin an interpreter explicitly; local builds prefer the `py` launcher.
if defined USBMONITOR_PYTHON (
  set "PY_LAUNCHER=%USBMONITOR_PYTHON%"
  %USBMONITOR_PYTHON% -c "import sys; assert sys.version_info[:2] >= (3,11)" >nul 2>nul
  if errorlevel 1 (
    echo [ERROR] USBMONITOR_PYTHON does not point to Python 3.11+.
    exit /b 4
  )
) else (
  set "PY_LAUNCHER=py -3.11"
  py -3.11 -c "import sys; print(sys.executable)" >nul 2>nul
  if errorlevel 1 (
    set "PY_LAUNCHER=python"
    python -c "import sys; assert sys.version_info[:2] >= (3,11)" >nul 2>nul
    if errorlevel 1 (
      echo [ERROR] Python 3.11+ not found. Install from https://www.python.org/
      exit /b 4
    )
  )
)

%PY_LAUNCHER% -c "import sys; assert sys.version_info[:2] >= (3,11); import PySide6, win32api, nuitka" >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Build environment is incomplete.
  echo Run: %PY_LAUNCHER% -m pip install -r requirements-build.txt
  exit /b 4
)

if not exist "dist" mkdir "dist"
if not exist "build\nuitka" mkdir "build\nuitka"

set "MODE_ARGS=--onefile"
if /I "%MODE%"=="standalone" set "MODE_ARGS=--standalone"

set "CONSOLE_MODE=disable"
if /I "%USBMONITOR_CONSOLE%"=="1" set "CONSOLE_MODE=force"

set "EXTRA_ARGS=%NUITKA_EXTRA_ARGS%"

rem Pass the bundled UPX binary to Nuitka when it is available and not
rem explicitly disabled.  Nuitka forwards --upx-binary to the onefile
rem compression stage; standalone builds do not benefit from UPX.
set "UPX_ARGS="
if /I not "%USBMONITOR_NO_UPX%"=="1" (
  if exist "upx\upx.exe" (
    set "UPX_ARGS=--upx-binary=upx\upx.exe"
  )
)

%PY_LAUNCHER% -m nuitka ^
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
  --no-docstrings ^
  --lto=yes ^
  --nofollow-import-to=tkinter ^
  --nofollow-import-to=pytest ^
  --nofollow-import-to=unittest ^
  --nofollow-import-to=PySide6.QtNetwork ^
  --nofollow-import-to=PySide6.QtSql ^
  --nofollow-import-to=PySide6.QtPrintSupport ^
  --nofollow-import-to=PySide6.QtMultimedia ^
  --nofollow-import-to=PySide6.QtQml ^
  --nofollow-import-to=PySide6.QtQuick ^
  --nofollow-import-to=PySide6.QtQuickWidgets ^
  --nofollow-import-to=PySide6.QtConcurrent ^
  --nofollow-import-to=PySide6.QtTest ^
  --nofollow-import-to=PySide6.QtOpenGL ^
  --nofollow-import-to=PySide6.QtOpenGLWidgets ^
  --nofollow-import-to=PySide6.QtBluetooth ^
  --nofollow-import-to=PySide6.QtCharts ^
  --nofollow-import-to=PySide6.QtDataVisualization ^
  --nofollow-import-to=PySide6.QtDesigner ^
  --nofollow-import-to=PySide6.QtHelp ^
  --nofollow-import-to=PySide6.QtLocation ^
  --nofollow-import-to=PySide6.QtPositioning ^
  --nofollow-import-to=PySide6.QtSensors ^
  --nofollow-import-to=PySide6.QtSerialPort ^
  --nofollow-import-to=PySide6.QtWebChannel ^
  --nofollow-import-to=PySide6.QtWebEngine ^
  --nofollow-import-to=PySide6.QtWebEngineWidgets ^
  --nofollow-import-to=PySide6.QtWebSockets ^
  --nofollow-import-to=PySide6.QtXmlPatterns ^
  --nofollow-import-to=PySide6.Qt3DCore ^
  --nofollow-import-to=PySide6.Qt3DRender ^
  --nofollow-import-to=PySide6.Qt3DInput ^
  --nofollow-import-to=PySide6.Qt3DAnimation ^
  --nofollow-import-to=PySide6.Qt3DExtras ^
  --nofollow-import-to=PySide6.QtPdf ^
  --nofollow-import-to=PySide6.QtPdfWidgets ^
  --report=build\nuitka\nuitka-report.xml ^
  --report-diffable ^
  %UPX_ARGS% ^
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
