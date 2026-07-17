@echo off
setlocal EnableExtensions
cd /d "%~dp0" || exit /b 1

where py >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Python launcher not found. Install Python 3.11 or newer first.
  pause
  exit /b 1
)

py -3.11 -c "import PySide6, win32api" >nul 2>nul
if errorlevel 1 (
  echo [INFO] Installing runtime dependencies...
  py -3.11 -m pip install -r requirements.txt
  if errorlevel 1 (
    echo [ERROR] Dependency installation failed.
    pause
    exit /b 1
  )
)

start "USB Monitor" /b pyw -3.11 "%~dp0USBMonitor.pyw" %*
if errorlevel 1 (
  echo [ERROR] USB Monitor failed to start.
  pause
  exit /b 1
)
exit /b 0
