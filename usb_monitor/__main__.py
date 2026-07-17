"""Allow ``python -m usb_monitor`` to serve as the CLI entry point."""
from __future__ import annotations

import sys

from usb_monitor.app import main

if __name__ == "__main__":
    sys.exit(main())
