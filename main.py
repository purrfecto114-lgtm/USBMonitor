#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Executable entry point for script, package, and frozen builds.

Supported launch forms:

- ``python -m usb_monitor``
- ``python usb_monitor/main.py``
- Nuitka / PyInstaller targeting this file directly

When this file is executed as a plain script, Python does not set a package
context, so relative imports would fail.  In that case we temporarily prepend
the project root to ``sys.path`` and import through the package name.
"""

from __future__ import annotations

from pathlib import Path
import sys


def _load_main():
    if __package__:
        from .app import main as app_main

        return app_main

    package_dir = Path(__file__).resolve().parent
    project_root = package_dir.parent
    root_text = str(project_root)
    if root_text not in sys.path:
        sys.path.insert(0, root_text)

    from usb_monitor.app import main as app_main

    return app_main


if __name__ == "__main__":
    sys.exit(_load_main()())
