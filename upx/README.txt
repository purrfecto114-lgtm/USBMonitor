UPX — the Ultimate Packer for eXecutables
=========================================

This directory contains the official UPX 4.2.4 win64 binary used by the
Nuitka / PyInstaller build scripts under Windows.ver.

Files
-----
- upx.exe          64-bit UPX binary (PE32+ console executable)
- LICENSE          GNU GPL v2 with special exception
- README           Original UPX readme
- README.txt       This file

Where the binary came from
--------------------------
Source: https://github.com/upx/upx/releases/tag/v4.2.4
Asset : upx-4.2.4-win64.zip
SHA256: <filled in by build pipeline when packaged for release>

Why we ship it
--------------
- The Nuitka build script (`build_windows_nuitka.bat`) and the
  PyInstaller build script (`build_windows_onedir.bat`) both pass
  `--upx-binary=upx\upx.exe` (Nuitka) or `--upx-dir upx` (PyInstaller)
  and look for the binary at this exact path. Having it inside the
  repo means the build is reproducible without depending on the host
  PATH or a globally installed UPX.
- Bundling also avoids the well-known issue where some anti-virus
  products flag executables packed with a *random* UPX pulled from
  the internet; this binary is the unmodified upstream release.

Notes for use with Nuitka
-------------------------
- Nuitka >= 1.5 takes either `--upx-binary=<path>` (single binary) or
  `--upx-binary=<exe>,<extra-cmd>` (with extra flags). The build
  script uses `--upx-binary=upx\upx.exe` which is enough.
- If UPX breaks a Qt plugin DLL during compression, the resulting
  exe will fail to start with "Qt platform plugin could not be
  initialized". The escape hatch is `build_windows_onedir_noupx.bat`
  for PyInstaller. For Nuitka, you can rebuild with
  `set SHANG_NO_NUITKA_UPX=1` (handled by the build script).

License
-------
UPX is GPL-2.0-or-later with a special exception that allows it to
compress arbitrary binaries, including commercial ones. See LICENSE.
