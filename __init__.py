"""USB Monitor — Windows USB plug/unplug notifier.

Modular rewrite of the original single-file app. Public entry points:

- ``python -m usb_monitor``  → runs the app via ``__main__.py``
- ``python usb_monitor/main.py``  → runs the app via ``main.py``
- ``from usb_monitor.app import main``  → programmatic entry point

Note: ``main`` is intentionally NOT re-exported from this package
namespace, because ``usb_monitor.main`` must remain resolvable to the
``main.py`` module (Nuitka / PyInstaller target it as the entry point).
"""

from __future__ import annotations

from .config import (
    APP_DISPLAY_NAME,
    APP_NAME,
    APP_ORG,
    AppConfig,
    ConfigStore,
    LogMode,
    app_data_dir,
    config_path,
    default_log_dir,
)
from .events import EventSink, UsbEvent, emit_usb_event

__all__ = [
    "APP_DISPLAY_NAME",
    "APP_NAME",
    "APP_ORG",
    "AppConfig",
    "ConfigStore",
    "EventSink",
    "LogMode",
    "UsbEvent",
    "app_data_dir",
    "config_path",
    "default_log_dir",
    "emit_usb_event",
]

__version__ = "7.0.0"
