"""Allow ``python -m usb_monitor`` to launch the app.

This file and :mod:`usb_monitor.main` share the same body so that
either invocation path works the same way. Nuitka / PyInstaller should
target ``usb_monitor/main.py`` as the entry point.
"""

from __future__ import annotations

import sys

from .app import main

if __name__ == "__main__":
    sys.exit(main())
