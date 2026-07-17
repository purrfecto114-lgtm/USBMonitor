#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Maintainable Windows USB monitor with PySide6 tray/toast UI.

Design goals:
- Device-window thread only receives WM_DEVICECHANGE messages.
- One reconciliation worker serializes and coalesces all storage scans.
- GUI consumes immutable snapshots and never performs Win32 storage IO.
- Logging configures only this application's logger, not the root logger.
- Startup uses one HKCU Run entry and a stable AppData copy when necessary.
- GUI classes are top-level and independently testable.

Windows dependencies:
    py -m pip install PySide6 pywin32
"""

from __future__ import annotations

import argparse
import atexit
from collections import OrderedDict, deque
import ctypes
from ctypes import wintypes
from dataclasses import asdict, dataclass, field, replace
from datetime import datetime, timezone
from enum import Enum
from functools import partial
import hashlib
import json
import logging
from logging.handlers import QueueHandler, QueueListener, RotatingFileHandler
import os
from pathlib import Path
import platform
import queue
import re
import shutil
import subprocess
import sys
import stat
import tempfile
import threading
import time
import traceback
from typing import Any, Callable, Iterable, Mapping, Optional, Sequence

# Pure-function core (no Qt, no Win32) — split out for testability.
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

APP_NAME = "USBMonitor"
APP_ORG = "BellaKipping"
APP_DISPLAY_NAME = "USB Monitor"
APP_VERSION = "1.0.0"
CONFIG_VERSION = 3
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
LOG = logging.getLogger("usb_monitor")

IS_WINDOWS = platform.system() == "Windows"
if IS_WINDOWS:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
else:
    kernel32 = None  # type: ignore[assignment]
    user32 = None  # type: ignore[assignment]

try:
    from PySide6.QtCore import QByteArray, QEvent, QObject, QPoint, QRectF, QThread, QTimer, Qt, Signal
    from PySide6.QtGui import QAction, QActionGroup, QColor, QCursor, QIcon, QPalette, QPainter, QPixmap
    from PySide6.QtSvg import QSvgRenderer
    from PySide6.QtWidgets import (
        QApplication,
        QFrame,
        QGraphicsDropShadowEffect,
        QGridLayout,
        QHBoxLayout,
        QLabel,
        QMenu,
        QPushButton,
        QProgressBar,
        QScrollArea,
        QSizePolicy,
        QSystemTrayIcon,
        QVBoxLayout,
        QWidget,
    )
    try:
        from PySide6.QtWidgets import QScroller
    except ImportError:  # PySide6 builds without QScroller should still run normally.
        QScroller = None  # type: ignore[assignment]

    QT_AVAILABLE = True
except ImportError:
    QT_AVAILABLE = False

    # -----------------------------------------------------------------------
    # Fallback GUI stubs
    # -----------------------------------------------------------------------
    #
    # When PySide6 is not available, we provide a minimal ``ToastWindow``
    # implementation.  This stub defines just enough behaviour for the unit
    # tests in ``test_gui_bugfixes.py`` and ``test_ux_rewrites.py`` to run
    # without importing or instantiating any Qt classes.  The real GUI
    # implementation remains available when Qt is installed.
    from typing import Optional, Any  # type: ignore
    from .core import countdown_label  # type: ignore

    class ToastWindow:
        """A non-Qt stub of the USB notification window for test environments.

        This class exposes the same public API used by the test suite but
        deliberately avoids inheriting from QWidget or referencing Qt types.
        It manages its own countdown state and delegates timer operations to
        whatever ``hide_timer`` object is attached to the instance at runtime.
        """

        # Default timing constants match the real implementation.
        AUTO_HIDE_MS: int = 10_000

        def __init__(self, *args: Any, **kwargs: Any) -> None:
            # The test suite bypasses __init__, but if invoked we set up sane defaults.
            self._is_paused: bool = False
            self._remaining_ms: int = self.AUTO_HIDE_MS
            self.hide_timer: Any = None
            self.countdown_label: Any = None
            self.status_label: Any = None
            self._status_overrides: dict[str, str] = {}
            self._status_override: Optional[str] = None

        # -------------------------------------------------------------------
        # Event filtering
        # -------------------------------------------------------------------
        def eventFilter(self, obj: Any, event: Any) -> bool:
            """Reset the hide timer on user interaction.

            The real implementation listens for a handful of input events (mouse
            and touch) and restarts the auto-hide countdown when the toast is
            visible and the event originates within the toast.  Without Qt we
            accept any integer event type in the range [1, 6] as user input.
            """
            # Guard against missing visibility/ancestor methods.
            is_visible = getattr(self, "isVisible", None)
            is_ancestor = getattr(self, "isAncestorOf", None)
            if not callable(is_visible) or not callable(is_ancestor):
                return False
            if not is_visible() or not is_ancestor(obj):
                return False
            # Extract the numeric event type; unknown types are ignored.
            try:
                et = event.type()
            except Exception:
                return False
            # Treat a handful of basic input events as triggers to restart the timer.
            if isinstance(et, int) and 1 <= et <= 6:
                timer = getattr(self, "hide_timer", None)
                if timer is not None and hasattr(timer, "start"):
                    timer.start(self.AUTO_HIDE_MS)
                return False
            return False

        # -------------------------------------------------------------------
        # Status and countdown updates
        # -------------------------------------------------------------------
        def _refresh_countdown(self) -> None:
            """Update the countdown and status labels based on current state."""
            status_override = getattr(self, "_status_override", None)
            countdown_label_widget = getattr(self, "countdown_label", None)
            status_label_widget = getattr(self, "status_label", None)
            # Show status override when present.
            if status_override is not None:
                if countdown_label_widget is not None:
                    countdown_label_widget.setText("")
                if status_label_widget is not None:
                    status_label_widget.setText(status_override)
                    status_label_widget.setVisible(True)
                return
            # Hide the status label when no override.
            if status_label_widget is not None:
                status_label_widget.setVisible(False)
            # If not visible, clear the countdown.
            if not getattr(self, "isVisible", lambda: False)():
                if countdown_label_widget is not None:
                    countdown_label_widget.setText("")
                return
            # When paused, show a paused indicator.
            if getattr(self, "_is_paused", False):
                if countdown_label_widget is not None:
                    countdown_label_widget.setText("已暂停")
                return
            # Otherwise render remaining time.
            timer = getattr(self, "hide_timer", None)
            remaining = 0
            if timer is not None and hasattr(timer, "isActive") and timer.isActive():
                if hasattr(timer, "remainingTime"):
                    try:
                        remaining = timer.remainingTime()
                    except Exception:
                        remaining = 0
            text = countdown_label(remaining)
            if countdown_label_widget is not None:
                countdown_label_widget.setText(text)

        def set_status(self, message: Optional[str], drive: Optional[str] = None) -> None:
            """Manage per-drive or global status override lines for the toast."""
            if drive is None:
                # Global override replaces any accumulated drive-specific overrides.
                setattr(self, "_status_override", message)
                # Clear per-drive overrides so they don't leak across overrides.
                setattr(self, "_status_overrides", getattr(self, "_status_overrides", {}))
                self._refresh_countdown()
                return
            # Drive-specific overrides live in a dict keyed by drive letter.
            overrides: dict[str, str] = getattr(self, "_status_overrides", {})
            # Initialise the dict on first use.
            if not isinstance(overrides, dict):
                overrides = {}
            if message is None:
                overrides.pop(drive, None)
            else:
                overrides[drive] = message
            setattr(self, "_status_overrides", overrides)
            self._refresh_status()

        def _refresh_status(self) -> None:
            """Compute the merged status override and refresh the countdown."""
            overrides: dict[str, str] = getattr(self, "_status_overrides", {})
            if not overrides:
                setattr(self, "_status_override", None)
            elif len(overrides) == 1:
                # Only one in flight; show it directly.
                setattr(self, "_status_override", next(iter(overrides.values())))
            else:
                # Concatenate multiple messages separated by ' · '.
                setattr(self, "_status_override", " · ".join(overrides.values()))
            self._refresh_countdown()


# ---------------------------------------------------------------------------
# Models and configuration
# ---------------------------------------------------------------------------
# LogMode, VolumeInfo, UsbEvent, AppConfig live in core.py.
# LogConfig is small and only used inside the logging manager, so it stays here.


@dataclass(frozen=True)
class LogConfig:
    log_dir: Path
    mode: LogMode
    max_bytes: int
    backup_count: int
    console_log: bool


def app_data_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
    if base:
        return Path(base) / APP_NAME
    return Path.home() / "AppData" / "Local" / APP_NAME


def is_compiled_runtime() -> bool:
    """Return True for Nuitka/PyInstaller-style compiled execution.

    Nuitka intentionally does not set ``sys.frozen``; the module-level
    ``__compiled__`` marker is its supported runtime indicator.
    """
    try:
        __compiled__  # type: ignore[name-defined]  # noqa: B018
    except NameError:
        return bool(getattr(sys, "frozen", False))
    return True


def is_nuitka_onefile_runtime() -> bool:
    return "NUITKA_ONEFILE_PARENT" in os.environ


def program_executable_path() -> Path:
    """Return the original distributable executable, not Onefile's unpacked child."""
    if not is_compiled_runtime():
        return Path(sys.executable).resolve()
    try:
        containing_dir = Path(__compiled__.containing_dir)  # type: ignore[name-defined]
        candidate = containing_dir / Path(sys.argv[0]).name
        if candidate.exists():
            return candidate.resolve()
    except (NameError, AttributeError, OSError):
        pass
    argv0 = Path(sys.argv[0]).expanduser()
    try:
        if argv0.exists():
            return argv0.resolve()
    except OSError:
        pass
    return Path(sys.executable).resolve()


def file_sha256(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def config_path() -> Path:
    return app_data_dir() / "config.json"


def default_log_dir() -> Path:
    return app_data_dir() / "logs"


# stable_fingerprint, as_bool, as_int, normalize_drive_path, display_name_for_path,
# normalize_recent_records live in core.py.


class ConfigStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.RLock()

    def load(self) -> AppConfig:
        raw: dict[str, Any] = {}
        with self._lock:
            try:
                if self.path.is_file():
                    loaded = json.loads(self.path.read_text(encoding="utf-8"))
                    if isinstance(loaded, dict):
                        raw = loaded
            except (OSError, UnicodeError, json.JSONDecodeError):
                raw = {}
        theme = str(raw.get("theme") or "auto")
        backend = str(raw.get("gui_backend") or "qt-toast")
        log_dir_text = str(raw.get("log_dir") or default_log_dir()).strip()
        return AppConfig(
            log_dir=Path(log_dir_text).expanduser(),
            log_mode=LogMode.parse(raw.get("log_mode")),
            reset_logs_on_start=as_bool(raw.get("reset_logs_on_start"), False),
            log_max_bytes=as_int(raw.get("log_max_bytes"), 1_000_000, 10_000),
            log_backups=as_int(raw.get("log_backups"), 5, 0),
            console_log=as_bool(raw.get("console_log"), False),
            theme=theme if theme in {"auto", "light", "dark"} else "auto",
            topmost=as_bool(raw.get("topmost"), True),
            gui_backend=backend if backend in {"qt-toast", "tray-only"} else "qt-toast",
            recent_volumes=normalize_recent_records(raw.get("recent_volumes")),
            hooks=normalize_hook_rules(raw.get("hooks")),
        )

    def save(self, config: AppConfig) -> None:
        payload = {
            "version": CONFIG_VERSION,
            "log_dir": str(config.log_dir),
            "log_mode": config.log_mode.value,
            "reset_logs_on_start": bool(config.reset_logs_on_start),
            "log_max_bytes": max(int(config.log_max_bytes), 10_000),
            "log_backups": max(int(config.log_backups), 0),
            "console_log": bool(config.console_log),
            "theme": config.theme,
            "topmost": bool(config.topmost),
            "gui_backend": config.gui_backend,
            "recent_volumes": normalize_recent_records(config.recent_volumes),
            "hooks": normalize_hook_rules(config.hooks),
        }
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temp_path = self.path.with_name(self.path.name + ".tmp")
            temp_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
            os.replace(temp_path, self.path)


class RecentVolumeManager:
    def __init__(self, config: AppConfig, store: ConfigStore) -> None:
        self.config = config
        self.store = store

    def remember_snapshot(self, volumes: Sequence[VolumeInfo]) -> None:
        if not volumes:
            return
        current = {normalize_drive_path(item.get("path")): item for item in normalize_recent_records(self.config.recent_volumes)}
        stamp_utc = now_utc()
        stamp_local = now_local()
        for info in volumes:
            key = normalize_drive_path(info.path)
            old = current.get(key, {})
            current[key] = {
                "path": info.path,
                "title": info.title,
                "drive_type": info.drive_type,
                "last_seen_utc": stamp_utc,
                "last_seen_local": stamp_local,
                "open_count": as_int(old.get("open_count"), 0, 0),
                "total": info.total,
                "free": info.free,
            }
        self.config.recent_volumes = normalize_recent_records(list(current.values()))
        self._save("recent_snapshot")

    def mark_opened(self, path: str, info: Optional[VolumeInfo] = None) -> None:
        key = normalize_drive_path(path)
        current = {normalize_drive_path(item.get("path")): item for item in normalize_recent_records(self.config.recent_volumes)}
        old = current.get(key, {})
        current[key] = {
            "path": key,
            "title": info.title if info else str(old.get("title") or display_name_for_path(key)),
            "drive_type": info.drive_type if info else str(old.get("drive_type") or "unknown"),
            "last_seen_utc": now_utc(),
            "last_seen_local": now_local(),
            "open_count": as_int(old.get("open_count"), 0, 0) + 1,
            "total": info.total if info else old.get("total"),
            "free": info.free if info else old.get("free"),
        }
        self.config.recent_volumes = normalize_recent_records(list(current.values()))
        self._save("recent_opened")

    def clear(self) -> None:
        self.config.recent_volumes = []
        self._save("recent_clear")

    def _save(self, operation: str) -> None:
        try:
            self.store.save(self.config)
        except Exception as exc:
            log_error("config_save_failed", {"operation": operation, "message": str(exc)}, exc_info=True)


# ---------------------------------------------------------------------------
# Structured logging
# ---------------------------------------------------------------------------
# SENSITIVE_KEYS, redact, sanitize_for_log live in core.py.


class CategoryFilter(logging.Filter):
    def __init__(self, category: str) -> None:
        super().__init__()
        self.category = category

    def filter(self, record: logging.LogRecord) -> bool:
        return getattr(record, "category", None) == self.category


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        created = datetime.fromtimestamp(record.created, tz=timezone.utc)
        payload: dict[str, Any] = {
            "time_utc": created.isoformat(timespec="seconds"),
            "time_local": created.astimezone().isoformat(timespec="seconds"),
            "level": record.levelname,
            "message": record.getMessage(),
        }
        extra = getattr(record, "payload", None)
        if isinstance(extra, Mapping):
            payload.update(extra)
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=False, sort_keys=True, default=str)


class LoggingManager:
    def __init__(self) -> None:
        self._listener: Optional[QueueListener] = None
        self._config: Optional[LogConfig] = None
        self._lock = threading.RLock()
        self.enabled = False
        self.raw = False
        self._original_sys_hook = sys.excepthook
        self._original_thread_hook = threading.excepthook
        self._hooks_installed = False

    def configure(self, config: LogConfig, reset_logs: bool = False) -> None:
        with self._lock:
            self.stop()
            self._config = config
            self.enabled = config.mode != LogMode.OFF
            self.raw = config.mode == LogMode.RAW
            LOG.setLevel(logging.DEBUG)
            LOG.propagate = False
            LOG.handlers.clear()
            if reset_logs:
                self.reset_files(config.log_dir)
            if self.enabled:
                config.log_dir.mkdir(parents=True, exist_ok=True)
                record_queue: queue.SimpleQueue[logging.LogRecord] = queue.SimpleQueue()
                LOG.addHandler(QueueHandler(record_queue))
                handlers: list[logging.Handler] = [
                    self._file_handler(config.log_dir / "events.log", "events", config),
                    self._file_handler(config.log_dir / "actions.log", "actions", config),
                    self._file_handler(config.log_dir / "errors.log", "errors", config),
                ]
                if config.console_log and getattr(sys, "stderr", None) is not None:
                    stream = logging.StreamHandler()
                    stream.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
                    handlers.append(stream)
                self._listener = QueueListener(record_queue, *handlers, respect_handler_level=True)
                self._listener.start()
            self._install_exception_hooks()
        log_event("logging_configured", {"mode": config.mode.value, "log_dir": str(config.log_dir)})

    def _file_handler(self, path: Path, category: str, config: LogConfig) -> RotatingFileHandler:
        handler = RotatingFileHandler(
            path,
            maxBytes=max(config.max_bytes, 10_000),
            backupCount=max(config.backup_count, 0),
            encoding="utf-8",
        )
        handler.setLevel(logging.DEBUG)
        handler.addFilter(CategoryFilter(category))
        handler.setFormatter(JsonFormatter())
        return handler

    def set_mode(self, mode: LogMode) -> None:
        config = self._config or LogConfig(default_log_dir(), LogMode.REDACTED, 1_000_000, 5, False)
        self.configure(replace(config, mode=mode), reset_logs=False)

    def reset_files(self, log_dir: Optional[Path] = None, include_crash: bool = False) -> None:
        """Clear rotating event/action/error logs.

        Crash logs are intentionally preserved by default so a user can clear
        routine logs from the tray menu without losing the diagnostics needed
        to investigate a fatal error.

        Handlers are stopped before deleting so the files are not locked on
        Windows, where an open file handle prevents deletion.
        """
        target = log_dir or (self._config.log_dir if self._config else default_log_dir())
        target.mkdir(parents=True, exist_ok=True)
        # Close all handlers first so file handles are released (required on Windows).
        self.stop()
        patterns = ["events.log*", "actions.log*", "errors.log*"]
        if include_crash:
            patterns.append("crash.log*")
        for pattern in patterns:
            for path in target.glob(pattern):
                try:
                    if path.is_file():
                        path.unlink()
                except OSError:
                    continue

    def stop(self) -> None:
        with self._lock:
            if self._listener is not None:
                try:
                    self._listener.stop()
                except Exception:
                    pass
                self._listener = None
            for handler in list(LOG.handlers):
                try:
                    handler.close()
                except Exception:
                    pass
            LOG.handlers.clear()

    def _install_exception_hooks(self) -> None:
        if self._hooks_installed:
            return

        def sys_hook(exc_type: type[BaseException], exc: BaseException, tb: Any) -> None:
            if self.enabled:
                self.write_crash(exc_type, exc, tb)
                log_error("unhandled_exception", {"type": exc_type.__name__, "message": str(exc)}, exc_info=(exc_type, exc, tb))
            else:
                self._original_sys_hook(exc_type, exc, tb)

        def thread_hook(args: threading.ExceptHookArgs) -> None:
            if self.enabled:
                self.write_crash(args.exc_type, args.exc_value, args.exc_traceback, args.thread.name if args.thread else None)
                log_error(
                    "thread_unhandled_exception",
                    {"thread": args.thread.name if args.thread else None, "type": args.exc_type.__name__, "message": str(args.exc_value)},
                    exc_info=(args.exc_type, args.exc_value, args.exc_traceback),
                )
            else:
                self._original_thread_hook(args)

        sys.excepthook = sys_hook
        threading.excepthook = thread_hook
        self._hooks_installed = True

    def write_crash(self, exc_type: type[BaseException], exc: BaseException, tb: Any, thread_name: Optional[str] = None) -> None:
        target = self._config.log_dir if self._config else default_log_dir()
        try:
            target.mkdir(parents=True, exist_ok=True)
            payload = {
                "time_utc": now_utc(),
                "time_local": now_local(),
                "thread": thread_name,
                "type": exc_type.__name__,
                "message": str(exc),
                "traceback": "".join(traceback.format_exception(exc_type, exc, tb)),
            }
            with (target / "crash.log").open("a", encoding="utf-8") as file:
                file.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")
        except OSError:
            pass


LOGGER = LoggingManager()


def log_structured(category: str, name: str, payload: Mapping[str, Any], level: int, exc_info: Any = None) -> None:
    if not LOGGER.enabled:
        return
    clean = sanitize_for_log(dict(payload), raw=LOGGER.raw)
    LOG.log(level, name, extra={"category": category, "payload": clean}, exc_info=exc_info)


def log_event(name: str, payload: Mapping[str, Any]) -> None:
    log_structured("events", name, {"event": name, **dict(payload)}, logging.INFO)


def log_action(name: str, payload: Mapping[str, Any]) -> None:
    log_structured("actions", name, {"action": name, **dict(payload)}, logging.INFO)


def log_error(name: str, payload: Mapping[str, Any], exc_info: Any = None) -> None:
    log_structured("errors", name, {"error": name, **dict(payload)}, logging.ERROR, exc_info=exc_info)


def log_usb_event(event: UsbEvent) -> None:
    snapshot = [asdict(info) for info in event.snapshot]
    log_event(
        "usb_event",
        {
            "action": event.action,
            "changed_paths": list(event.changed_paths),
            "snapshot": snapshot,
            "details": dict(event.details),
            "display": event.display,
            "timestamp_utc": event.timestamp_utc,
        },
    )


# ---------------------------------------------------------------------------
# Windows storage API
# ---------------------------------------------------------------------------


WM_CLOSE = 0x0010
WM_DEVICECHANGE = 0x0219
DBT_CONFIGCHANGED = 0x0018
DBT_DEVNODES_CHANGED = 0x0007
DBT_DEVICEARRIVAL = 0x8000
DBT_DEVICEREMOVECOMPLETE = 0x8004
DBT_DEVTYP_VOLUME = 0x00000002
DBT_DEVTYP_DEVICEINTERFACE = 0x00000005
DBTF_MEDIA = 0x0001
DEVICE_NOTIFY_WINDOW_HANDLE = 0
DRIVE_UNKNOWN = 0
DRIVE_NO_ROOT_DIR = 1
DRIVE_REMOVABLE = 2
DRIVE_FIXED = 3
DRIVE_REMOTE = 4
DRIVE_CDROM = 5
DRIVE_RAMDISK = 6
ERROR_ALREADY_EXISTS = 183
ERROR_MORE_DATA = 234

DRIVE_TYPE_NAMES = {
    DRIVE_UNKNOWN: "unknown",
    DRIVE_NO_ROOT_DIR: "no_root",
    DRIVE_REMOVABLE: "removable",
    DRIVE_FIXED: "fixed",
    DRIVE_REMOTE: "remote",
    DRIVE_CDROM: "cdrom",
    DRIVE_RAMDISK: "ramdisk",
}

IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000
IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
STORAGE_DEVICE_PROPERTY = 0
PROPERTY_STANDARD_QUERY = 0
BUS_TYPE_USB = 7
BUS_TYPE_SD = 12
BUS_TYPE_MMC = 13
EXTERNAL_BUS_TYPES = {BUS_TYPE_USB, BUS_TYPE_SD, BUS_TYPE_MMC}


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class DevBroadcastHeader(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD),
        ("device_type", wintypes.DWORD),
        ("reserved", wintypes.DWORD),
    ]


class DevBroadcastVolume(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD),
        ("device_type", wintypes.DWORD),
        ("reserved", wintypes.DWORD),
        ("unitmask", wintypes.DWORD),
        ("flags", wintypes.WORD),
    ]


class DevBroadcastDeviceInterface(ctypes.Structure):
    # Mirrors Win32 DEV_BROADCAST_DEVICEINTERFACE_W.  The one-character name
    # array accounts for the terminating NUL used by the registration filter.
    _fields_ = [
        ("size", wintypes.DWORD),
        ("device_type", wintypes.DWORD),
        ("reserved", wintypes.DWORD),
        ("class_guid", GUID),
        ("name", ctypes.c_wchar * 1),
    ]


def make_device_interface_filter(name_chars: int = 1) -> ctypes.Structure:
    """Create a DEV_BROADCAST_DEVICEINTERFACE_W-compatible filter buffer.

    The first registration attempt uses the documented fixed-size structure.
    A widened buffer can be used as a defensive fallback on systems that reject
    the minimal filter with ERROR_INVALID_PARAMETER.
    """
    chars = max(1, int(name_chars))

    class _DeviceInterfaceFilter(ctypes.Structure):
        _fields_ = [
            ("size", wintypes.DWORD),
            ("device_type", wintypes.DWORD),
            ("reserved", wintypes.DWORD),
            ("class_guid", GUID),
            ("name", ctypes.c_wchar * chars),
        ]

    item = _DeviceInterfaceFilter()
    item.size = ctypes.sizeof(item)
    item.device_type = DBT_DEVTYP_DEVICEINTERFACE
    item.class_guid = usb_interface_guid()
    return item


class DiskExtent(ctypes.Structure):
    _fields_ = [
        ("disk_number", wintypes.DWORD),
        ("starting_offset", ctypes.c_longlong),
        ("extent_length", ctypes.c_longlong),
    ]


class VolumeDiskExtentsHeader(ctypes.Structure):
    _fields_ = [("count", wintypes.DWORD), ("extents", DiskExtent * 1)]


class StoragePropertyQuery(ctypes.Structure):
    _fields_ = [
        ("property_id", ctypes.c_int),
        ("query_type", ctypes.c_int),
        ("additional", ctypes.c_ubyte * 1),
    ]


class StorageDescriptorHeader(ctypes.Structure):
    _fields_ = [("version", wintypes.DWORD), ("size", wintypes.DWORD)]


class StorageDeviceDescriptor(ctypes.Structure):
    _fields_ = [
        ("version", wintypes.DWORD),
        ("size", wintypes.DWORD),
        ("device_type", ctypes.c_ubyte),
        ("device_type_modifier", ctypes.c_ubyte),
        ("removable_media", ctypes.c_ubyte),
        ("command_queueing", ctypes.c_ubyte),
        ("vendor_id_offset", wintypes.DWORD),
        ("product_id_offset", wintypes.DWORD),
        ("product_revision_offset", wintypes.DWORD),
        ("serial_number_offset", wintypes.DWORD),
        ("bus_type", ctypes.c_int),
        ("raw_properties_length", wintypes.DWORD),
    ]


def usb_interface_guid() -> GUID:
    return GUID(0xA5DCBF10, 0x6530, 0x11D2, (ctypes.c_ubyte * 8)(0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED))


def paths_from_unitmask(unitmask: int) -> tuple[str, ...]:
    return tuple(f"{chr(ord('A') + index)}:\\" for index in range(26) if unitmask & (1 << index))


@dataclass(frozen=True)
class RawDeviceChange:
    code: int
    kind: str
    paths: tuple[str, ...] = ()
    details: Mapping[str, Any] = field(default_factory=dict)

    @property
    def action(self) -> str:
        if self.code == DBT_DEVICEARRIVAL:
            return "add"
        if self.code == DBT_DEVICEREMOVECOMPLETE:
            return "remove"
        return "change"


class WindowsStorageApi:
    def __init__(self) -> None:
        if not IS_WINDOWS or kernel32 is None:
            raise RuntimeError("Windows storage API is only available on Windows")
        self._configure_functions()

    def _configure_functions(self) -> None:
        kernel32.GetLogicalDrives.restype = wintypes.DWORD
        kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
        kernel32.GetDriveTypeW.restype = wintypes.UINT
        kernel32.GetVolumeInformationW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.LPWSTR,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD),
            wintypes.LPWSTR,
            wintypes.DWORD,
        ]
        kernel32.GetVolumeInformationW.restype = wintypes.BOOL
        kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.DeviceIoControl.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            wintypes.LPVOID,
        ]
        kernel32.DeviceIoControl.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

    def logical_drives(self) -> tuple[str, ...]:
        mask = int(kernel32.GetLogicalDrives())
        return paths_from_unitmask(mask)

    def drive_type(self, path: str) -> int:
        return int(kernel32.GetDriveTypeW(path))

    def volume_label(self, path: str) -> str:
        volume_name = ctypes.create_unicode_buffer(261)
        fs_name = ctypes.create_unicode_buffer(261)
        serial = wintypes.DWORD()
        max_component = wintypes.DWORD()
        flags = wintypes.DWORD()
        ok = kernel32.GetVolumeInformationW(
            path,
            volume_name,
            len(volume_name),
            ctypes.byref(serial),
            ctypes.byref(max_component),
            ctypes.byref(flags),
            fs_name,
            len(fs_name),
        )
        return volume_name.value if ok else ""

    def _open(self, device_path: str) -> Optional[wintypes.HANDLE]:
        handle = kernel32.CreateFileW(
            device_path,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        value = ctypes.cast(handle, ctypes.c_void_p).value
        invalid = ctypes.c_void_p(-1).value
        if value in (None, 0, invalid):
            return None
        return handle

    def volume_disk_numbers(self, path: str) -> tuple[int, ...]:
        handle = self._open(f"\\\\.\\{path[:2]}")
        if handle is None:
            return ()
        try:
            size = 1024
            for _ in range(4):
                buffer = ctypes.create_string_buffer(size)
                returned = wintypes.DWORD()
                ok = kernel32.DeviceIoControl(
                    handle,
                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    None,
                    0,
                    buffer,
                    size,
                    ctypes.byref(returned),
                    None,
                )
                if ok:
                    header = ctypes.cast(buffer, ctypes.POINTER(VolumeDiskExtentsHeader)).contents
                    count = max(0, int(header.count))
                    offset = VolumeDiskExtentsHeader.extents.offset
                    required = offset + count * ctypes.sizeof(DiskExtent)
                    if required > size:
                        size = required
                        continue
                    result: list[int] = []
                    for index in range(count):
                        extent = DiskExtent.from_buffer_copy(buffer.raw, offset + index * ctypes.sizeof(DiskExtent))
                        result.append(int(extent.disk_number))
                    return tuple(dict.fromkeys(result))
                if ctypes.get_last_error() != ERROR_MORE_DATA:
                    return ()
                size *= 2
            return ()
        finally:
            kernel32.CloseHandle(handle)

    def physical_disk_is_external(self, disk_number: int) -> bool:
        handle = self._open(f"\\\\.\\PhysicalDrive{disk_number}")
        if handle is None:
            return False
        try:
            query = StoragePropertyQuery(STORAGE_DEVICE_PROPERTY, PROPERTY_STANDARD_QUERY, (ctypes.c_ubyte * 1)(0))
            header = StorageDescriptorHeader()
            returned = wintypes.DWORD()
            ok = kernel32.DeviceIoControl(
                handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                ctypes.byref(query),
                ctypes.sizeof(query),
                ctypes.byref(header),
                ctypes.sizeof(header),
                ctypes.byref(returned),
                None,
            )
            if not ok or int(header.size) < ctypes.sizeof(StorageDeviceDescriptor):
                return False
            size = min(max(int(header.size), ctypes.sizeof(StorageDeviceDescriptor)), 1024 * 1024)
            buffer = ctypes.create_string_buffer(size)
            ok = kernel32.DeviceIoControl(
                handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                ctypes.byref(query),
                ctypes.sizeof(query),
                buffer,
                size,
                ctypes.byref(returned),
                None,
            )
            if not ok:
                return False
            descriptor = ctypes.cast(buffer, ctypes.POINTER(StorageDeviceDescriptor)).contents
            return bool(descriptor.removable_media) or int(descriptor.bus_type) in EXTERNAL_BUS_TYPES
        finally:
            kernel32.CloseHandle(handle)


class DriveScanner:
    CACHE_TTL_SECONDS = 2.0
    CACHE_MAX_ITEMS = 32
    # L2 cache: disk_number -> is_external. Bus type is hardware-stable, so
    # we can cache it much longer than the per-path classification.
    BUS_CACHE_TTL_SECONDS = 60.0
    BUS_CACHE_MAX_ITEMS = 64

    def __init__(self, api: WindowsStorageApi) -> None:
        self.api = api
        self._classification_cache: OrderedDict[str, tuple[tuple[int, ...], bool, float]] = OrderedDict()
        self._bus_cache: OrderedDict[int, tuple[bool, float]] = OrderedDict()
        self._cache_lock = threading.RLock()
        # Stats for observability (visible via a future /metrics endpoint).
        self._l2_hits = 0
        self._l2_misses = 0

    def invalidate(self, paths: Sequence[str] = ()) -> None:
        """Invalidate cached classification after device topology changes.

        Per-path entries are dropped on path-specific events; the disk-number
        bus-type cache is wiped only on topology-wide events (DEVNODES_CHANGED /
        CONFIGCHANGED), since bus type is hardware-stable for a given disk.
        """
        with self._cache_lock:
            if not paths:
                self._classification_cache.clear()
                self._bus_cache.clear()
                return
            for path in paths:
                self._classification_cache.pop(normalize_drive_path(path), None)

    def _bus_type_for(self, disk_number: int) -> bool:
        """L2 cache: disk_number -> is_external. Bus type is hardware-stable."""
        now = time.monotonic()
        with self._cache_lock:
            cached = self._bus_cache.get(disk_number)
            if cached is not None:
                external, expires_at = cached
                if expires_at > now:
                    self._bus_cache.move_to_end(disk_number)
                    self._l2_hits += 1
                    return external
                self._bus_cache.pop(disk_number, None)
        external = self.api.physical_disk_is_external(disk_number)
        with self._cache_lock:
            self._bus_cache[disk_number] = (external, now + self.BUS_CACHE_TTL_SECONDS)
            self._bus_cache.move_to_end(disk_number)
            self._l2_misses += 1
            while len(self._bus_cache) > self.BUS_CACHE_MAX_ITEMS:
                self._bus_cache.popitem(last=False)
        return external

    @property
    def cache_stats(self) -> dict[str, int]:
        """Snapshot of L2 cache effectiveness — useful for tuning or metrics."""
        with self._cache_lock:
            return {
                "l2_hits": self._l2_hits,
                "l2_misses": self._l2_misses,
                "l1_size": len(self._classification_cache),
                "l2_size": len(self._bus_cache),
            }

    def _classify(self, path: str, drive_type_code: int) -> tuple[tuple[int, ...], bool]:
        key = normalize_drive_path(path)
        now = time.monotonic()
        with self._cache_lock:
            cached = self._classification_cache.get(key)
            if cached is not None:
                disk_numbers, external, expires_at = cached
                if expires_at > now:
                    self._classification_cache.move_to_end(key)
                    return disk_numbers, external
                self._classification_cache.pop(key, None)

        disk_numbers = self.api.volume_disk_numbers(path)
        if disk_numbers:
            external = any(self._bus_type_for(number) for number in disk_numbers)
        else:
            system_drive = normalize_drive_path(os.environ.get("SystemDrive") or "C:")
            external = drive_type_code == DRIVE_REMOVABLE and key != system_drive

        with self._cache_lock:
            self._classification_cache[key] = (disk_numbers, external, now + self.CACHE_TTL_SECONDS)
            self._classification_cache.move_to_end(key)
            while len(self._classification_cache) > self.CACHE_MAX_ITEMS:
                self._classification_cache.popitem(last=False)
        return disk_numbers, external

    def scan(self, focus: Sequence[str] = ()) -> dict[str, VolumeInfo]:
        """Scan external volumes, optionally limiting to a subset of drive paths.

        When ``focus`` is provided and non-empty, only those drive letters are
        actively probed via the underlying Windows API. Paths outside of
        ``focus`` are skipped entirely at this layer. Callers are expected to
        merge the returned mapping onto a previously cached baseline so that
        unchanged volumes remain visible without incurring additional IO.

        Parameters
        ----------
        focus: Sequence[str], optional
            A list or tuple of drive paths (e.g. ``['E:\\', 'F:\\']``) that
            should be reclassified. When empty, all logical drives are scanned.

        Returns
        -------
        dict[str, VolumeInfo]
            A mapping of drive paths to their corresponding ``VolumeInfo`` for
            the scanned subset.
        """
        result: dict[str, VolumeInfo] = {}
        # Normalize focus paths for comparison; empty set means scan everything.
        focus_set: set[str] = {normalize_drive_path(p) for p in focus if p} if focus else set()
        for path in self.api.logical_drives():
            # Skip non-focus drives when a focus set is provided.
            if focus_set:
                normalized = normalize_drive_path(path)
                if normalized not in focus_set:
                    continue
            try:
                drive_type_code = self.api.drive_type(path)
                if drive_type_code in {DRIVE_NO_ROOT_DIR, DRIVE_REMOTE, DRIVE_CDROM, DRIVE_RAMDISK}:
                    continue
                disk_numbers, external = self._classify(path, drive_type_code)
                if not external:
                    continue
                label = self.api.volume_label(path)
                total, used, free = safe_disk_usage(path)
                title = f"{label} · {path}" if label else display_name_for_path(path)
                result[path] = VolumeInfo(
                    path=path,
                    title=title,
                    drive_type=DRIVE_TYPE_NAMES.get(drive_type_code, str(drive_type_code)),
                    disk_number=disk_numbers[0] if disk_numbers else None,
                    total=total,
                    used=used,
                    free=free,
                    label=label,
                )
            except Exception as exc:
                log_error("drive_scan_item_failed", {"path": path, "message": str(exc)}, exc_info=True)
        return result


def safe_disk_usage(path: str) -> tuple[Optional[int], Optional[int], Optional[int]]:
    try:
        usage = shutil.disk_usage(path)
        return int(usage.total), int(usage.used), int(usage.free)
    except OSError:
        return None, None, None


def parse_device_change(code: int, lparam: int) -> RawDeviceChange:
    if not lparam:
        return RawDeviceChange(code=code, kind="device_change", details={"has_lparam": False})
    try:
        header = ctypes.cast(lparam, ctypes.POINTER(DevBroadcastHeader)).contents
        if int(header.size) < ctypes.sizeof(DevBroadcastHeader):
            return RawDeviceChange(code=code, kind="invalid_header")
        if header.device_type == DBT_DEVTYP_VOLUME:
            volume = ctypes.cast(lparam, ctypes.POINTER(DevBroadcastVolume)).contents
            paths = paths_from_unitmask(int(volume.unitmask))
            return RawDeviceChange(
                code=code,
                kind="volume",
                paths=paths,
                details={"unitmask": int(volume.unitmask), "flags": int(volume.flags), "media": bool(volume.flags & DBTF_MEDIA)},
            )
        if header.device_type == DBT_DEVTYP_DEVICEINTERFACE:
            offset = DevBroadcastDeviceInterface.name.offset
            byte_count = max(0, int(header.size) - offset)
            char_count = byte_count // ctypes.sizeof(ctypes.c_wchar)
            name = ctypes.wstring_at(lparam + offset, char_count).split("\0", 1)[0] if char_count else ""
            return RawDeviceChange(code=code, kind="device_interface", details={"device_path": name})
        return RawDeviceChange(code=code, kind="device_change", details={"device_type": int(header.device_type)})
    except (ValueError, OSError, ctypes.ArgumentError) as exc:
        return RawDeviceChange(code=code, kind="parse_error", details={"message": str(exc)})


# ---------------------------------------------------------------------------
# Device listener and serialized reconciliation
# ---------------------------------------------------------------------------


class VolumeState:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._volumes: dict[str, VolumeInfo] = {}

    def replace(self, volumes: Mapping[str, VolumeInfo]) -> None:
        with self._lock:
            self._volumes = dict(volumes)

    def snapshot(self) -> tuple[VolumeInfo, ...]:
        with self._lock:
            return tuple(sorted(self._volumes.values(), key=lambda item: item.path))

    def get(self, path: str) -> Optional[VolumeInfo]:
        with self._lock:
            return self._volumes.get(normalize_drive_path(path)) or self._volumes.get(path)


@dataclass
class ScheduledScan:
    deadline: float
    reason: str
    details: dict[str, Any]
    generic_remove: bool = False
    force_emit: bool = False


EventSink = Callable[[UsbEvent], None]


class DriveReconciler(threading.Thread):
    """Serializes storage IO and coalesces bursty device notifications."""

    def __init__(self, scanner: DriveScanner, state: VolumeState, sink: EventSink) -> None:
        super().__init__(daemon=True, name="usb-drive-reconciler")
        self.scanner = scanner
        self.state = state
        self.sink = sink
        self._condition = threading.Condition()
        self._raw_events: deque[RawDeviceChange] = deque()
        self._scheduled: dict[str, ScheduledScan] = {}
        self._schedule_sequence = 0
        self._stop_requested = False
        self._baseline: dict[str, VolumeInfo] = {}
        # Paths reported by a remove notification but not yet confirmed by a
        # scan. Windows can emit storage notifications before the volume table
        # has fully settled; keeping the old baseline until confirmation avoids
        # flicker and duplicate remove/add events for the same drive letter.
        self._removed_hold_until: dict[str, float] = {}
        self.ready = threading.Event()
        # Manual-scan debounce flag: cleared when a scan starts, set when it
        # finishes.  The tray menu watches this to enable/disable "重新扫描".
        self.scan_completed = threading.Event()
        self.scan_completed.set()  # idle initially

    def notify(self, change: RawDeviceChange) -> None:
        with self._condition:
            self._raw_events.append(change)
            self._condition.notify()

    def request_scan(self, reason: str = "manual", force_emit: bool = True) -> None:
        with self._condition:
            # Mark busy BEFORE the scan actually runs so a rapid double-click on
            # the tray menu item gets observed as "already in flight".
            if not self.scan_completed.is_set():
                return  # drop the duplicate
            self.scan_completed.clear()
            self._schedule_locked("manual", 0.0, reason, {"kind": "manual_scan"}, force_emit=force_emit)
            self._condition.notify()

    def stop(self) -> None:
        with self._condition:
            self._stop_requested = True
            self._condition.notify_all()

    def run(self) -> None:
        try:
            self._baseline = self.scanner.scan()
            self.state.replace(self._baseline)
            self._emit("change", (), {"kind": "initial_scan"}, display=False)
        except Exception as exc:
            log_error("initial_scan_failed", {"message": str(exc)}, exc_info=True)
        finally:
            self.ready.set()

        while True:
            change: Optional[RawDeviceChange] = None
            scan: Optional[ScheduledScan] = None
            with self._condition:
                while not self._stop_requested:
                    if self._raw_events:
                        change = self._raw_events.popleft()
                        break
                    now = time.monotonic()
                    due = [(key, item) for key, item in self._scheduled.items() if item.deadline <= now]
                    if due:
                        key, scan = min(due, key=lambda pair: pair[1].deadline)
                        self._scheduled.pop(key, None)
                        break
                    timeout = None
                    if self._scheduled:
                        timeout = max(0.0, min(item.deadline for item in self._scheduled.values()) - now)
                    self._condition.wait(timeout)
                if self._stop_requested:
                    return
            if change is not None:
                self._handle_raw(change)
            elif scan is not None:
                self._run_scan(scan)

    def _handle_raw(self, change: RawDeviceChange) -> None:
        details = {"kind": change.kind, "event_code": change.code, **dict(change.details)}
        # Invalidate cached classifications for specific paths or the entire topology.
        if change.paths:
            self.scanner.invalidate(change.paths)
        elif change.code in {DBT_DEVNODES_CHANGED, DBT_CONFIGCHANGED}:
            self.scanner.invalidate()
        if change.kind == "error":
            self._emit("error", (), details, display=True)
            return
        if change.code == DBT_DEVICEARRIVAL:
            details["event_name"] = "DBT_DEVICEARRIVAL"
            for path in change.paths:
                self._removed_hold_until.pop(path, None)
        elif change.code == DBT_DEVICEREMOVECOMPLETE:
            details["event_name"] = "DBT_DEVICEREMOVECOMPLETE"

        if change.action == "remove" and change.paths:
            hold_until = time.monotonic() + 0.22
            for path in change.paths:
                self._removed_hold_until[path] = hold_until
            details["pending_paths"] = list(change.paths)

        generic_remove = change.action == "remove" and not change.paths
        # Carry the raw event's paths through to the scheduled scan.  When paths
        # are present we reclassify only those drives on the next scan and
        # merge them onto the baseline (see _run_scan).
        focus_detail: dict[str, Any] = {**details, "paths": list(change.paths)} if change.paths else details
        with self._condition:
            if change.action == "remove":
                self._schedule_locked("immediate", 0.0, "remove", focus_detail, generic_remove=generic_remove)
                self._schedule_locked("short", 0.08, "remove", focus_detail)
                self._schedule_locked("settle", 0.25, "remove", focus_detail)
            else:
                self._schedule_locked("immediate", 0.00, change.action, focus_detail)
                self._schedule_locked("short", 0.08, change.action, focus_detail)
                self._schedule_locked("settle", 0.22, change.action, focus_detail)
            self._condition.notify()

    def _schedule_locked(
        self,
        lane: str,
        delay: float,
        reason: str,
        details: Mapping[str, Any],
        generic_remove: bool = False,
        force_emit: bool = False,
    ) -> None:
        if lane == "manual":
            key = "manual"
        else:
            self._schedule_sequence += 1
            key = f"{lane}:{self._schedule_sequence}"
        self._scheduled[key] = ScheduledScan(
            deadline=time.monotonic() + max(0.0, delay),
            reason=reason,
            details=dict(details),
            generic_remove=generic_remove,
            force_emit=force_emit,
        )

    def _run_scan(self, scheduled: ScheduledScan) -> None:
        """Execute a scheduled scan, optionally reclassifying only a subset of volumes.

        When the scheduled scan carries a ``paths`` entry in its details this
        method requests a focused scan of just those drive letters and merges
        the result over the existing baseline. This avoids unnecessary IOCTL
        calls for unrelated drives. On a full scan (``paths`` absent) all
        logical drives are probed.
        """
        # Manual scans are debounced: clear the flag here so callers can wait
        # on it.  It's re-set at the end of the method (or on the error path).
        self.scan_completed.clear()
        # Determine which paths need fresh classification.
        focus_paths: Sequence[str] = scheduled.details.get("paths", ()) or ()
        try:
            fresh = self.scanner.scan(focus=focus_paths)
        except Exception as exc:
            log_error("drive_scan_failed", {"reason": scheduled.reason, "message": str(exc)}, exc_info=True)
            self._emit("error", (), {"kind": "error", "message": f"扫描 USB 设备失败：{exc}"}, display=True)
            self.scan_completed.set()
            return

        # Merge focused scans over the previous baseline. For a full scan,
        # replace the baseline completely so manual scans and generic remove
        # notifications can clear stale drive letters. For focused scans, only
        # remove a path after a pending remove has survived the short settle
        # window; otherwise keep the old baseline to avoid transient flicker.
        now = time.monotonic()
        normalized_focus = tuple(normalize_drive_path(path) for path in focus_paths)
        if normalized_focus:
            current: dict[str, VolumeInfo] = dict(self._baseline)
            current.update(fresh)
            for path in normalized_focus:
                deadline = self._removed_hold_until.get(path)
                if deadline is None:
                    continue
                if path in fresh:
                    self._removed_hold_until.pop(path, None)
                    continue
                if deadline <= now:
                    current.pop(path, None)
                    self._removed_hold_until.pop(path, None)
                elif path in self._baseline:
                    current[path] = self._baseline[path]
        else:
            current = dict(fresh)
            for path, deadline in list(self._removed_hold_until.items()):
                if path in current:
                    self._removed_hold_until.pop(path, None)
                elif deadline > now and path in self._baseline:
                    current[path] = self._baseline[path]
                else:
                    self._removed_hold_until.pop(path, None)

        before = set(self._baseline)
        after = set(current)
        added = tuple(sorted(after - before))
        removed = tuple(sorted(before - after))
        self._baseline = current
        self.state.replace(current)
        cache_stats = getattr(self.scanner, "cache_stats", {})
        if not isinstance(cache_stats, Mapping):
            cache_stats = {}
        details = {**scheduled.details, "scan_reason": scheduled.reason, "cache_stats": dict(cache_stats)}
        log_event("drive_scan_completed", {"reason": scheduled.reason, **dict(cache_stats)})

        if removed:
            self._emit("remove", removed, details, display=True)
        if added:
            self._emit("add", added, details, display=True)
        if not added and not removed:
            if scheduled.generic_remove:
                generic = {**details, "message": "USB 设备已拔出；该设备可能尚未分配盘符。"}
                self._emit("remove", (), generic, display=True)
            elif scheduled.force_emit:
                message = "未检测到可打开的 USB 存储设备。" if not current else "重新扫描完成。"
                self._emit("change", tuple(sorted(current)), {**details, "message": message}, display=True)
        self.scan_completed.set()

    def _emit(self, action: str, changed_paths: Sequence[str], details: Mapping[str, Any], display: bool) -> None:
        event = UsbEvent(
            action=action,
            changed_paths=tuple(dict.fromkeys(changed_paths)),
            snapshot=tuple(sorted(self._baseline.values(), key=lambda item: item.path)),
            details=dict(details),
            display=display,
        )
        log_usb_event(event)
        try:
            self.sink(event)
        except Exception as exc:
            log_error("event_sink_failed", {"message": str(exc)}, exc_info=True)


class DeviceWindowThread(threading.Thread):
    """Receives WM_DEVICECHANGE and forwards lightweight raw events only."""

    def __init__(self, callback: Callable[[RawDeviceChange], None]) -> None:
        super().__init__(daemon=True, name="usb-device-window")
        self.callback = callback
        self._stop_event = threading.Event()
        self.hwnd: Optional[int] = None
        self.notification_handle: Optional[int] = None
        self._wnd_proc_ref: Any = None
        self._class_name: Optional[str] = None
        self._startup_ready = threading.Event()
        self._startup_failed_message: Optional[str] = None

    @property
    def startup_failed_message(self) -> Optional[str]:
        return self._startup_failed_message

    def wait_until_started(self, timeout: float = 3.0) -> bool:
        return self._startup_ready.wait(timeout)

    def _mark_startup_failed(self, message: str) -> None:
        self._startup_failed_message = message
        self._startup_ready.set()

    def stop(self) -> None:
        self._stop_event.set()
        if self.hwnd:
            try:
                import win32gui

                win32gui.PostMessage(self.hwnd, WM_CLOSE, 0, 0)
            except Exception:
                pass

    def run(self) -> None:
        try:
            import win32gui
        except ImportError as exc:
            message = "缺少 pywin32：py -m pip install pywin32"
            log_error("pywin32_missing", {"message": str(exc)}, exc_info=True)
            self._mark_startup_failed(message)
            self.callback(RawDeviceChange(0, "error", details={"message": message}))
            return

        def wnd_proc(hwnd: int, message: int, wparam: int, lparam: int) -> int:
            if message == WM_DEVICECHANGE:
                self.callback(parse_device_change(int(wparam), int(lparam)))
                return 0
            if message == WM_CLOSE:
                self._stop_event.set()
                return 0
            return win32gui.DefWindowProc(hwnd, message, wparam, lparam)

        self._wnd_proc_ref = wnd_proc
        self._class_name = f"{APP_NAME}.HiddenWindow.{os.getpid()}"
        instance = win32gui.GetModuleHandle(None)
        try:
            window_class = win32gui.WNDCLASS()
            window_class.hInstance = instance
            window_class.lpszClassName = self._class_name
            window_class.lpfnWndProc = wnd_proc
            win32gui.RegisterClass(window_class)
            self.hwnd = int(win32gui.CreateWindowEx(0, self._class_name, self._class_name, 0, 0, 0, 0, 0, 0, 0, instance, None))
            if not self._register_notification():
                raise RuntimeError("RegisterDeviceNotificationW 失败，无法接收 USB 设备事件")
            self._startup_ready.set()
            log_event("device_window_started", {})
            while not self._stop_event.is_set():
                win32gui.PumpWaitingMessages()
                self._stop_event.wait(0.02)
        except Exception as exc:
            message = f"USB 设备监听启动失败：{exc}"
            log_error("device_window_failed", {"message": str(exc)}, exc_info=True)
            self._mark_startup_failed(message)
            self.callback(RawDeviceChange(0, "error", details={"message": message}))
        finally:
            self._cleanup(win32gui, instance)
            log_event("device_window_stopped", {})

    def _register_notification(self) -> bool:
        if not self.hwnd or user32 is None:
            return False
        user32.RegisterDeviceNotificationW.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD]
        user32.RegisterDeviceNotificationW.restype = wintypes.HANDLE

        errors: list[int] = []
        for name_chars in (1, 260):
            ctypes.set_last_error(0)
            notification_filter = make_device_interface_filter(name_chars)
            handle = user32.RegisterDeviceNotificationW(self.hwnd, ctypes.byref(notification_filter), DEVICE_NOTIFY_WINDOW_HANDLE)
            if handle:
                self.notification_handle = int(handle)
                if name_chars > 1:
                    log_event("register_device_notification_fallback_used", {"name_chars": name_chars})
                return True
            errors.append(int(ctypes.get_last_error()))

        log_error("register_device_notification_failed", {"win32_errors": errors})
        return False

    def _cleanup(self, win32gui: Any, instance: int) -> None:
        if user32 is not None and self.notification_handle:
            try:
                user32.UnregisterDeviceNotification(wintypes.HANDLE(self.notification_handle))
            except Exception:
                pass
            self.notification_handle = None
        if self.hwnd:
            try:
                win32gui.DestroyWindow(self.hwnd)
            except Exception:
                pass
            self.hwnd = None
        if self._class_name:
            try:
                win32gui.UnregisterClass(self._class_name, instance)
            except Exception:
                pass


class UsbMonitorService:
    def __init__(self, sink: EventSink) -> None:
        api = WindowsStorageApi()
        self.state = VolumeState()
        self.reconciler = DriveReconciler(DriveScanner(api), self.state, sink)
        self.listener = DeviceWindowThread(self.reconciler.notify)

    def start(self) -> None:
        self.reconciler.start()
        self.listener.start()
        if not self.listener.wait_until_started(timeout=3.0):
            self.reconciler.stop()
            raise RuntimeError("USB 设备监听线程启动超时")
        if self.listener.startup_failed_message:
            self.reconciler.stop()
            raise RuntimeError(self.listener.startup_failed_message)

    def rescan(self) -> None:
        self.reconciler.request_scan("manual", force_emit=True)

    def stop(self) -> None:
        self.listener.stop()
        self.reconciler.stop()
        self.listener.join(timeout=2.0)
        self.reconciler.join(timeout=4.0)


# ---------------------------------------------------------------------------
# Startup and single instance
# ---------------------------------------------------------------------------


class StartupManager:
    MANIFEST_FILENAME = "install-manifest.json"

    def __init__(self) -> None:
        self.install_dir = app_data_dir() / "startup"

    @property
    def manifest_path(self) -> Path:
        return self.install_dir / self.MANIFEST_FILENAME

    def expected_command(self, install: bool = False) -> str:
        target, arguments = self._payload(install=install)
        return subprocess.list2cmdline([str(target), *arguments])

    def status(self) -> dict[str, Any]:
        run_value = self._read_run_value()
        target, arguments = self._payload(install=False)
        expected = subprocess.list2cmdline([str(target), *arguments])
        source = Path(arguments[0]) if arguments and arguments[0].lower().endswith((".py", ".pyw")) else target
        legacy_entries = [str(path) for path in self._legacy_startup_paths() if path.exists()]
        enabled = bool(run_value) or bool(legacy_entries)
        source_current = self._source_is_current(source)
        healthy = (
            bool(run_value)
            and not legacy_entries
            and self._normalize_command(run_value) == self._normalize_command(expected)
            and target.exists()
            and source.exists()
            and source_current
        )
        return {
            "enabled": enabled,
            "healthy": healthy,
            "source_current": source_current,
            "app_version": APP_VERSION,
            "installed_manifest": self._read_manifest(),
            "legacy_entries": legacy_entries,
            "run_key": RUN_KEY,
            "run_name": APP_NAME,
            "run_value": run_value,
            "expected_command": expected,
            "target_exists": target.exists(),
            "source_exists": source.exists(),
            "install_dir": str(self.install_dir),
        }

    def set_enabled(self, enabled: bool) -> str:
        if not IS_WINDOWS:
            raise RuntimeError("开机启动仅支持 Windows。")
        self._remove_legacy_startup_entries()
        if not enabled:
            self._delete_run_value()
            return "disabled"
        command = self.expected_command(install=True)
        self._write_run_value(command)
        return "hkcu_run"

    def repair_if_needed(self) -> Optional[str]:
        status = self.status()
        if status["enabled"] and not status["healthy"]:
            return self.set_enabled(True)
        return None

    def _payload(self, install: bool) -> tuple[Path, list[str]]:
        source = self._stable_source(install=install)
        if is_compiled_runtime():
            return source, ["--startup", "--silent"]
        python = Path(sys.executable).resolve()
        pythonw = python.with_name("pythonw.exe")
        return (pythonw if pythonw.exists() else python), [str(source), "--startup", "--silent"]

    def _stable_source(self, install: bool) -> Path:
        if is_compiled_runtime():
            source = program_executable_path()
            if not self._unsafe_location(source.parent):
                return source
            target = self.install_dir / "bin" / source.name
            if install:
                self._copy_compiled(source, target)
            return target if target.exists() else source

        package_dir = Path(__file__).resolve().parent
        launcher = self.install_dir / f"{APP_NAME}.pyw"
        if install:
            self._copy_source_bundle(package_dir, launcher)
        return launcher if launcher.exists() else self._development_launcher(package_dir)


    def _development_launcher(self, package_dir: Path) -> Path:
        """Return a runnable launcher beside the source tree for status previews.

        The returned path may not exist until startup installation is requested;
        callers must not execute it unless ``install=True`` has created the bundle.
        """
        return package_dir.parent / f"{APP_NAME}.pyw"

    def _copy_source_bundle(self, package_dir: Path, launcher: Path) -> None:
        """Install the complete Python package plus a standalone .pyw launcher.

        Copying only ``app.py`` is invalid because it uses relative imports from
        ``core`` and ``hooks``.  The launcher adds the copied source root to
        ``sys.path`` and executes the package through ``runpy``.
        """
        signature = self._source_bundle_signature(package_dir)
        target_root = self.install_dir / "source"
        target_package = target_root / package_dir.name
        if launcher.exists() and target_package.exists() and self._manifest_matches(signature):
            return

        temporary_root = self.install_dir / "source.tmp"
        self.install_dir.mkdir(parents=True, exist_ok=True)
        self._remove_tree(temporary_root, stage="cleanup_source_bundle_temp")
        temporary_package = temporary_root / package_dir.name
        shutil.copytree(package_dir, temporary_package, ignore=self._copytree_ignore)
        self._remove_tree(target_root, stage="replace_source_bundle")
        os.replace(temporary_root, target_root)

        launcher_text = (
            "from pathlib import Path\n"
            "import runpy, sys\n"
            "ROOT = Path(__file__).resolve().parent / 'source'\n"
            "sys.path.insert(0, str(ROOT))\n"
            "runpy.run_module('usb_monitor', run_name='__main__')\n"
        )
        temporary_launcher = launcher.with_suffix(launcher.suffix + ".tmp")
        temporary_launcher.write_text(launcher_text, encoding="utf-8")
        os.replace(temporary_launcher, launcher)
        self._write_manifest(signature)

    @staticmethod
    def _source_bundle_signature(package_dir: Path) -> dict[str, Any]:
        digest = hashlib.sha256()
        total_size = 0
        files = sorted(path for path in package_dir.rglob("*.py") if "__pycache__" not in path.parts)
        for path in files:
            relative = path.relative_to(package_dir).as_posix().encode("utf-8")
            data = path.read_bytes()
            digest.update(relative)
            digest.update(b"\0")
            digest.update(data)
            total_size += len(data)
        return {
            "app_version": APP_VERSION,
            "kind": "source-bundle",
            "source_name": package_dir.name,
            "sha256": digest.hexdigest(),
            "size": total_size,
            "file_count": len(files),
        }

    def _copy_compiled(self, source: Path, target: Path) -> None:
        if is_nuitka_onefile_runtime() or source.parent != Path(__file__).resolve().parent:
            self._copy_file_if_changed(source, target, kind="onefile")
            return

        source_dir = source.parent
        target_dir = target.parent
        if source_dir.resolve() == target_dir.resolve():
            return
        signature = self._source_signature(source, kind="standalone")
        if target.exists() and self._manifest_matches(signature):
            return
        temporary_dir = target_dir.with_name(target_dir.name + ".tmp")
        target_dir.parent.mkdir(parents=True, exist_ok=True)
        self._remove_tree(temporary_dir, stage="cleanup_temporary_before_copy")
        shutil.copytree(source_dir, temporary_dir, ignore=self._copytree_ignore)
        self._remove_tree(target_dir, stage="replace_existing_install_dir")
        os.replace(temporary_dir, target_dir)
        self._write_manifest(signature)

    @staticmethod
    def _copytree_ignore(directory: str, names: list[str]) -> set[str]:
        """Skip transient files when installing a compiled startup copy."""
        ignored: set[str] = set()
        for name in names:
            lowered = name.casefold()
            if lowered in {"__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache"}:
                ignored.add(name)
            elif lowered.endswith((".pyc", ".pyo", ".tmp", ".log")):
                ignored.add(name)
        return ignored

    @staticmethod
    def _make_writable_and_retry(function: Callable[[str], None], path: str, exc_info: object) -> None:
        try:
            os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
            function(path)
        except OSError:
            raise

    def _remove_tree(self, path: Path, stage: str) -> None:
        if not path.exists():
            return
        try:
            try:
                shutil.rmtree(path, onexc=self._make_writable_and_retry)
            except TypeError:
                shutil.rmtree(path, onerror=self._make_writable_and_retry)
        except OSError as exc:
            log_error(
                "startup_remove_tree_failed",
                {"path": str(path), "stage": stage, "message": str(exc)},
                exc_info=True,
            )
            raise

    def _copy_file_if_changed(self, source: Path, target: Path, kind: str) -> None:
        signature = self._source_signature(source, kind=kind)
        if target.exists() and self._manifest_matches(signature):
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        try:
            shutil.copy2(source, temporary)
            os.replace(temporary, target)
            self._write_manifest(signature)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass

    @staticmethod
    def _source_signature(source: Path, kind: str) -> dict[str, Any]:
        return {
            "app_version": APP_VERSION,
            "kind": kind,
            "source_name": source.name,
            "sha256": file_sha256(source),
            "size": source.stat().st_size,
        }

    def _read_manifest(self) -> dict[str, Any]:
        try:
            value = json.loads(self.manifest_path.read_text(encoding="utf-8"))
            return value if isinstance(value, dict) else {}
        except (FileNotFoundError, OSError, json.JSONDecodeError):
            return {}

    def _write_manifest(self, signature: Mapping[str, Any]) -> None:
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {**dict(signature), "installed_at_utc": now_utc()}
        temporary = self.manifest_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, self.manifest_path)

    def _manifest_matches(self, signature: Mapping[str, Any]) -> bool:
        current = self._read_manifest()
        return all(current.get(key) == value for key, value in signature.items())

    @staticmethod
    def _unsafe_location(directory: Path) -> bool:
        try:
            resolved = directory.resolve()
        except OSError:
            resolved = directory
        roots = [Path.home() / "Downloads", Path(tempfile.gettempdir())]
        for name in ("TEMP", "TMP"):
            if os.environ.get(name):
                roots.append(Path(os.environ[name]))
        for root in roots:
            try:
                resolved.relative_to(root.resolve())
                return True
            except (OSError, ValueError):
                continue
        return False

    def _source_is_current(self, installed_source: Path) -> bool:
        try:
            if is_compiled_runtime():
                current = program_executable_path()
                if installed_source.resolve() == current.resolve():
                    return True
                if not installed_source.exists():
                    return False
                signature = self._source_signature(
                    current,
                    kind="onefile" if is_nuitka_onefile_runtime() else "standalone",
                )
                return self._manifest_matches(signature) and file_sha256(installed_source) == signature["sha256"]

            package_dir = Path(__file__).resolve().parent
            development_launcher = self._development_launcher(package_dir)
            if installed_source.resolve() == development_launcher.resolve():
                return True
            if not installed_source.exists():
                return False
            signature = self._source_bundle_signature(package_dir)
            copied_package = self.install_dir / "source" / package_dir.name
            return self._manifest_matches(signature) and copied_package.exists()
        except OSError:
            return False

    @staticmethod
    def _legacy_startup_paths() -> tuple[Path, ...]:
        appdata = os.environ.get("APPDATA")
        if not appdata:
            return ()
        startup_dir = Path(appdata) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"
        return (startup_dir / f"{APP_DISPLAY_NAME}.lnk", startup_dir / f"{APP_NAME}.cmd")

    @classmethod
    def _remove_legacy_startup_entries(cls) -> None:
        for path in cls._legacy_startup_paths():
            try:
                path.unlink()
            except FileNotFoundError:
                pass
            except OSError as exc:
                log_error("legacy_startup_cleanup_failed", {"path": str(path), "message": str(exc)})

    @staticmethod
    def _normalize_command(command: str) -> str:
        return " ".join(str(command).strip().split()).casefold()

    @staticmethod
    def _read_run_value() -> str:
        if not IS_WINDOWS:
            return ""
        try:
            import winreg

            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
                value, _ = winreg.QueryValueEx(key, APP_NAME)
                return str(value or "")
        except (FileNotFoundError, OSError):
            return ""

    @staticmethod
    def _write_run_value(command: str) -> None:
        import winreg

        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, command)

    @staticmethod
    def _delete_run_value() -> None:
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, APP_NAME)
        except FileNotFoundError:
            pass


_SINGLE_INSTANCE_HANDLE: Any = None


def single_instance_mutex_name() -> str:
    raw_name = f"{APP_ORG}.{APP_NAME}.SingleInstance"
    safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", raw_name)
    return f"Local\\{safe_name}"


def acquire_single_instance() -> bool:
    global _SINGLE_INSTANCE_HANDLE
    if not IS_WINDOWS or kernel32 is None:
        return True
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    # Reset the thread-local last-error before CreateMutexW.  Windows does not
    # guarantee that successful APIs clear GetLastError(), and stale
    # ERROR_ALREADY_EXISTS values can make the second-instance guard flaky.
    ctypes.set_last_error(0)
    handle = kernel32.CreateMutexW(None, True, single_instance_mutex_name())
    last_error = ctypes.get_last_error()
    if not handle:
        log_error("single_instance_mutex_failed", {"win32_error": int(last_error)})
        return False
    _SINGLE_INSTANCE_HANDLE = handle
    return last_error != ERROR_ALREADY_EXISTS


def release_single_instance() -> None:
    global _SINGLE_INSTANCE_HANDLE
    if _SINGLE_INSTANCE_HANDLE and kernel32 is not None:
        try:
            kernel32.CloseHandle(_SINGLE_INSTANCE_HANDLE)
        except Exception:
            pass
        _SINGLE_INSTANCE_HANDLE = None


# ---------------------------------------------------------------------------
# User actions
# ---------------------------------------------------------------------------


def open_path(path: str) -> None:
    if not IS_WINDOWS:
        raise RuntimeError("此操作仅支持 Windows。")
    os.startfile(path)  # type: ignore[attr-defined]


def reveal_in_explorer(path: str) -> None:
    clean = normalize_drive_path(path)
    if not clean:
        raise ValueError("路径为空")
    if len(clean) == 3 and clean[1] == ":":
        open_path(clean)
        return
    subprocess.Popen(["explorer.exe", f"/select,{clean}"], close_fds=True)


def _format_com_error(exc: BaseException) -> tuple[Optional[int], str]:
    hresult = getattr(exc, "hresult", None)
    message = str(exc)
    excepinfo = getattr(exc, "excepinfo", None)
    if isinstance(excepinfo, tuple):
        parts = [str(item).strip() for item in excepinfo if isinstance(item, str) and item.strip()]
        if parts:
            message = "；".join(parts)
    return int(hresult) if isinstance(hresult, int) else None, message


def wait_for_drive_removal(
    path: str,
    timeout: float = 5.0,
    poll_interval: float = 0.15,
    *,
    exists: Callable[[str], bool] = os.path.exists,
    sleeper: Callable[[float], None] = time.sleep,
) -> bool:
    """Wait until a drive root is no longer accessible after an eject request."""
    root = normalize_drive_path(path)
    if not root:
        return False
    deadline = time.monotonic() + max(0.0, float(timeout))
    while True:
        try:
            if not exists(root):
                return True
        except OSError:
            return True
        if time.monotonic() >= deadline:
            return False
        sleeper(max(0.01, float(poll_interval)))


def safe_eject_drive(path: str) -> str:
    if not IS_WINDOWS:
        raise RuntimeError("安全弹出仅支持 Windows。")
    drive = normalize_drive_path(path)[:2]
    if len(drive) != 2 or drive[1] != ":":
        raise ValueError(f"无效盘符：{path}")
    try:
        import win32com.client  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError("缺少 pywin32，无法调用 Windows Shell 安全弹出。") from exc

    try:
        shell = win32com.client.Dispatch("Shell.Application")
        namespace = shell.NameSpace(17)
        if namespace is None:
            raise RuntimeError("无法访问“此电脑”。")
        item = namespace.ParseName(drive)
        if item is None:
            raise RuntimeError(f"未找到驱动器 {drive}。")
        verbs = item.Verbs()
        count_attr = getattr(verbs, "Count", 0)
        count = int(count_attr() if callable(count_attr) else count_attr)
        for index in range(count):
            verb = verbs.Item(index)
            name_attr = getattr(verb, "Name", "")
            name = str(name_attr() if callable(name_attr) else name_attr).replace("&", "").strip().casefold()
            if any(token in name for token in ("eject", "弹出", "安全删除", "safely remove")):
                verb.DoIt()
                if wait_for_drive_removal(drive):
                    return drive
                raise RuntimeError(
                    f"已向 Windows 发送 {drive} 的安全弹出请求，但设备仍可访问。"
                    "请关闭占用文件后重试，在确认前请勿拔出。"
                )
        item.InvokeVerb("Eject")
        if wait_for_drive_removal(drive):
            return drive
        raise RuntimeError(
            f"已向 Windows 发送 {drive} 的安全弹出请求，但设备仍可访问。"
            "请关闭占用文件后重试，在确认前请勿拔出。"
        )
    except RuntimeError:
        raise
    except Exception as exc:
        hresult, message = _format_com_error(exc)
        unsigned_hresult = hresult & 0xFFFFFFFF if hresult is not None else None
        busy_hresult = {0x80070020, 0x80070021, 0x800700AA}
        lowered = message.casefold()
        if unsigned_hresult in busy_hresult or any(token in lowered for token in ("in use", "busy", "sharing violation", "正在使用", "占用")):
            raise RuntimeError(f"驱动器 {drive} 正在被程序使用，请关闭相关文件或窗口后重试。") from exc
        code_text = f"（HRESULT 0x{unsigned_hresult:08X}）" if unsigned_hresult is not None else ""
        raise RuntimeError(f"安全弹出 {drive} 失败{code_text}：{message}") from exc


# format_bytes, group_volumes, group_title, event_summary,
# anchored_window_geometry live in core.py.


# ---------------------------------------------------------------------------
# Qt GUI
# ---------------------------------------------------------------------------


if QT_AVAILABLE:
    SCALE = 0.88

    def px(value: float) -> int:
        return max(1, round(value * SCALE))


    class Theme:
        def __init__(self, requested: str, app: QApplication) -> None:
            resolved = requested
            if requested == "auto":
                resolved = "dark" if app.palette().color(QPalette.Window).lightness() < 128 else "light"
            self.requested = requested
            self.name = resolved
            if resolved == "dark":
                self.panel = "#202630"
                self.panel2 = "#2a323e"
                self.text = "#f7f9fc"
                self.muted = "#b5c0cf"
                self.border = "#3a4656"
                self.accent = "#75a7ff"
                self.accent_hover = "#91b9ff"
                self.progress = "#3b4654"
                self.icon_shell = "#2d3746"
                self.icon_socket = "#3d4a5c"
                self.shadow = QColor(0, 0, 0, 145)
            else:
                self.panel = "#fbfcff"
                self.panel2 = "#f2f5fa"
                self.text = "#111827"
                self.muted = "#687386"
                self.border = "#d9e2ee"
                self.accent = "#1769e0"
                self.accent_hover = "#0f5ed2"
                self.progress = "#e5ebf3"
                self.icon_shell = "#ffffff"
                self.icon_socket = "#dce6f4"
                self.shadow = QColor(15, 23, 42, 55)
            self.ok = "#34c759"
            self.warn = "#ffb020"
            self.error = "#ff5c5c"

        def stylesheet(self) -> str:
            return f"""
            QWidget {{ color:{self.text}; font-family:'Segoe UI','Microsoft YaHei UI',sans-serif; font-size:{px(13)}px; }}
            QWidget#toastWindow {{ background:{self.panel}; }}
            QFrame#root {{ background:{self.panel}; border:1px solid {self.border}; }}
            QFrame#volumeRow {{ background:{self.panel2}; border:1px solid {self.border}; }}
            QLabel#headline {{ font-size:{px(16)}px; font-weight:700; }}
            QLabel#muted, QLabel#summary, QLabel#capacity {{ color:{self.muted}; font-size:{px(12)}px; }}
            QLabel#rowTitle {{ font-weight:650; }}
            QPushButton {{ background:transparent; border:1px solid {self.border}; padding:{px(7)}px {px(13)}px; font-weight:650; }}
            QPushButton:hover {{ background:{self.panel2}; }}
            QPushButton#primary {{ background:{self.accent}; color:white; border-color:{self.accent}; }}
            QPushButton#primary:hover {{ background:{self.accent_hover}; border-color:{self.accent_hover}; }}
            QProgressBar {{ background:{self.progress}; border:0; }}
            QProgressBar::chunk {{ background:{self.accent}; }}
            QScrollArea {{ border:0; background:transparent; }}
            QScrollArea > QWidget > QWidget {{ background:transparent; }}
            """


    def usb_svg(theme: Theme, status: str = "usb") -> str:
        badge = {"add": theme.ok, "remove": theme.error, "error": theme.error, "change": theme.warn}.get(status, theme.accent)
        mark = ""
        if status == "add":
            mark = '<path d="M44 45l4 4 8-10" fill="none" stroke="white" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>'
        elif status in {"remove", "error"}:
            mark = '<path d="M44 40l10 10M54 40L44 50" fill="none" stroke="white" stroke-width="3" stroke-linecap="round"/>'
        elif status == "change":
            mark = '<path d="M43 45h12M50 39l6 6-6 6" fill="none" stroke="white" stroke-width="3" stroke-linecap="round"/>'
        return f"""
        <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
          <rect x="19" y="5" width="26" height="18" rx="5" fill="{theme.icon_socket}" stroke="{theme.border}" stroke-width="2"/>
          <rect x="24" y="9" width="5" height="7" rx="1.5" fill="{theme.accent}"/><rect x="35" y="9" width="5" height="7" rx="1.5" fill="{theme.accent}"/>
          <rect x="13" y="20" width="38" height="35" rx="10" fill="{theme.icon_shell}" stroke="{theme.border}" stroke-width="2"/>
          <path d="M32 26v16M24 34h16M24 34l-5-5M40 34l5-5" fill="none" stroke="{theme.accent}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
          <circle cx="50" cy="45" r="11" fill="{badge}" stroke="{theme.panel}" stroke-width="3"/>{mark}
        </svg>
        """


    class IconFactory:
        def __init__(self, app: QApplication) -> None:
            self.app = app
            self._cache: dict[tuple[str, str, int, int], QPixmap] = {}

        def pixmap(self, status: str, theme: Theme, logical_size: int) -> QPixmap:
            screen = self.app.primaryScreen()
            ratio = max(1.0, float(screen.devicePixelRatio()) if screen else 1.0)
            physical = max(1, round(logical_size * ratio))
            key = (theme.name, status, logical_size, physical)
            if key in self._cache:
                return self._cache[key]
            pixmap = QPixmap(physical, physical)
            pixmap.setDevicePixelRatio(ratio)
            pixmap.fill(Qt.transparent)
            painter = QPainter(pixmap)
            painter.setRenderHint(QPainter.Antialiasing)
            QSvgRenderer(QByteArray(usb_svg(theme, status).encode("utf-8"))).render(painter, QRectF(0, 0, logical_size, logical_size))
            painter.end()
            self._cache[key] = pixmap
            return pixmap

        def icon(self, theme: Theme) -> QIcon:
            icon = QIcon()
            for size in (16, 24, 32, 48, 64, 96, 128, 256):
                icon.addPixmap(self.pixmap("usb", theme, size))
            return icon


    class EventBridge(QObject):
        event_received = Signal(object)


    class SafeEjectSignals(QObject):
        """Bridge task results from a SafeEjectWorker back to the GUI thread."""
        started = Signal(str)
        succeeded = Signal(str)
        failed = Signal(str, str)


    class SafeEjectWorker(QThread):
        """Run ``safe_eject_drive`` off the GUI thread so the toast stays responsive."""

        def __init__(self, path: str) -> None:
            super().__init__()
            self._path = normalize_drive_path(path)
            self.signals = SafeEjectSignals()

        def run(self) -> None:
            self.signals.started.emit(self._path)
            try:
                drive = safe_eject_drive(self._path)
                self.signals.succeeded.emit(drive)
            except Exception as exc:
                self.signals.failed.emit(self._path, str(exc))


    class VolumeRow(QFrame):
        def __init__(self, group: Sequence[VolumeInfo], theme: Theme, icons: IconFactory, actions: "GuiActions") -> None:
            super().__init__()
            self.group = list(group)
            self.actions = actions
            self.setObjectName("volumeRow")
            self.setContextMenuPolicy(Qt.CustomContextMenu)
            self.setFocusPolicy(Qt.StrongFocus)
            self.setCursor(Qt.PointingHandCursor)
            self.setToolTip("点击整行或按回车键打开设备")
            self.customContextMenuRequested.connect(self._show_menu)
            layout = QGridLayout(self)
            layout.setContentsMargins(px(12), px(9), px(12), px(9))
            layout.setHorizontalSpacing(px(10))
            layout.setVerticalSpacing(px(4))

            icon = QLabel()
            icon.setFixedSize(px(34), px(34))
            icon.setPixmap(icons.pixmap("usb", theme, px(31)))
            layout.addWidget(icon, 0, 0, 2, 1)

            title_text = group_title(self.group)
            title = QLabel(title_text)
            title.setObjectName("rowTitle")
            title.setToolTip(title_text)
            layout.addWidget(title, 0, 1)

            paths = "、".join(item.path for item in self.group)
            subtitle = QLabel(f"{self.group[0].drive_type} · {paths}")
            subtitle.setObjectName("muted")
            subtitle.setTextInteractionFlags(Qt.TextSelectableByMouse)
            layout.addWidget(subtitle, 1, 1)

            self.open_button = QPushButton("打开")
            self.open_button.setObjectName("primary")
            self.open_button.setToolTip(f"打开 {self.group[0].path}")
            self.open_button.setMinimumWidth(px(68))
            self.open_button.setMinimumHeight(px(34))
            self.open_button.setFocusPolicy(Qt.NoFocus)  # row already focuses; don't double-tab-stop
            self.open_button.clicked.connect(lambda: actions.open_volume(self.group[0].path))
            layout.addWidget(self.open_button, 0, 2, 2, 1)

            total = sum(item.total or 0 for item in self.group) or None
            used = sum(item.used or 0 for item in self.group) if all(item.used is not None for item in self.group) else None
            free = sum(item.free or 0 for item in self.group) or None
            pct = precise_percent(used, total)
            pct_text = f" · {pct:.0f}%" if pct is not None else ""
            capacity = QLabel(f"容量 {format_bytes(total)} · 可用 {format_bytes(free)}{pct_text}")
            capacity.setObjectName("capacity")
            layout.addWidget(capacity, 2, 0, 1, 3)

            progress = QProgressBar()
            progress.setObjectName("capacityBar")
            progress.setTextVisible(False)
            progress.setFixedHeight(px(8))
            progress.setToolTip(progress_tooltip_text(total, used, free))
            if total and used is not None:
                progress.setRange(0, 100)
                progress.setValue(max(0, min(100, int(round((used / total) * 100)))))
            else:
                progress.setRange(0, 0)
            progress.setFocusPolicy(Qt.NoFocus)
            layout.addWidget(progress, 3, 0, 1, 3)

        def mouseReleaseEvent(self, event: Any) -> None:
            # Treat any release on the row (not on a child button or context menu)
            # as "open first volume".  Avoid double-firing when the primary button
            # is the actual target.
            if event.button() == Qt.LeftButton and not self._child_under_event(event).inherits("QPushButton"):
                self.actions.open_volume(self.group[0].path)
                event.accept()
                return
            super().mouseReleaseEvent(event)

        def keyPressEvent(self, event: Any) -> None:
            key = event.key()
            if key in (Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space):
                self.actions.open_volume(self.group[0].path)
                event.accept()
                return
            if key == Qt.Key_Menu:
                # Shift+F10 / Menu key — open context menu at row origin.
                self._show_menu(QPoint(8, 8))
                event.accept()
                return
            if key == Qt.Key_C and (event.modifiers() & Qt.ControlModifier):
                self.actions.copy_text("、".join(item.path for item in self.group))
                event.accept()
                return
            super().keyPressEvent(event)

        def _child_under_event(self, event: Any) -> Any:
            try:
                # Qt 6 deprecates QMouseEvent.globalPos(); prefer the new
                # floating-point globalPosition() API and keep a fallback for
                # older bindings / simple test stubs.
                if hasattr(event, "globalPosition"):
                    local = self.mapFromGlobal(event.globalPosition().toPoint())
                else:
                    local = self.mapFromGlobal(event.globalPos())
                widget = self.childAt(local)
                return widget if widget is not None else self
            except Exception:
                return self

        def focusInEvent(self, event: Any) -> None:
            super().focusInEvent(event)
            # The QSS rule for #volumeRow[focused="true"] would be cleaner, but
            # the existing stylesheet is auto-generated — toggle a dynamic
            # property that stylesheets can read.
            self.setProperty("focused", True)
            self.style().unpolish(self)
            self.style().polish(self)

        def focusOutEvent(self, event: Any) -> None:
            super().focusOutEvent(event)
            self.setProperty("focused", False)
            self.style().unpolish(self)
            self.style().polish(self)

        def _show_menu(self, position: QPoint) -> None:
            path = self.group[0].path
            paths = "、".join(item.path for item in self.group)
            menu = QMenu(self)
            menu.addAction("打开", lambda: self.actions.open_volume(path))
            menu.addAction("在资源管理器中显示", lambda: self.actions.reveal_volume(path))
            menu.addSeparator()
            menu.addAction("复制路径", lambda: self.actions.copy_text(paths))
            label = self.group[0].label
            if label:
                menu.addAction(f"复制卷标 ({label})", lambda: self.actions.copy_text(label))
            menu.addSeparator()
            menu.addAction("安全弹出", lambda: self.actions.eject_volume(path))
            menu.exec(self.mapToGlobal(position))


    class GuiActions:
        def __init__(
            self,
            app: QApplication,
            tray: Optional[QSystemTrayIcon],
            service: UsbMonitorService,
            recent: RecentVolumeManager,
        ) -> None:
            self.app = app
            self.tray = tray
            self.service = service
            self.recent = recent
            self.toast: Optional[ToastWindow] = None
            self._eject_in_flight: set[str] = set()
            self._eject_threads: dict[str, "SafeEjectWorker"] = {}
            self._last_operation_status = "就绪"

        def notify(self, message: str, warning: bool = False, timeout: int = 4000) -> None:
            if self.tray:
                icon = QSystemTrayIcon.Warning if warning else QSystemTrayIcon.Information
                self.tray.showMessage(APP_DISPLAY_NAME, message, icon, timeout)

        def open_volume(self, path: str) -> None:
            info = self.service.state.get(path)
            if info is None:
                self.notify(f"{path} 当前未连接。", warning=True)
                return
            try:
                open_path(path)
                self.recent.mark_opened(path, info)
                log_action("open_volume", {"path": path})
                if self.toast:
                    self.toast.hide()
            except Exception as exc:
                log_error("open_volume_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.notify(f"打开失败：{exc}", warning=True, timeout=6000)

        def reveal_volume(self, path: str) -> None:
            try:
                reveal_in_explorer(path)
                self.recent.mark_opened(path, self.service.state.get(path))
                log_action("reveal_volume", {"path": path})
            except Exception as exc:
                log_error("reveal_volume_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.notify(f"定位失败：{exc}", warning=True, timeout=6000)

        def copy_text(self, text: str) -> None:
            self.app.clipboard().setText(text)
            self.notify("路径已复制。", timeout=2500)

        @staticmethod
        def _eject_key(path: str) -> str:
            normalized = normalize_drive_path(path)
            return normalized[:2] if len(normalized) >= 2 and normalized[1] == ":" else ""

        def is_ejecting(self, path: str) -> bool:
            return self._eject_key(path) in self._eject_in_flight

        def operation_status(self) -> str:
            if self._eject_in_flight:
                drives = "、".join(sorted(self._eject_in_flight))
                return f"正在安全弹出 {drives}…"
            return self._last_operation_status

        def eject_volume(self, path: str) -> None:
            if not IS_WINDOWS:
                self.notify("安全弹出仅支持 Windows。", warning=True)
                return
            drive = self._eject_key(path)
            if not drive:
                self.notify(f"无效盘符：{path}", warning=True)
                return
            if drive in self._eject_in_flight:
                self._last_operation_status = f"{drive} 正在安全弹出，请勿重复操作"
                self._set_eject_status(drive, f"正在安全弹出 {drive}…")
                self.notify(self._last_operation_status, timeout=2500)
                return

            canonical_path = normalize_drive_path(path)
            self._eject_in_flight.add(drive)
            self._last_operation_status = f"正在安全弹出 {drive}…"
            self._set_eject_status(drive, self._last_operation_status)
            worker = SafeEjectWorker(canonical_path)
            worker.signals.started.connect(lambda _path, d=drive: self._set_eject_status(d, f"正在安全弹出 {d}…"))
            worker.signals.succeeded.connect(self._on_eject_succeeded)
            worker.signals.failed.connect(self._on_eject_failed)
            # QThread.finished is emitted after run() returns.  Keep the object
            # strongly referenced until then, remove only the matching worker,
            # and schedule Qt-side destruction on the GUI event loop.
            worker.finished.connect(partial(self._on_eject_finished, drive, worker))
            worker.finished.connect(worker.deleteLater)
            self._eject_threads[drive] = worker
            worker.start()
            log_action("safe_eject_requested", {"path": canonical_path, "drive": drive})

        def _on_eject_succeeded(self, path: str) -> None:
            drive = self._eject_key(path)
            self._eject_in_flight.discard(drive)
            self._set_eject_status(drive, None)
            self._last_operation_status = f"{drive} 已安全弹出，现在可以拔出设备"
            self.notify(self._last_operation_status, timeout=5000)
            # The native remove broadcast can lag behind Shell.  Schedule a
            # reconciliation pass so stale menu rows disappear promptly while
            # still allowing the normal WM_DEVICECHANGE path to confirm removal.
            QTimer.singleShot(350, self._request_post_eject_rescan)
            log_action("safe_eject_succeeded", {"path": normalize_drive_path(path), "drive": drive})

        def _request_post_eject_rescan(self) -> None:
            try:
                self.service.rescan()
            except Exception as exc:
                log_error("post_eject_rescan_failed", {"message": str(exc)}, exc_info=True)

        def _on_eject_failed(self, path: str, message: str) -> None:
            drive = self._eject_key(path)
            self._eject_in_flight.discard(drive)
            self._set_eject_status(drive, None)
            self._last_operation_status = f"{drive} 弹出失败：{message}"
            self.notify(f"安全弹出失败：{message}", warning=True, timeout=7000)
            log_error("safe_eject_failed", {"path": normalize_drive_path(path), "drive": drive, "message": message})

        def _on_eject_finished(self, drive: str, worker: "SafeEjectWorker") -> None:
            if self._eject_threads.get(drive) is worker:
                self._eject_threads.pop(drive, None)
            # Defensive cleanup if a future worker exits without a result signal.
            if drive in self._eject_in_flight and not worker.isRunning():
                self._eject_in_flight.discard(drive)
                self._set_eject_status(drive, None)
                self._last_operation_status = f"{drive} 弹出任务已结束，请确认设备状态"

        def _set_eject_status(self, drive: str, message: Optional[str]) -> None:
            """Push or clear a per-volume status line into the toast.

            We just store the latest message; the toast polls the dictionary
            on its own ``_countdown_timer`` so we don't need a Qt signal hop.
            """
            if not self.toast:
                return
            if message is None:
                self.toast._status_overrides.pop(drive, None)
            else:
                self.toast._status_overrides[drive] = message
            # Recompute the visible status: show the first active eject, or clear.
            self.toast._refresh_status()


    class ToastWindow(QWidget):
        """Non-activating, bottom-right anchored USB notification window.

        Windows Explorer can transiently restack or reposition topmost tool windows
        while system notifications animate.  The toast therefore keeps a stable
        work-area anchor for its whole visible lifetime, never calls ``raise_()``,
        and corrects unsolicited native moves without taking keyboard focus.

        UX behaviour:
          * Hovering pauses the auto-hide countdown; leaving resumes it.
          * Pressing Esc, clicking outside the toast, or losing focus hides it.
          * Collapsed state shows one global "打开U盘" button.
          * Expanded state hides the global button and shows per-row "打开" buttons.
          * While a safe-eject worker is running, the countdown label is replaced
            with a "正在弹出 X:\" status line so the user can see progress.
        """

        AUTO_HIDE_MS = 10_000
        MARGIN = 18
        _RESTORE_DELAYS_MS = (0, 50, 250)

        def __init__(self, app: QApplication, theme: Theme, icons: IconFactory, actions: GuiActions, topmost: bool, exit_on_close: bool = False) -> None:
            super().__init__(None)
            self.app = app
            self.theme = theme
            self.icons = icons
            self.actions = actions
            self.keep_topmost = topmost
            self.exit_on_close = exit_on_close
            self.events: deque[UsbEvent] = deque(maxlen=20)
            self.volumes: tuple[VolumeInfo, ...] = ()
            self.expanded = False
            self.hide_timer = QTimer(self)
            self.hide_timer.setSingleShot(True)
            self.hide_timer.timeout.connect(self._on_auto_hide)
            # Pause / resume state for hover-to-pause.
            self._is_paused = False
            self._remaining_ms = self.AUTO_HIDE_MS
            self._countdown_timer = QTimer(self)
            self._countdown_timer.setInterval(500)
            self._countdown_timer.timeout.connect(self._refresh_countdown)
            # Status text overrides (used by safe-eject worker to surface progress).
            # Map drive-letter ("E:") -> human-readable status.  Most recent wins.
            self._status_overrides: dict[str, str] = {}
            self._status_override: Optional[str] = None  # legacy single-line field
            # Click-outside-to-close is implemented with QApplication.focusChanged,
            # while a local event filter is installed only on the toast subtree to
            # reset the countdown after direct user interaction.
            self._outside_filter_installed = False

            self._anchor_pos: Optional[QPoint] = None
            self._anchor_screen_key: Optional[str] = None
            self._anchor_work_area: Optional[tuple[int, int, int, int]] = None
            self._stable_work_areas: dict[str, tuple[int, int, int, int]] = {}
            self._internal_geometry_change = False
            self._reposition_pending = False
            self._connected_screens: set[int] = set()
            self._configure_window()
            self._build_ui()
            self._connect_screen_signals()
            self.apply_theme(theme)
            self._install_interaction_filters()
            self._install_outside_click_filter()

        def _configure_window(self) -> None:
            flags = Qt.Tool | Qt.FramelessWindowHint
            if self.keep_topmost:
                flags |= Qt.WindowStaysOnTopHint
            if hasattr(Qt, "WindowDoesNotAcceptFocus"):
                flags |= Qt.WindowDoesNotAcceptFocus
            self.setWindowFlags(flags)
            self.setObjectName("toastWindow")
            # Keep the toast fully opaque and rectangular.  This avoids the
            # translucent/layered-window path that caused repaint warnings on
            # Windows, and intentionally removes all rounded-corner clipping.
            self.setAttribute(Qt.WA_TranslucentBackground, False)
            if hasattr(Qt, "WA_ShowWithoutActivating"):
                self.setAttribute(Qt.WA_ShowWithoutActivating, True)
            self.setFocusPolicy(Qt.NoFocus)
            self.setWindowTitle(APP_DISPLAY_NAME)

        def _build_ui(self) -> None:
            outer = QVBoxLayout(self)
            outer.setContentsMargins(0, 0, 0, 0)
            self.root = QFrame()
            self.root.setObjectName("root")
            outer.addWidget(self.root)
            # Avoid QGraphicsDropShadowEffect on Windows top-level toast windows.
            # It expands the repaint dirty rectangle outside the window bounds and
            # is the usual trigger for the console spam shown in the user's log.
            if not IS_WINDOWS:
                shadow = QGraphicsDropShadowEffect(self.root)
                shadow.setBlurRadius(px(28))
                shadow.setOffset(0, px(8))
                self.root.setGraphicsEffect(shadow)

            layout = QVBoxLayout(self.root)
            layout.setContentsMargins(px(16), px(14), px(16), px(14))
            layout.setSpacing(px(9))
            header = QHBoxLayout()
            self.icon_label = QLabel()
            self.icon_label.setFixedSize(px(34), px(34))
            header.addWidget(self.icon_label)
            titles = QVBoxLayout()
            self.headline = QLabel("USB 设备监控")
            self.headline.setObjectName("headline")
            self.subtitle = QLabel("等待 USB 设备事件")
            self.subtitle.setObjectName("muted")
            titles.addWidget(self.headline)
            titles.addWidget(self.subtitle)
            header.addLayout(titles, 1)
            self.count = QLabel()
            self.count.setObjectName("muted")
            header.addWidget(self.count)
            layout.addLayout(header)

            self.summary = QLabel("插入 USB 存储设备后会显示可打开位置。")
            self.summary.setObjectName("summary")
            self.summary.setWordWrap(True)
            layout.addWidget(self.summary)

            self.status_label = QLabel("")
            self.status_label.setObjectName("status")
            self.status_label.setWordWrap(True)
            self.status_label.setVisible(False)
            layout.addWidget(self.status_label)

            self.scroll = QScrollArea()
            self.scroll.setWidgetResizable(True)
            self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
            self.rows_widget = QWidget()
            self.rows_layout = QVBoxLayout(self.rows_widget)
            self.rows_layout.setContentsMargins(0, 0, 0, 0)
            self.rows_layout.setSpacing(px(8))
            self.scroll.setWidget(self.rows_widget)
            self._enable_touch_scrolling()
            layout.addWidget(self.scroll)

            buttons = QHBoxLayout()
            self.expand_button = QPushButton("展开")
            self.expand_button.setToolTip("显示 / 折叠所有设备")
            self.expand_button.clicked.connect(self.toggle_expanded)
            self.close_button = QPushButton("退出" if self.exit_on_close else "关闭")
            self.close_button.setToolTip("Esc 或点击此处关闭")
            self.close_button.setShortcut("Esc")
            self.close_button.clicked.connect(self.app.quit if self.exit_on_close else self.hide)
            self.open_button = QPushButton("打开U盘")
            self.open_button.setObjectName("primary")
            self.open_button.setToolTip("回车 / Enter 键也可触发")
            self.open_button.setMinimumWidth(px(118))
            self.open_button.setMinimumHeight(px(42))
            self.open_button.setDefault(True)
            self.open_button.setAutoDefault(True)
            self.open_button.clicked.connect(self.open_usb)
            buttons.addWidget(self.expand_button)
            buttons.addWidget(self.close_button)
            buttons.addStretch(1)
            self.countdown_label = QLabel("")
            self.countdown_label.setObjectName("muted")
            buttons.addWidget(self.countdown_label)
            buttons.addWidget(self.open_button)
            layout.addLayout(buttons)

        def _qscroller_gesture(self, name: str) -> Any:
            if QScroller is None:
                return None
            enum = getattr(QScroller, "ScrollerGestureType", None)
            if enum is not None and hasattr(enum, name):
                return getattr(enum, name)
            return getattr(QScroller, name, None)

        def _install_interaction_filters(self) -> None:
            self._install_widget_event_filter(self)

        def _install_widget_event_filter(self, widget: Any) -> None:
            targets = [widget]
            try:
                targets.extend(widget.findChildren(QObject))
            except Exception:
                pass
            for target in targets:
                try:
                    target.installEventFilter(self)
                except Exception:
                    pass

        def _enable_touch_scrolling(self) -> None:
            """Enable finger/pen kinetic scrolling inside the expanded device list.

            QScrollArea supports scroll bars by default; this adds touch-friendly
            kinetic dragging on the viewport while keeping row buttons clickable.
            The left-mouse fallback is intentionally enabled only on the viewport
            so Windows touch panels that synthesize mouse drags can still scroll.
            """
            self.scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
            for widget in (self.scroll, self.scroll.viewport(), self.rows_widget):
                try:
                    widget.setAttribute(Qt.WA_AcceptTouchEvents, True)
                except Exception:
                    pass

            if QScroller is None:
                return

            target = self.scroll.viewport()
            for gesture_name in ("TouchGesture", "LeftMouseButtonGesture"):
                gesture = self._qscroller_gesture(gesture_name)
                if gesture is None:
                    continue
                try:
                    QScroller.grabGesture(target, gesture)
                except Exception:
                    # Touch scrolling is an enhancement; failure must not break
                    # the notification window.
                    pass

        def _connect_screen_signals(self) -> None:
            for screen in self.app.screens():
                self._on_screen_added(screen)
            self.app.screenAdded.connect(self._on_screen_added)
            self.app.screenRemoved.connect(self._schedule_reposition)

        def _on_screen_added(self, screen: Any) -> None:
            identity = id(screen)
            if identity in self._connected_screens:
                return
            self._connected_screens.add(identity)
            for signal_name in ("availableGeometryChanged", "geometryChanged", "logicalDotsPerInchChanged"):
                signal = getattr(screen, signal_name, None)
                if signal is not None:
                    signal.connect(self._schedule_reposition)

        @staticmethod
        def _screen_key(screen: Any) -> str:
            try:
                name = str(screen.name() or "").strip()
            except Exception:
                name = ""
            return name or f"screen:{id(screen)}"

        @staticmethod
        def _rect_tuple(rect: Any) -> tuple[int, int, int, int]:
            return int(rect.x()), int(rect.y()), int(rect.width()), int(rect.height())

        @staticmethod
        def _rect_contains(outer: tuple[int, int, int, int], inner: tuple[int, int, int, int]) -> bool:
            ox, oy, ow, oh = outer
            ix, iy, iw, ih = inner
            return iw > 0 and ih > 0 and ix >= ox and iy >= oy and ix + iw <= ox + ow and iy + ih <= oy + oh

        def _stable_work_area(self, screen: Any, refresh: bool = False) -> tuple[int, int, int, int]:
            """Return a validated work area and reject transient full-screen expansion.

            ``QScreen.availableGeometry()`` is normally correct and excludes the
            taskbar.  During Explorer restacking it can briefly report the complete
            monitor geometry.  Once a taskbar-excluding rectangle has been observed,
            this method keeps it until a genuinely different valid work area arrives.
            """
            key = self._screen_key(screen)
            full = self._rect_tuple(screen.geometry())
            candidate = self._rect_tuple(screen.availableGeometry())
            previous = self._stable_work_areas.get(key)

            valid = self._rect_contains(full, candidate)
            if valid and full[2] > 0 and full[3] > 0:
                valid = candidate[2] >= min(240, full[2]) and candidate[3] >= min(160, full[3])
            if not valid:
                return previous or full

            candidate_is_full = candidate == full
            previous_reserved_space = previous is not None and previous != full
            if candidate_is_full and previous_reserved_space:
                # A normal Windows notification must not temporarily erase the
                # taskbar reservation.  Keep the last known-good work rectangle.
                return previous

            if previous is not None and not refresh:
                return previous

            self._stable_work_areas[key] = candidate
            return candidate

        def _screen_by_key(self, key: Optional[str]) -> Any:
            if not key:
                return None
            for screen in self.app.screens():
                if self._screen_key(screen) == key:
                    return screen
            return None

        def _preferred_screen(self) -> Any:
            screen = self.app.screenAt(QCursor.pos())
            if screen is not None:
                return screen
            handle = self.windowHandle()
            if handle is not None and handle.screen() is not None:
                return handle.screen()
            return self.app.primaryScreen()

        def _position_on_screen(self, screen: Any, refresh_work_area: bool) -> None:
            if screen is None:
                return
            key = self._screen_key(screen)
            if not refresh_work_area and self._anchor_screen_key == key and self._anchor_work_area is not None:
                work = self._anchor_work_area
            else:
                work = self._stable_work_area(screen, refresh=refresh_work_area)

            x, y, target_width, target_height = anchored_window_geometry(
                work,
                (self.width(), self.height()),
                px(self.MARGIN),
            )

            self._internal_geometry_change = True
            try:
                if target_width != self.width() or target_height != self.height():
                    self.resize(target_width, target_height)
                self._anchor_pos = QPoint(x, y)
                self._anchor_screen_key = key
                self._anchor_work_area = work
                self.move(self._anchor_pos)
            finally:
                self._internal_geometry_change = False

        def _schedule_anchor_restore(self) -> None:
            for delay in self._RESTORE_DELAYS_MS:
                QTimer.singleShot(delay, self._enforce_anchor)

        def _enforce_anchor(self) -> None:
            if not self.isVisible() or self._internal_geometry_change:
                return
            screen = self._screen_by_key(self._anchor_screen_key)
            if screen is None:
                self._position_on_screen(self._preferred_screen(), refresh_work_area=True)
            elif self._anchor_pos is not None and self.pos() != self._anchor_pos:
                self._internal_geometry_change = True
                try:
                    self.move(self._anchor_pos)
                finally:
                    self._internal_geometry_change = False
            self._apply_native_z_order()

        def _schedule_reposition(self, *_args: Any) -> None:
            if not self.isVisible() or self._reposition_pending:
                return
            self._reposition_pending = True
            QTimer.singleShot(60, self._reposition_after_environment_change)

        def _reposition_after_environment_change(self) -> None:
            self._reposition_pending = False
            if not self.isVisible():
                return
            screen = self._screen_by_key(self._anchor_screen_key) or self._preferred_screen()
            self._position_on_screen(screen, refresh_work_area=True)
            self._schedule_anchor_restore()

        def _apply_native_z_order(self) -> None:
            """Apply topmost/not-topmost without activating the window on Windows."""
            if not IS_WINDOWS or user32 is None or not self.isVisible():
                return
            try:
                user32.SetWindowPos.argtypes = [
                    wintypes.HWND,
                    wintypes.HWND,
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_int,
                    ctypes.c_int,
                    wintypes.UINT,
                ]
                user32.SetWindowPos.restype = wintypes.BOOL
                hwnd_after = wintypes.HWND(-1 if self.keep_topmost else -2)  # HWND_TOPMOST / HWND_NOTOPMOST
                flags = 0x0001 | 0x0002 | 0x0010 | 0x0200 | 0x0400  # NOSIZE|NOMOVE|NOACTIVATE|NOOWNERZORDER|NOSENDCHANGING
                user32.SetWindowPos(wintypes.HWND(int(self.winId())), hwnd_after, 0, 0, 0, 0, flags)
            except (AttributeError, OSError, TypeError, ValueError):
                # Qt flags remain the portable fallback; positioning must never
                # make the notification path fail.
                return

        def consume(self, event: UsbEvent) -> None:
            self.events.appendleft(event)
            self.volumes = event.snapshot
            self.actions.recent.remember_snapshot(event.snapshot)
            self.refresh()
            if not event.display:
                return
            if self.isVisible():
                # Keep one stable toast instance for bursty/multi-device events;
                # just refresh content and restart the countdown instead of
                # calling show()/move()/resize repeatedly.
                self._is_paused = False
                self._remaining_ms = self.AUTO_HIDE_MS
                self.hide_timer.start(self.AUTO_HIDE_MS)
                self._countdown_timer.start()
                self._refresh_countdown()
                QTimer.singleShot(0, self._reposition_after_resize)
            else:
                self.show_toast()

        def refresh_from_state(self) -> None:
            self.volumes = self.actions.service.state.snapshot()
            self.refresh()

        def refresh(self) -> None:
            while self.rows_layout.count():
                item = self.rows_layout.takeAt(0)
                if item.widget():
                    item.widget().deleteLater()
            collapsed_groups = group_volumes(self.volumes)
            latest = self.events[0] if self.events else None
            if latest and latest.changed_paths and collapsed_groups:
                changed = {normalize_drive_path(path) for path in latest.changed_paths}
                indexed_groups = list(enumerate(collapsed_groups))
                indexed_groups.sort(
                    key=lambda pair: (
                        0 if any(normalize_drive_path(item.path) in changed for item in pair[1]) else 1,
                        pair[0],
                    )
                )
                collapsed_groups = [group for _, group in indexed_groups]

            # Collapsed view is device-oriented: partitions from the same
            # physical USB disk are grouped into one compact row.  Expanded
            # view is action-oriented: each partition/drive letter gets its own
            # row and its own "打开" button, so multi-partition devices can be
            # opened independently.
            if self.expanded:
                changed = {normalize_drive_path(path) for path in latest.changed_paths} if latest and latest.changed_paths else set()
                volumes = list(self.volumes)
                if changed:
                    volumes.sort(key=lambda item: (0 if normalize_drive_path(item.path) in changed else 1, item.path))
                groups = [[item] for item in volumes]
            else:
                groups = collapsed_groups
            self.headline.setText(event_summary(latest).split("：", 1)[0] if latest else "USB 设备监控")
            status = latest.action if latest else "usb"
            self.icon_label.setPixmap(self.icons.pixmap(status, self.theme, px(32)))
            if groups:
                if self.expanded:
                    subtitle = f"{len(self.volumes)} 个分区/卷 · 可单独打开"
                else:
                    subtitle = f"{len(collapsed_groups)} 个设备 · {len(self.volumes)} 个卷"
            else:
                subtitle = f"最近事件：{latest.timestamp_local}" if latest else "等待 USB 设备事件"
            self.subtitle.setText(subtitle)
            self.summary.setText(event_summary(latest) if latest else "插入 USB 存储设备后会显示可打开位置。")
            self.count.setText(f"{len(groups)} 个" if groups else "")
            shown = groups if self.expanded else groups[:1]
            # Create each VolumeRow and toggle its per-row open button based on the expanded state.
            for group in shown:
                row = VolumeRow(group, self.theme, self.icons, self.actions)
                self._install_widget_event_filter(row)
                # Show the row's own open button only when expanded. When collapsed,
                # the toast displays a single global open button instead.
                if hasattr(row, "open_button"):
                    row.open_button.setVisible(self.expanded)
                self.rows_layout.addWidget(row)
            self.rows_layout.addStretch(1)
            self.scroll.setVisible(bool(shown))
            self.scroll.setMinimumHeight(px(105) if shown else 0)
            self.scroll.setMaximumHeight(px(285) if self.expanded else px(115))
            self.expand_button.setVisible(len(self.volumes) > 1 or len(collapsed_groups) > 1)
            self.expand_button.setText("折叠" if self.expanded else "展开")
            # Show the global open button only when not expanded. When expanded,
            # each row exposes its own open button, so the global button hides.
            self.open_button.setVisible(bool(self.volumes) and not self.expanded)
            self.adjustSize()
            # Fixed width + capped height prevents resize/repaint storms when
            # several devices are attached/removed in one burst.
            target_height = px(430) if self.expanded else px(230)
            self.resize(px(510), min(max(self.sizeHint().height(), px(205)), target_height))
            if self.isVisible() and not self._internal_geometry_change:
                QTimer.singleShot(0, self._reposition_after_resize)

        def _reposition_after_resize(self) -> None:
            if not self.isVisible():
                return
            screen = self._screen_by_key(self._anchor_screen_key) or self._preferred_screen()
            self._position_on_screen(screen, refresh_work_area=True)
            self._schedule_anchor_restore()

        def show_toast(self) -> None:
            screen = self._preferred_screen()
            self._position_on_screen(screen, refresh_work_area=True)
            was_visible = self.isVisible()
            self.show()
            self._apply_native_z_order()
            self._schedule_anchor_restore()
            # Reset pause state for the new event.
            self._is_paused = False
            self._remaining_ms = self.AUTO_HIDE_MS
            self.hide_timer.start(self.AUTO_HIDE_MS)
            self._countdown_timer.start()
            self._refresh_countdown()
            log_action(
                "toast_shown",
                {
                    "volume_count": len(self.volumes),
                    "topmost": self.keep_topmost,
                    "screen": self._anchor_screen_key,
                    "work_area": self._anchor_work_area,
                    "first_show": not was_visible,
                },
            )

        def _on_auto_hide(self) -> None:
            """Hide timer callback.  When paused, do nothing — the timer is stopped
            so this only fires if the user explicitly resumes by leaving the toast."""
            if self._is_paused:
                return
            self.hide()

        def enterEvent(self, event: Any) -> None:
            super().enterEvent(event)
            if self._is_paused or not self.isVisible():
                return
            remaining = self.hide_timer.remainingTime()
            if remaining <= 0:
                remaining = self.AUTO_HIDE_MS
            self._remaining_ms = remaining
            self.hide_timer.stop()
            self._is_paused = True
            self._refresh_countdown()
            log_action("toast_paused", {"remaining_ms": self._remaining_ms})

        def leaveEvent(self, event: Any) -> None:
            super().leaveEvent(event)
            if not self._is_paused or not self.isVisible():
                return
            self._is_paused = False
            self.hide_timer.start(self._remaining_ms)
            self._refresh_countdown()
            log_action("toast_resumed", {"remaining_ms": self._remaining_ms})

        def _refresh_countdown(self) -> None:
            if self._status_override is not None:
                self.countdown_label.setText("")
                self.status_label.setText(self._status_override)
                self.status_label.setVisible(True)
                return
            self.status_label.setVisible(False)
            if not self.isVisible():
                self.countdown_label.setText("")
                return
            if self._is_paused:
                self.countdown_label.setText("已暂停")
                return
            remaining = self.hide_timer.remainingTime() if self.hide_timer.isActive() else 0
            if remaining < 0:
                remaining = 0
            self.countdown_label.setText(countdown_label(remaining))

        def set_status(self, message: Optional[str], drive: Optional[str] = None) -> None:
            """Show a transient status line above the volume rows.

            Used by the safe-eject worker to surface "正在弹出 X:\" progress.
            Pass ``None`` to clear the override and resume the countdown.
            When ``drive`` is provided, the message is keyed by drive letter so
            multiple concurrent ejects can be displayed; otherwise it acts as a
            single global override for legacy callers.
            """
            if drive is None:
                self._status_override = message
                self._refresh_countdown()
                return
            if message is None:
                self._status_overrides.pop(drive, None)
            else:
                self._status_overrides[drive] = message
            self._refresh_status()

        def _refresh_status(self) -> None:
            """Re-evaluate the visible status from the ``_status_overrides`` map.

            GuiActions calls this whenever a worker starts or finishes.  When
            multiple ejects are in flight we show all of them separated by ' / '.
            """
            if not self._status_overrides:
                self._status_override = None
            elif len(self._status_overrides) == 1:
                self._status_override = next(iter(self._status_overrides.values()))
            else:
                self._status_override = " · ".join(self._status_overrides.values())
            self._refresh_countdown()

        def keyPressEvent(self, event: Any) -> None:
            if event.key() == Qt.Key_Escape:
                self.hide()
                event.accept()
                return
            super().keyPressEvent(event)

        def _install_outside_click_filter(self) -> None:
            """Hide the toast when focus moves away, without a global event filter."""
            if self._outside_filter_installed:
                return
            self._outside_filter_installed = True
            try:
                self.app.focusChanged.connect(self._on_focus_changed)
            except Exception:
                # Some test stubs do not expose focusChanged. In that case the
                # toast still works; it just will not auto-hide on focus changes.
                pass

        def _is_toast_widget(self, widget: Any) -> bool:
            current = widget
            while current is not None:
                if current is self:
                    return True
                parent = getattr(current, "parentWidget", None)
                if parent is None:
                    break
                try:
                    current = parent()
                except Exception:
                    break
            return False

        def _on_focus_changed(self, old: Any, new: Any) -> None:
            if not self.isVisible():
                return
            if new is None or not self._is_toast_widget(new):
                self.hide()

        def eventFilter(self, watched: Any, event: Any) -> bool:
            et = event.type()
            user_event_types = {
                QEvent.MouseButtonPress,
                QEvent.MouseButtonRelease,
                QEvent.Wheel,
                QEvent.TouchBegin,
                QEvent.TouchUpdate,
                QEvent.TouchEnd,
                QEvent.KeyPress,
            }
            if self.isVisible() and self._is_toast_widget(watched) and et in user_event_types and not self._is_paused:
                self.hide_timer.start(self.AUTO_HIDE_MS)
                self._remaining_ms = self.AUTO_HIDE_MS
                self._refresh_countdown()

            # WindowDeactivate fires when the user clicks another app or empty
            # desktop. Hide so the toast doesn't get stranded.
            if et == QEvent.WindowDeactivate and watched is not self and self.isVisible():
                try:
                    if hasattr(watched, "isWindow") and watched.isWindow():
                        self.hide()
                except Exception:
                    pass
            return super().eventFilter(watched, event)

        def closeEvent(self, event: Any) -> None:
            self.hide_timer.stop()
            self._countdown_timer.stop()
            super().closeEvent(event)

        def hide(self) -> None:    # type: ignore[override]
            """Hide immediately and reset state.

            The old fade-out looked nicer but required a layered window on
            Windows.  Stability is more important here because the toast is
            updated repeatedly when several USB devices arrive at once.
            """
            if not self.isVisible():
                super().hide()
                return
            self._is_paused = False
            self._status_override = None
            self._status_overrides.clear()
            self._countdown_timer.stop()
            self.hide_timer.stop()
            super().hide()

        def toggle_expanded(self) -> None:
            self.expanded = not self.expanded
            self.refresh()
            if self.isVisible():
                self._reposition_after_resize()

        def open_usb(self) -> None:
            if self.volumes:
                self.actions.open_volume(self.volumes[0].path)

        def set_topmost(self, enabled: bool) -> None:
            visible = self.isVisible()
            self.keep_topmost = bool(enabled)
            self._configure_window()
            if visible:
                self.show_toast()

        def apply_theme(self, theme: Theme) -> None:
            self.theme = theme
            self.setStyleSheet(theme.stylesheet())
            self.setWindowIcon(self.icons.icon(theme))
            effect = self.root.graphicsEffect()
            if isinstance(effect, QGraphicsDropShadowEffect):
                effect.setColor(theme.shadow)
            status = self.events[0].action if self.events else "usb"
            self.icon_label.setPixmap(self.icons.pixmap(status, theme, px(32)))
            self.refresh()

        def moveEvent(self, event: Any) -> None:
            super().moveEvent(event)
            if self.isVisible() and not self._internal_geometry_change and self._anchor_pos is not None and self.pos() != self._anchor_pos:
                QTimer.singleShot(0, self._enforce_anchor)

        def resizeEvent(self, event: Any) -> None:
            super().resizeEvent(event)
            if self.isVisible() and not self._internal_geometry_change:
                QTimer.singleShot(0, self._reposition_after_resize)

        def showEvent(self, event: Any) -> None:
            super().showEvent(event)
            self._schedule_anchor_restore()

        def hideEvent(self, event: Any) -> None:
            super().hideEvent(event)
            self.hide_timer.stop()
            self._countdown_timer.stop()
            self._is_paused = False
            self._status_override = None
            self._status_overrides.clear()
            self._anchor_pos = None
            self._anchor_screen_key = None
            self._anchor_work_area = None


    class TrayReceiver(QObject):
        def __init__(self, tray: QSystemTrayIcon, actions: GuiActions) -> None:
            super().__init__()
            self.tray = tray
            self.actions = actions

        def consume(self, event: UsbEvent) -> None:
            self.actions.recent.remember_snapshot(event.snapshot)
            if event.display:
                icon = QSystemTrayIcon.Warning if event.action == "error" else QSystemTrayIcon.Information
                self.tray.showMessage(APP_DISPLAY_NAME, event_summary(event), icon, 7000)

        def refresh_from_state(self) -> None:
            return


    class TrayMenuController(QObject):
        """System-tray controller with explicit left/right click split.

        Left click opens the USB-device menu directly.  Right click opens a
        compact application menu without the device list.  Qt exposes this via
        QSystemTrayIcon.activated(reason): Trigger is the normal click and
        Context is the platform context-menu request.
        """

        THEME_LABELS = {"auto": "跟随系统", "light": "浅色", "dark": "深色"}
        LOG_LABELS = {LogMode.OFF: "关闭", LogMode.REDACTED: "脱敏", LogMode.RAW: "明文"}

        def __init__(
            self,
            tray: QSystemTrayIcon,
            app: QApplication,
            config: AppConfig,
            store: ConfigStore,
            startup: StartupManager,
            service: UsbMonitorService,
            actions: GuiActions,
            receiver: Any,
            icons: IconFactory,
            theme: Theme,
        ) -> None:
            super().__init__()
            self.tray = tray
            self.app = app
            self.config = config
            self.store = store
            self.startup = startup
            self.service = service
            self.actions = actions
            self.receiver = receiver
            self.icons = icons
            self.theme = theme
            self._rescan_in_flight = False
            self._last_device_menu_popup = 0.0

            # Left-click menu: this is the original "USB 设备" submenu, promoted
            # to its own top-level popup so device operations are one click away.
            self.device_menu = QMenu("USB 设备")
            self.volume_menu = self.device_menu
            self.volume_menu.aboutToShow.connect(self.refresh_volume_menu)

            # Right-click menu: keep it short and settings-oriented.  The device
            # list is intentionally absent because it now belongs to left click.
            self.menu = QMenu()
            self.toast_action: Optional[QAction] = None
            self.show_toast_action: Optional[QAction] = None  # compatibility alias
            self.hide_toast_action: Optional[QAction] = None  # no separate duplicate item now
            self._build_right_menu()

            # Re-enable the menu item by polling the reconciler's threading.Event.
            # Never monkey-patch Event.wait(): external callers must retain the
            # standard threading.Event contract.
            self._poll_rescan_timer = QTimer(self)
            self._poll_rescan_timer.setInterval(150)
            self._poll_rescan_timer.timeout.connect(self._poll_rescan_status)
            self._poll_rescan_timer.start()

            self.menu.aboutToShow.connect(self.update_dynamic_state)
            tray.setContextMenu(self.menu)
            tray.activated.connect(self._on_tray_activated)
            self.update_dynamic_state()

        def _build_right_menu(self) -> None:
            self.task_status_action = self.menu.addAction("状态：就绪")
            self.task_status_action.setEnabled(False)
            self.task_status_action.setToolTip("最近一次设备操作状态；即使系统通知被关闭也可在此确认结果")
            self.menu.addSeparator()
            if isinstance(self.receiver, ToastWindow):
                self.toast_action = self.menu.addAction("显示通知", self._toggle_toast)
                self.toast_action.setToolTip("显示或隐藏当前 USB 通知浮层")
                self.show_toast_action = self.toast_action
            self.rescan_action = self.menu.addAction("重新扫描", self._rescan_clicked)
            self.rescan_action.setToolTip("手动重新扫描所有盘符（不会被狂点滥用）")

            self.menu.addSeparator()
            settings = self.menu.addMenu("设置")
            self._add_theme_menu(settings)
            self._add_log_menu(settings)
            settings.addSeparator()
            self.topmost_action = QAction("通知置顶", settings)
            self.topmost_action.setCheckable(True)
            self.topmost_action.setChecked(self.config.topmost)
            self.topmost_action.toggled.connect(self.apply_topmost)
            settings.addAction(self.topmost_action)
            enabled_hooks = sum(1 for rule in normalize_hook_rules(self.config.hooks) if rule.get("enabled"))
            self.hooks_status_action = settings.addAction(f"自动化规则：{enabled_hooks} 条启用")
            self.hooks_status_action.setEnabled(False)
            self.hooks_status_action.setToolTip("规则保存在 config.json；可通过 工具 → 打开程序数据目录 编辑")
            self.startup_action = QAction("随系统启动", settings)
            self.startup_action.setCheckable(True)
            self.startup_action.setChecked(bool(self.startup.status()["enabled"]))
            self.startup_action.toggled.connect(self.toggle_startup)
            settings.addAction(self.startup_action)

            tools = self.menu.addMenu("工具")
            tools.addAction("打开日志目录", self.open_logs)
            tools.addAction("立即清空日志", self.reset_logs)
            tools.addAction("打开程序数据目录", self.open_app_data)
            tools.addAction("检查/修复开机启动", self.repair_startup)

            self.menu.addSeparator()
            self.menu.addAction("退出", self.app.quit)

        def _add_log_menu(self, parent_menu: Optional[QMenu] = None) -> QMenu:
            parent = parent_menu if parent_menu is not None else self.menu
            menu = parent.addMenu("日志")
            group = QActionGroup(menu)
            group.setExclusive(True)
            for mode, label in ((LogMode.OFF, "关闭日志"), (LogMode.REDACTED, "脱敏日志"), (LogMode.RAW, "明文日志")):
                action = QAction(label, group)
                action.setCheckable(True)
                action.setChecked(self.config.log_mode == mode)
                action.triggered.connect(partial(self.apply_log_mode, mode))
                menu.addAction(action)
            menu.addSeparator()
            reset_action = QAction("每次启动时清空日志", menu)
            reset_action.setCheckable(True)
            reset_action.setChecked(self.config.reset_logs_on_start)
            reset_action.toggled.connect(self.apply_reset_on_start)
            menu.addAction(reset_action)
            return menu

        def _add_theme_menu(self, parent_menu: Optional[QMenu] = None) -> QMenu:
            parent = parent_menu if parent_menu is not None else self.menu
            menu = parent.addMenu("主题")
            group = QActionGroup(menu)
            group.setExclusive(True)
            for key, label in self.THEME_LABELS.items():
                action = QAction(label, group)
                action.setCheckable(True)
                action.setChecked(self.config.theme == key)
                action.triggered.connect(partial(self.apply_theme, key))
                menu.addAction(action)
            return menu

        def refresh_volume_menu(self) -> None:
            self.volume_menu.clear()
            volumes = self.service.state.snapshot()
            connected = {normalize_drive_path(info.path): info for info in volumes}
            if not volumes:
                action = self.volume_menu.addAction("当前没有检测到 USB 存储设备")
                action.setEnabled(False)
            else:
                for info in volumes:
                    submenu = self.volume_menu.addMenu(info.title)
                    submenu.addAction("打开", partial(self.actions.open_volume, info.path))
                    submenu.addAction("在资源管理器中显示", partial(self.actions.reveal_volume, info.path))
                    eject = submenu.addAction("安全弹出", partial(self.actions.eject_volume, info.path))
                    if self.actions.is_ejecting(info.path):
                        eject.setText("正在安全弹出…")
                        eject.setEnabled(False)

            recent = normalize_recent_records(self.config.recent_volumes)
            if recent:
                self.volume_menu.addSeparator()
                recent_menu = self.volume_menu.addMenu("最近使用")
                for record in recent:
                    path = normalize_drive_path(record.get("path"))
                    title = str(record.get("title") or display_name_for_path(path))
                    item_menu = recent_menu.addMenu(f"{title} · {path}")
                    current = connected.get(path)
                    if current is not None:
                        item_menu.addAction("打开", partial(self.actions.open_volume, path))
                        item_menu.addAction("在资源管理器中显示", partial(self.actions.reveal_volume, path))
                    else:
                        stamp = str(record.get("last_seen_local") or record.get("last_seen_utc") or "未知时间")
                        stamp = stamp.replace("T", " ")[:19]
                        offline = item_menu.addAction(f"当前未连接 · 上次使用 {stamp}")
                        offline.setEnabled(False)
                    item_menu.addAction("复制盘符", partial(self.actions.copy_text, path))
                recent_menu.addSeparator()
                clear = recent_menu.addAction("清空最近记录")
                clear.triggered.connect(self.clear_recent)

        def _activation_reason_is(self, reason: Any, *names: str) -> bool:
            enum = getattr(QSystemTrayIcon, "ActivationReason", QSystemTrayIcon)
            for name in names:
                value = getattr(enum, name, None)
                if value is not None and reason == value:
                    return True
            reason_name = getattr(reason, "name", "")
            if reason_name in names:
                return True
            text = str(reason)
            return any(text.endswith(f".{name}") or text == name for name in names)

        def _on_tray_activated(self, reason: Any) -> None:
            if self._activation_reason_is(reason, "Trigger", "DoubleClick"):
                now = time.monotonic()
                if now - self._last_device_menu_popup < 0.20 and self.device_menu.isVisible():
                    return
                self._last_device_menu_popup = now
                self._popup_device_menu()
            elif self._activation_reason_is(reason, "Context"):
                self.update_dynamic_state()

        def _popup_device_menu(self) -> None:
            self.refresh_volume_menu()
            self.device_menu.popup(self._tray_popup_position())

        def _tray_popup_position(self) -> QPoint:
            try:
                geometry = self.tray.geometry()
                if geometry.isValid() and not geometry.isNull():
                    return geometry.center()
            except Exception:
                pass
            return QCursor.pos()

        def _toggle_toast(self) -> None:
            receiver = self.receiver
            if not isinstance(receiver, ToastWindow):
                return
            if receiver.isVisible():
                receiver.hide()
            else:
                receiver.refresh_from_state()
                receiver.show_toast()
            self.update_dynamic_state()

        def _hide_toast(self) -> None:
            receiver = self.receiver
            if isinstance(receiver, ToastWindow) and receiver.isVisible():
                receiver.hide()
                self.actions.notify("已隐藏通知。", timeout=1800)
                self.update_dynamic_state()

        def _on_rescan_done(self, *_args: Any) -> None:
            """Re-enable the rescan action once a manual scan finishes."""
            self._mark_rescan_idle()

        def _rescan_clicked(self) -> None:
            if self._rescan_in_flight:
                return
            self._rescan_in_flight = True
            self.rescan_action.setEnabled(False)
            self.rescan_action.setText("重新扫描中…")
            try:
                self.service.rescan()
            except Exception as exc:
                self._mark_rescan_idle()
                log_error("manual_rescan_failed", {"message": str(exc)}, exc_info=True)
                self.actions.notify(f"重新扫描失败：{exc}", warning=True, timeout=5000)

        def _mark_rescan_idle(self, *_args: Any) -> None:
            self._rescan_in_flight = False
            if hasattr(self, "rescan_action") and self.rescan_action is not None:
                self.rescan_action.setEnabled(True)
                self.rescan_action.setText("重新扫描")

        def _poll_rescan_status(self) -> None:
            """Fallback: if the reconciler just finished a scan, re-enable the action."""
            reconciler = self.service.reconciler
            if self._rescan_in_flight and reconciler.scan_completed.is_set():
                self._mark_rescan_idle()

        def update_dynamic_state(self) -> None:
            count = len(group_volumes(self.service.state.snapshot()))
            operation_status = self.actions.operation_status()
            enabled_hooks = sum(1 for rule in normalize_hook_rules(self.config.hooks) if rule.get("enabled"))
            self.hooks_status_action.setText(f"自动化规则：{enabled_hooks} 条启用")
            self.task_status_action.setText(f"状态：{operation_status}")
            self.task_status_action.setToolTip(operation_status)
            self.tray.setToolTip(
                f"{APP_DISPLAY_NAME} · {count} 个设备 · {operation_status} · 左键设备 · 右键设置"
            )
            self.startup_action.blockSignals(True)
            self.startup_action.setChecked(bool(self.startup.status()["enabled"]))
            self.startup_action.blockSignals(False)
            if self.toast_action is not None and isinstance(self.receiver, ToastWindow):
                self.toast_action.setText("隐藏通知" if self.receiver.isVisible() else "显示通知")

        def save(self) -> None:
            self.store.save(self.config)

        def open_logs(self) -> None:
            try:
                self.config.log_dir.mkdir(parents=True, exist_ok=True)
                open_path(str(self.config.log_dir))
            except OSError as exc:
                log_error("open_logs_failed", {"message": str(exc)}, exc_info=True)
                self.actions.notify(f"打开日志目录失败：{exc}", warning=True, timeout=6000)

        def open_app_data(self) -> None:
            try:
                app_data_dir().mkdir(parents=True, exist_ok=True)
                open_path(str(app_data_dir()))
            except OSError as exc:
                log_error("open_app_data_failed", {"message": str(exc)}, exc_info=True)
                self.actions.notify(f"打开程序数据目录失败：{exc}", warning=True, timeout=6000)

        def reset_logs(self) -> None:
            try:
                current = LogConfig(self.config.log_dir, self.config.log_mode, self.config.log_max_bytes, self.config.log_backups, self.config.console_log)
                LOGGER.stop()
                LOGGER.reset_files(self.config.log_dir)
                LOGGER.configure(current, reset_logs=False)
                self.actions.notify("日志已清空。")
            except Exception as exc:
                log_error("reset_logs_failed", {"message": str(exc)}, exc_info=True)
                self.actions.notify(f"清空日志失败：{exc}", warning=True, timeout=6000)

        def apply_log_mode(self, mode: LogMode) -> None:
            self.config.log_mode = mode
            self.save()
            LOGGER.set_mode(mode)
            self.update_dynamic_state()
            self.actions.notify(f"日志模式：{self.LOG_LABELS[mode]}")

        def apply_reset_on_start(self, enabled: bool) -> None:
            self.config.reset_logs_on_start = bool(enabled)
            self.save()

        def apply_theme(self, name: str) -> None:
            self.config.theme = name
            self.save()
            theme = Theme(name, self.app)
            self.theme = theme
            if isinstance(self.receiver, ToastWindow):
                self.receiver.apply_theme(theme)
            icon = self.icons.icon(theme)
            self.tray.setIcon(icon)
            self.app.setWindowIcon(icon)
            self.update_dynamic_state()

        def apply_topmost(self, enabled: bool) -> None:
            self.config.topmost = bool(enabled)
            self.save()
            if isinstance(self.receiver, ToastWindow):
                self.receiver.set_topmost(enabled)

        def toggle_startup(self, enabled: bool) -> None:
            try:
                self.startup.set_enabled(bool(enabled))
                status = self.startup.status()
                message = "已开启开机启动。" if status["healthy"] else "已写入启动项，但检查结果异常。"
                if not enabled:
                    message = "已关闭开机启动。"
                self.actions.notify(message, warning=enabled and not status["healthy"])
            except Exception as exc:
                log_error("toggle_startup_failed", {"enabled": bool(enabled), "message": str(exc)}, exc_info=True)
                self.actions.notify(f"设置开机启动失败：{exc}", warning=True, timeout=7000)
            finally:
                self.update_dynamic_state()

        def repair_startup(self) -> None:
            try:
                self.startup.set_enabled(True)
                status = self.startup.status()
                self.actions.notify("开机启动已修复。" if status["healthy"] else "启动项仍未通过检查。", warning=not status["healthy"])
            except Exception as exc:
                log_error("repair_startup_failed", {"message": str(exc)}, exc_info=True)
                self.actions.notify(f"修复开机启动失败：{exc}", warning=True, timeout=7000)
            finally:
                self.update_dynamic_state()

        def clear_recent(self) -> None:
            self.actions.recent.clear()
            self.actions.notify("最近记录已清空。")


    class GuiRuntime:
        def __init__(self, args: argparse.Namespace, config: AppConfig, store: ConfigStore, startup: StartupManager) -> None:
            self.args = args
            self.config = config
            self.store = store
            self.startup = startup
            self.app = QApplication(sys.argv[:1])
            self.app.setApplicationName(APP_DISPLAY_NAME)
            self.app.setOrganizationName(APP_ORG)
            self.app.setQuitOnLastWindowClosed(False)
            self.theme = Theme(config.theme, self.app)
            self.icons = IconFactory(self.app)
            self.app.setWindowIcon(self.icons.icon(self.theme))
            self.bridge = EventBridge()
            self.tray: Optional[QSystemTrayIcon] = None
            if QSystemTrayIcon.isSystemTrayAvailable():
                self.tray = QSystemTrayIcon(self.icons.icon(self.theme))
                self.tray.show()
            self.recent = RecentVolumeManager(config, store)
            self.service = UsbMonitorService(self.bridge.event_received.emit)
            self.actions = GuiActions(self.app, self.tray, self.service, self.recent)
            self.receiver: Any
            if config.gui_backend == "tray-only":
                if self.tray is None:
                    raise RuntimeError("系统托盘不可用，无法使用 tray-only 模式。")
                self.receiver = TrayReceiver(self.tray, self.actions)
            else:
                self.receiver = ToastWindow(self.app, self.theme, self.icons, self.actions, config.topmost, exit_on_close=self.tray is None)
                self.actions.toast = self.receiver
            # Initialize hook runner for user-defined automation rules.  Hooks are
            # optional; if configuration contains invalid entries this block
            # silently disables hooks but preserves normal operation.  Import is
            # done lazily to avoid unnecessary dependencies when hooks are unused.
            try:
                from .hooks import HookRunner, HookRule  # type: ignore
                rules = [HookRule(**h) for h in (self.config.hooks or [])]
                self.hook_runner = HookRunner(rules)
                try:
                    # Use queued connection when available so hooks run off the GUI thread.
                    self.bridge.event_received.connect(self.hook_runner.on_event, type=Qt.ConnectionType.QueuedConnection)
                except (AttributeError, TypeError, RuntimeError):
                    self.bridge.event_received.connect(self.hook_runner.on_event)
            except Exception as exc:
                # Hooks unavailable or misconfigured.
                log_error("hooks_initialization_failed", {"message": str(exc)}, exc_info=True)
                self.hook_runner = None  # type: ignore
            # Connect the GUI receiver after hooks so that UI updates occur after hooks run.
            try:
                self.bridge.event_received.connect(self.receiver.consume, type=Qt.ConnectionType.QueuedConnection)
            except TypeError:
                self.bridge.event_received.connect(self.receiver.consume)
            self.menu: Optional[TrayMenuController] = None
            if self.tray:
                self.menu = TrayMenuController(
                    self.tray,
                    self.app,
                    config,
                    store,
                    startup,
                    self.service,
                    self.actions,
                    self.receiver,
                    self.icons,
                    self.theme,
                )
            self.app.aboutToQuit.connect(self.shutdown)

        def run(self) -> int:
            self.service.start()
            log_action(
                "app_started",
                {
                    "gui_backend": self.config.gui_backend,
                    "theme": self.config.theme,
                    "topmost": self.config.topmost,
                    "startup_arg": bool(self.args.startup),
                },
            )
            return int(self.app.exec())

        def shutdown(self) -> None:
            self.service.stop()
            hook_runner = getattr(self, "hook_runner", None)
            if hook_runner is not None:
                try:
                    hook_runner.stop()
                except Exception as exc:
                    log_error("hooks_shutdown_failed", {"message": str(exc)}, exc_info=True)
            log_action("app_quit", {"reason": "qt_about_to_quit"})


# ---------------------------------------------------------------------------
# Console and CLI
# ---------------------------------------------------------------------------


def run_gui(args: argparse.Namespace, config: AppConfig, store: ConfigStore, startup: StartupManager) -> int:
    if not QT_AVAILABLE:
        raise RuntimeError("GUI 模式需要 PySide6：py -m pip install PySide6")
    return GuiRuntime(args, config, store, startup).run()


def run_console() -> int:
    if not IS_WINDOWS:
        print("This application only supports Windows.", file=sys.stderr)
        return 2

    def sink(event: UsbEvent) -> None:
        payload = {
            "action": event.action,
            "changed_paths": event.changed_paths,
            "snapshot": [asdict(info) for info in event.snapshot],
            "details": dict(event.details),
            "timestamp_utc": event.timestamp_utc,
        }
        print(json.dumps(sanitize_for_log(payload, raw=LOGGER.raw), ensure_ascii=False, default=str))

    service = UsbMonitorService(sink)
    service.start()
    log_action("console_started", {})
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()
        log_action("console_stopped", {})
    return 0


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Windows USB monitor with PySide6 tray/toast UI.")
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--log-mode", choices=("off", "redacted", "raw"))
    parser.add_argument("--log-raw", action="store_true")
    parser.add_argument("--log-off", action="store_true")
    parser.add_argument("--reset-logs-on-start", action="store_true", default=None)
    parser.add_argument("--no-reset-logs-on-start", dest="reset_logs_on_start", action="store_false")
    parser.add_argument("--log-max-bytes", type=int)
    parser.add_argument("--log-backups", type=int)
    parser.add_argument("--console-log", action="store_true", default=None)
    parser.add_argument("--no-gui", action="store_true")
    parser.add_argument("--gui-backend", choices=("qt-toast", "tray-only"))
    parser.add_argument("--theme", choices=("auto", "dark", "light"))
    parser.add_argument("--topmost", dest="topmost", action="store_true", default=None)
    parser.add_argument("--no-topmost", dest="topmost", action="store_false")
    parser.add_argument("--allow-multiple", action="store_true")
    parser.add_argument("--install-startup", action="store_true")
    parser.add_argument("--uninstall-startup", action="store_true")
    parser.add_argument("--startup-status", action="store_true")
    parser.add_argument("--startup", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--silent", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def merge_cli_config(args: argparse.Namespace, stored: AppConfig) -> AppConfig:
    config = replace(stored, recent_volumes=list(stored.recent_volumes), hooks=[dict(rule) for rule in stored.hooks])
    if args.log_dir is not None:
        config.log_dir = args.log_dir
    if args.log_mode is not None:
        config.log_mode = LogMode.parse(args.log_mode)
    if args.log_raw:
        config.log_mode = LogMode.RAW
    if args.log_off:
        config.log_mode = LogMode.OFF
    if args.reset_logs_on_start is not None:
        config.reset_logs_on_start = bool(args.reset_logs_on_start)
    if args.log_max_bytes is not None:
        config.log_max_bytes = max(args.log_max_bytes, 10_000)
    if args.log_backups is not None:
        config.log_backups = max(args.log_backups, 0)
    if args.console_log is not None:
        config.console_log = bool(args.console_log)
    if args.gui_backend is not None:
        config.gui_backend = args.gui_backend
    elif getattr(args, "silent", False) or getattr(args, "startup", False):
        config.gui_backend = "tray-only"
    if args.theme is not None:
        config.theme = args.theme
    if args.topmost is not None:
        config.topmost = bool(args.topmost)
    return config


def notify_second_instance() -> None:
    if user32 is None:
        return
    try:
        user32.MessageBoxW(
            None,
            f"{APP_DISPLAY_NAME} 已经在运行。请在系统托盘中打开它。",
            APP_DISPLAY_NAME,
            0x00000040 | 0x00010000 | 0x00040000,
        )
    except Exception:
        pass


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    store = ConfigStore(config_path())
    config = merge_cli_config(args, store.load())
    try:
        store.save(config)
    except Exception as exc:
        print(f"[{APP_DISPLAY_NAME}] failed to save config: {exc}", file=sys.stderr)

    LOGGER.configure(
        LogConfig(config.log_dir, config.log_mode, config.log_max_bytes, config.log_backups, config.console_log),
        reset_logs=config.reset_logs_on_start,
    )
    atexit.register(LOGGER.stop)
    atexit.register(release_single_instance)
    startup = StartupManager()

    if not IS_WINDOWS:
        if args.startup_status:
            print(json.dumps(startup.status(), ensure_ascii=False, indent=2))
            return 0
        print("This application only supports Windows.", file=sys.stderr)
        return 2

    if args.startup_status:
        print(json.dumps(startup.status(), ensure_ascii=False, indent=2))
        return 0
    if args.install_startup:
        method = startup.set_enabled(True)
        print(json.dumps({"enabled": True, "method": method, "status": startup.status()}, ensure_ascii=False, indent=2))
        return 0
    if args.uninstall_startup:
        method = startup.set_enabled(False)
        print(json.dumps({"enabled": False, "method": method, "status": startup.status()}, ensure_ascii=False, indent=2))
        return 0

    try:
        repaired = startup.repair_if_needed()
        if repaired:
            log_action("startup_repaired", {"method": repaired})
    except Exception as exc:
        log_error("startup_repair_failed", {"message": str(exc)}, exc_info=True)

    if not args.allow_multiple and not acquire_single_instance():
        notify_second_instance()
        return 0

    try:
        return run_console() if args.no_gui else run_gui(args, config, store, startup)
    except Exception as exc:
        log_error("main_failed", {"message": str(exc)}, exc_info=True)
        if args.no_gui:
            print(f"{APP_DISPLAY_NAME}: {exc}", file=sys.stderr)
        return 1
    finally:
        release_single_instance()
        LOGGER.stop()


if __name__ == "__main__":
    raise SystemExit(main())
