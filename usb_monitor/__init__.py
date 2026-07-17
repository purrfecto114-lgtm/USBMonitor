"""USB Monitor — Windows tray + toast notifier for USB storage events.

Public API is split across two submodules:

* :mod:`usb_monitor.core` — pure functions, models, config (no Qt, no Win32).
  Import-safe in any environment; this is what unit tests exercise.
* :mod:`usb_monitor.app` — the Windows application: Win32 storage API
  bindings, the reconciler/scanner service, the PySide6 GUI, and the CLI
  entry point.  Importing this on non-Windows or without PySide6 will
  succeed but the GUI classes degrade to a test-only stub.

The canonical invocation is ``python -m usb_monitor`` (see
:mod:`usb_monitor.__main__`).
"""

from __future__ import annotations

# Public symbols re-exported for convenience.  Tests historically use
# ``from usb_monitor import DriveScanner`` etc., so we mirror the surface
# from both submodules at the package level.
from . import app as _app  # noqa: F401  (re-exported via star below)
from .core import (
    AppConfig,
    LogMode,
    SENSITIVE_KEYS,
    UsbEvent,
    VolumeInfo,
    anchored_window_geometry,
    as_bool,
    as_int,
    countdown_label,
    display_name_for_path,
    event_summary,
    format_bytes,
    group_title,
    group_volumes,
    stable_fingerprint,
    normalize_drive_path,
    normalize_hook_rules,
    normalize_recent_records,
    now_local,
    now_utc,
    precise_percent,
    progress_tooltip_text,
    redact,
    sanitize_for_log,
)

# Re-export the GUI/service classes from the app submodule so callers can
# do ``from usb_monitor import DriveScanner, ToastWindow`` without reaching
# into ``usb_monitor.app`` directly.  We do this after the core import so
# the explicit names above take precedence and we can safely glob the rest.
from .app import *  # noqa: E402, F401, F403

__version__ = "1.0.0"
__all__ = [
    # core
    "AppConfig",
    "LogMode",
    "SENSITIVE_KEYS",
    "UsbEvent",
    "VolumeInfo",
    "anchored_window_geometry",
    "as_bool",
    "as_int",
    "countdown_label",
    "display_name_for_path",
    "event_summary",
    "format_bytes",
    "group_title",
    "group_volumes",
    "stable_fingerprint",
    "normalize_drive_path",
    "normalize_hook_rules",
    "normalize_recent_records",
    "now_local",
    "now_utc",
    "precise_percent",
    "progress_tooltip_text",
    "redact",
    "sanitize_for_log",
    "__version__",
]
