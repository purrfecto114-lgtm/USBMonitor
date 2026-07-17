"""Console-preserving USB Monitor entry point for diagnostics."""
from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parent
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from usb_monitor.app import main as application_main

    return int(application_main())


if __name__ == "__main__":
    raise SystemExit(main())
