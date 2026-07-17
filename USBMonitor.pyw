"""USB Monitor stable application entry point.

Supported uses:
- Double-click with pythonw.exe on Windows.
- ``python USBMonitor.pyw`` during development.
- Nuitka onefile/standalone compilation target.
"""
from __future__ import annotations

import sys
from pathlib import Path


def _ensure_source_root() -> None:
    """Make the adjacent source package importable for source and compiled runs."""
    root = Path(__file__).resolve().parent
    root_text = str(root)
    if root_text not in sys.path:
        sys.path.insert(0, root_text)


def main() -> int:
    _ensure_source_root()
    from usb_monitor.app import main as application_main

    return int(application_main())


if __name__ == "__main__":
    raise SystemExit(main())
