#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Windows USB monitor, rewritten as a maintainable single-file PySide6 app.

What changed in this rewrite:
- Tray menu has a real "日志" submenu: open directory, disable logging, redacted logging,
  explicit/raw logging, and reset logs on every app start.
- Startup registration uses a named Startup-folder shortcut plus HKCU Run fallback,
  with script-mode source copied into AppData to avoid broken download/temp paths.
- SVG/window/tray icons are rendered as high-DPI pixmaps with multiple icon sizes.
- Logging is runtime-switchable and can be fully disabled without tearing down the GUI.
- Fast USB insert/remove bursts are reconciled by immediate remove delivery plus short
  multi-pass drive rescans, avoiding stale GUI rows after quick unplug.
- The previous giant run_gui() body has been split into small manager/UI classes.

Dependencies on Windows:
    py -m pip install PySide6 pywin32
"""

from __future__ import annotations

import argparse
import atexit
import ctypes
from ctypes import wintypes
from dataclasses import dataclass, field
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
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from datetime import datetime, timezone
from typing import Any, Callable, Optional, Sequence

APP_NAME = "USBMonitor"
APP_ORG = "BellaKipping"
APP_DISPLAY_NAME = "USB Monitor"
STARTUP_REG_NAME = APP_NAME
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
STARTUP_SCRIPT_FILENAME = f"{APP_NAME}.py"
STARTUP_EXE_FILENAME = f"{APP_NAME}.exe"
LOG = logging.getLogger("usb_monitor")
EventSink = Callable[["UsbEvent"], None]

SENSITIVE_KEYS = {
    "serial", "serial_num", "id_serial", "id_serial_short", "uuid", "id_fs_uuid",
    "label", "volume_label", "device_path", "device_node", "sys_path", "mount_point",
    "drive_paths", "removed_volumes", "removed_paths", "open_paths", "path", "paths",
    "raw_path", "_name", "location_id", "volume_serial", "device_instance_id",
}

# ---------------------------------------------------------------------------
# Data models and persistent app config
# ---------------------------------------------------------------------------


class LogMode(str, Enum):
    OFF = "off"
    REDACTED = "redacted"
    RAW = "raw"

    @classmethod
    def normalize(cls, value: Any) -> "LogMode":
        text = str(value or cls.REDACTED.value).strip().lower()
        if text in {"0", "false", "no", "none", "disabled", "disable", "closed", "close", "off"}:
            return cls.OFF
        if text in {"raw", "plain", "explicit", "visible", "full", "unredacted", "明文", "显式"}:
            return cls.RAW
        return cls.REDACTED


@dataclass(frozen=True)
class UsbEvent:
    action: str
    details: dict[str, Any]
    open_paths: tuple[str, ...] = field(default_factory=tuple)
    display: bool = True
    timestamp_utc: str = field(default_factory=lambda: _now_utc())
    timestamp_local: str = field(default_factory=lambda: _now_local())
    timezone: str = field(default_factory=lambda: _local_tz_name())


@dataclass
class VolumeInfo:
    path: str
    title: str
    source: str
    timestamp_utc: str
    drive_type: str = "unknown"
    total: Optional[int] = None
    used: Optional[int] = None
    free: Optional[int] = None
    disk_number: Optional[int] = None


@dataclass
class AppConfig:
    log_dir: Path
    log_mode: LogMode = LogMode.REDACTED
    reset_logs_on_start: bool = False
    log_max_bytes: int = 1_000_000
    log_backups: int = 5
    console_log: bool = False
    theme: str = "auto"
    topmost: bool = True
    gui_backend: str = "qt-toast"
    recent_volumes: list[dict[str, Any]] = field(default_factory=list)


@dataclass(frozen=True)
class LogConfig:
    log_dir: Path
    mode: LogMode = LogMode.REDACTED
    max_bytes: int = 1_000_000
    backup_count: int = 5
    console_log: bool = False


def _now_utc() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _now_local() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def _local_tz_name() -> str:
    tz = datetime.now().astimezone().tzinfo
    return tz.tzname(None) if tz else "local"


def app_data_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
    if base:
        return Path(base) / APP_NAME
    return Path.home() / "AppData" / "Local" / APP_NAME


def default_log_dir() -> Path:
    return app_data_dir() / "logs"


def config_path() -> Path:
    return app_data_dir() / "config.json"


def _hash_id(value: Any) -> str:
    return hashlib.sha256(str(value).encode("utf-8", errors="replace")).hexdigest()[:12]


def _coerce_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in {"1", "true", "yes", "y", "on", "enable", "enabled"}


def _normalise_drive_key(path: Any) -> str:
    text = str(path or "").strip().replace("/", "\\")
    if len(text) >= 2 and text[1] == ":":
        return (text[:2].upper() + "\\")
    return text.rstrip("\\").lower()


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _normalize_recent_volume_records(value: Any, limit: int = 8) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        return []
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            continue
        original_path = str(item.get("path") or "").strip()
        key = _normalise_drive_key(original_path)
        if not key or key in seen:
            continue
        seen.add(key)
        path = key if len(key) == 3 and key[1] == ":" else original_path
        fallback_title = _display_name_for_path(path)
        records.append(
            {
                "path": path,
                "title": str(item.get("title") or fallback_title),
                "drive_type": str(item.get("drive_type") or "unknown"),
                "last_seen_utc": str(item.get("last_seen_utc") or item.get("timestamp_utc") or ""),
                "last_seen_local": str(item.get("last_seen_local") or ""),
                "open_count": max(_safe_int(item.get("open_count"), 0), 0),
                "total": item.get("total"),
                "free": item.get("free"),
            }
        )
    records.sort(key=lambda rec: str(rec.get("last_seen_utc") or ""), reverse=True)
    return records[:limit]


class ConfigStore:
    def __init__(self, path: Path) -> None:
        self.path = path

    def load(self) -> AppConfig:
        raw: dict[str, Any] = {}
        try:
            if self.path.exists():
                raw = json.loads(self.path.read_text(encoding="utf-8"))
        except Exception:
            raw = {}
        return AppConfig(
            log_dir=Path(raw.get("log_dir") or default_log_dir()),
            log_mode=LogMode.normalize(raw.get("log_mode", LogMode.REDACTED.value)),
            reset_logs_on_start=_coerce_bool(raw.get("reset_logs_on_start"), False),
            log_max_bytes=max(int(raw.get("log_max_bytes", 1_000_000) or 1_000_000), 10_000),
            log_backups=max(int(raw.get("log_backups", 5) or 5), 0),
            console_log=_coerce_bool(raw.get("console_log"), False),
            theme=str(raw.get("theme") or "auto") if str(raw.get("theme") or "auto") in {"auto", "light", "dark"} else "auto",
            topmost=_coerce_bool(raw.get("topmost"), True),
            gui_backend=str(raw.get("gui_backend") or "qt-toast") if str(raw.get("gui_backend") or "qt-toast") in {"qt-toast", "tray-only"} else "qt-toast",
            recent_volumes=_normalize_recent_volume_records(raw.get("recent_volumes", [])),
        )

    def save(self, config: AppConfig) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "log_dir": str(config.log_dir),
            "log_mode": config.log_mode.value,
            "reset_logs_on_start": config.reset_logs_on_start,
            "log_max_bytes": config.log_max_bytes,
            "log_backups": config.log_backups,
            "console_log": config.console_log,
            "theme": config.theme,
            "topmost": config.topmost,
            "gui_backend": config.gui_backend,
            "recent_volumes": _normalize_recent_volume_records(config.recent_volumes),
        }
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(self.path)


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------


def _redact_value(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, dict):
        return {str(k): _redact_value(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [f"redacted:{_hash_id(item)}" for item in value]
    text = str(value)
    if not text:
        return ""
    return f"redacted:{_hash_id(text)}"


def sanitize_for_log(value: Any, raw: bool = False) -> Any:
    if raw:
        return value
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, item in value.items():
            lowered = str(key).lower()
            if lowered in SENSITIVE_KEYS:
                result[str(key)] = _redact_value(item)
            elif isinstance(item, dict):
                result[str(key)] = sanitize_for_log(item, raw=False)
            elif isinstance(item, (list, tuple, set)):
                result[str(key)] = [sanitize_for_log(child, raw=False) for child in item]
            else:
                result[str(key)] = item
        return result
    if isinstance(value, (list, tuple, set)):
        return [sanitize_for_log(child, raw=False) for child in value]
    return value


class CategoryFilter(logging.Filter):
    def __init__(self, category: str) -> None:
        super().__init__()
        self.category = category

    def filter(self, record: logging.LogRecord) -> bool:
        return getattr(record, "category", None) == self.category


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        # record.created is captured at LogRecord construction time (i.e. when the
        # original log call happened), not when this formatter later runs on the
        # QueueListener thread. Re-querying datetime.now() here would record the
        # time the queue got drained instead of when the event actually occurred.
        created_utc = datetime.fromtimestamp(record.created, tz=timezone.utc)
        created_local = created_utc.astimezone()
        payload: dict[str, Any] = {
            "time_utc": created_utc.isoformat(timespec="seconds"),
            "time_local": created_local.isoformat(timespec="seconds"),
            "timezone": created_local.tzname() or "local",
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
        }
        if hasattr(record, "payload"):
            payload.update(getattr(record, "payload"))
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=False, sort_keys=True, default=str)


def _make_rotating_handler(path: Path, category: str, max_bytes: int, backup_count: int) -> RotatingFileHandler:
    handler = RotatingFileHandler(path, maxBytes=max_bytes, backupCount=backup_count, encoding="utf-8")
    handler.setLevel(logging.DEBUG)
    handler.addFilter(CategoryFilter(category))
    handler.setFormatter(JsonFormatter())
    return handler


class LoggingManager:
    """Runtime-switchable structured logging."""

    def __init__(self) -> None:
        self.listener: Optional[QueueListener] = None
        self.config: Optional[LogConfig] = None
        self.enabled = False
        self.raw_logs = False
        self._configured_once = False

    def configure(self, config: LogConfig, reset_logs: bool = False) -> None:
        self.stop()
        self.config = config
        self.enabled = config.mode != LogMode.OFF
        self.raw_logs = config.mode == LogMode.RAW
        LOG.raw_logs = self.raw_logs  # type: ignore[attr-defined]
        LOG.log_dir = str(config.log_dir)  # type: ignore[attr-defined]
        root = logging.getLogger()
        root.handlers.clear()
        root.setLevel(logging.INFO)

        if config.mode == LogMode.OFF:
            root.addHandler(logging.NullHandler())
            self._install_exception_hooks(config.log_dir)
            return

        config.log_dir.mkdir(parents=True, exist_ok=True)
        if reset_logs:
            self.reset_log_files(config.log_dir)

        log_queue: queue.Queue[logging.LogRecord] = queue.Queue(maxsize=2000)
        root.addHandler(QueueHandler(log_queue))
        handlers: list[logging.Handler] = [
            _make_rotating_handler(config.log_dir / "events.log", "events", config.max_bytes, config.backup_count),
            _make_rotating_handler(config.log_dir / "actions.log", "actions", config.max_bytes, config.backup_count),
            _make_rotating_handler(config.log_dir / "errors.log", "errors", config.max_bytes, config.backup_count),
        ]
        if config.console_log and getattr(sys, "stderr", None) is not None:
            stream = logging.StreamHandler()
            stream.setFormatter(logging.Formatter("%(asctime)s - %(levelname)s - %(message)s"))
            handlers.append(stream)
        self.listener = QueueListener(log_queue, *handlers, respect_handler_level=True)
        self.listener.start()
        self._install_exception_hooks(config.log_dir)
        self._configured_once = True
        log_event("logging_started", {"log_dir": str(config.log_dir), "mode": config.mode.value, "raw_logs": self.raw_logs})

    def _install_exception_hooks(self, log_dir: Path) -> None:
        def excepthook(exc_type: type[BaseException], exc: BaseException, tb: Any) -> None:
            if self.enabled:
                write_crash_log(log_dir, exc_type, exc, tb)
                log_error("unhandled_exception", {"type": getattr(exc_type, "__name__", str(exc_type)), "message": str(exc)}, exc_info=(exc_type, exc, tb))
            else:
                traceback.print_exception(exc_type, exc, tb)

        def threading_excepthook(args: threading.ExceptHookArgs) -> None:
            if self.enabled:
                write_crash_log(log_dir, args.exc_type, args.exc_value, args.exc_traceback, thread_name=args.thread.name if args.thread else None)
                log_error(
                    "thread_unhandled_exception",
                    {"thread": args.thread.name if args.thread else None, "type": getattr(args.exc_type, "__name__", str(args.exc_type)), "message": str(args.exc_value)},
                    exc_info=(args.exc_type, args.exc_value, args.exc_traceback),
                )
            else:
                traceback.print_exception(args.exc_type, args.exc_value, args.exc_traceback)

        sys.excepthook = excepthook
        threading.excepthook = threading_excepthook

    def set_mode(self, mode: LogMode) -> None:
        cfg = self.config or LogConfig(default_log_dir())
        self.configure(LogConfig(cfg.log_dir, mode, cfg.max_bytes, cfg.backup_count, cfg.console_log), reset_logs=False)

    def reset_log_files(self, log_dir: Optional[Path] = None) -> None:
        target = log_dir or (self.config.log_dir if self.config else default_log_dir())
        target.mkdir(parents=True, exist_ok=True)
        for pattern in ("events.log*", "actions.log*", "errors.log*", "crash.log*"):
            for file_path in target.glob(pattern):
                try:
                    if file_path.is_file():
                        file_path.unlink()
                except Exception:
                    pass

    def stop(self) -> None:
        if self.listener is not None:
            try:
                self.listener.stop()
            except Exception:
                pass
            self.listener = None
        logging.getLogger().handlers.clear()


LOGGER_MANAGER = LoggingManager()


def stop_logging() -> None:
    LOGGER_MANAGER.stop()


def write_crash_log(log_dir: Path, exc_type: type[BaseException], exc: BaseException, tb: Any, thread_name: Optional[str] = None) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "time_utc": _now_utc(),
        "time_local": _now_local(),
        "timezone": _local_tz_name(),
        "thread": thread_name,
        "type": getattr(exc_type, "__name__", str(exc_type)),
        "message": str(exc),
        "traceback": "".join(traceback.format_exception(exc_type, exc, tb)),
    }
    try:
        with (log_dir / "crash.log").open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")
    except Exception:
        pass


def _log_structured(category: str, message: str, payload: dict[str, Any], level: int = logging.INFO, exc_info: Any = None) -> None:
    if not LOGGER_MANAGER.enabled:
        return
    safe_payload = sanitize_for_log(payload, raw=LOGGER_MANAGER.raw_logs)
    LOG.log(level, message, extra={"category": category, "payload": safe_payload}, exc_info=exc_info)


def log_event(name: str, payload: dict[str, Any]) -> None:
    _log_structured("events", name, {"event": name, **payload}, logging.INFO)


def log_action(name: str, payload: dict[str, Any]) -> None:
    _log_structured("actions", name, {"action": name, **payload}, logging.INFO)


def log_error(name: str, payload: dict[str, Any], exc_info: Any = None) -> None:
    _log_structured("errors", name, {"error": name, **payload}, logging.ERROR, exc_info=exc_info)


def log_usb_event(event: UsbEvent) -> None:
    payload: dict[str, Any] = {
        "timestamp_utc": event.timestamp_utc,
        "timestamp_local": event.timestamp_local,
        "timezone": event.timezone,
        "action": event.action,
        "details": event.details,
        "open_path_count": len(event.open_paths),
        "display": event.display,
    }
    if LOGGER_MANAGER.raw_logs:
        payload["open_paths"] = list(event.open_paths)
    else:
        payload["open_path_hashes"] = [_hash_id(path) for path in event.open_paths]
    log_event("usb_event", payload)


def emit_usb_event(action: str, details: dict[str, Any], sink: Optional[EventSink] = None, open_paths: Sequence[str] = (), display: bool = True) -> UsbEvent:
    unique_paths = tuple(dict.fromkeys(str(path) for path in open_paths if path))
    event = UsbEvent(action=action, details=details, open_paths=unique_paths, display=display)
    log_usb_event(event)
    if sink is not None and display:
        sink(event)
    return event


# ---------------------------------------------------------------------------
# Windows helpers
# ---------------------------------------------------------------------------

if platform.system() == "Windows":
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
else:
    kernel32 = None  # type: ignore[assignment]
    user32 = None  # type: ignore[assignment]

WM_CLOSE = 0x0010
WM_DEVICECHANGE = 0x0219
DBT_CONFIGCHANGED = 0x0018
DBT_DEVNODES_CHANGED = 0x0007
DBT_DEVICEARRIVAL = 0x8000
DBT_DEVICEREMOVECOMPLETE = 0x8004
DBT_DEVTYP_VOLUME = 0x00000002
DBT_DEVTYP_DEVICEINTERFACE = 0x00000005
DBTF_MEDIA = 0x0001
DEVICE_NOTIFY_WINDOW_HANDLE = 0x00000000
DRIVE_UNKNOWN = 0
DRIVE_NO_ROOT_DIR = 1
DRIVE_REMOVABLE = 2
DRIVE_FIXED = 3
DRIVE_REMOTE = 4
DRIVE_CDROM = 5
DRIVE_RAMDISK = 6
ERROR_ALREADY_EXISTS = 183

DRIVE_TYPE_NAMES = {
    DRIVE_UNKNOWN: "unknown",
    DRIVE_NO_ROOT_DIR: "no_root",
    DRIVE_REMOVABLE: "removable",
    DRIVE_FIXED: "fixed",
    DRIVE_REMOTE: "remote",
    DRIVE_CDROM: "cdrom",
    DRIVE_RAMDISK: "ramdisk",
}


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class DEV_BROADCAST_HDR(ctypes.Structure):
    _fields_ = [
        ("dbch_size", wintypes.DWORD),
        ("dbch_devicetype", wintypes.DWORD),
        ("dbch_reserved", wintypes.DWORD),
    ]


class DEV_BROADCAST_VOLUME(ctypes.Structure):
    _fields_ = [
        ("dbcv_size", wintypes.DWORD),
        ("dbcv_devicetype", wintypes.DWORD),
        ("dbcv_reserved", wintypes.DWORD),
        ("dbcv_unitmask", wintypes.DWORD),
        ("dbcv_flags", wintypes.WORD),
    ]


class DEV_BROADCAST_DEVICEINTERFACE_W(ctypes.Structure):
    _fields_ = [
        ("dbcc_size", wintypes.DWORD),
        ("dbcc_devicetype", wintypes.DWORD),
        ("dbcc_reserved", wintypes.DWORD),
        ("dbcc_classguid", GUID),
        ("dbcc_name", ctypes.c_wchar * 1),
    ]


def _usb_device_interface_guid() -> GUID:
    return GUID(0xA5DCBF10, 0x6530, 0x11D2, (ctypes.c_ubyte * 8)(0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED))


def _windows_drive_paths_from_unitmask(unitmask: int) -> list[str]:
    return [f"{chr(ord('A') + index)}:\\" for index in range(26) if unitmask & (1 << index)]


def _drive_type(path: str) -> int:
    if kernel32 is None:
        return DRIVE_UNKNOWN
    kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetDriveTypeW.restype = wintypes.UINT
    return int(kernel32.GetDriveTypeW(path))


def _logical_drive_paths() -> list[str]:
    if kernel32 is None:
        return []
    kernel32.GetLogicalDrives.restype = wintypes.DWORD
    mask = int(kernel32.GetLogicalDrives())
    return [f"{chr(ord('A') + i)}:\\" for i in range(26) if mask & (1 << i)]


def _volume_label(path: str) -> str:
    if kernel32 is None:
        return ""
    volume_name = ctypes.create_unicode_buffer(261)
    fs_name = ctypes.create_unicode_buffer(261)
    serial = wintypes.DWORD()
    max_component = wintypes.DWORD()
    flags = wintypes.DWORD()
    ok = kernel32.GetVolumeInformationW(
        wintypes.LPCWSTR(path),
        volume_name,
        len(volume_name),
        ctypes.byref(serial),
        ctypes.byref(max_component),
        ctypes.byref(flags),
        fs_name,
        len(fs_name),
    )
    if not ok:
        return ""
    return volume_name.value or ""


def _system_drive_path() -> str:
    """The Windows boot/system drive (usually C:\\) must never be treated as a
    monitored USB volume, even though it is a DRIVE_FIXED drive just like many
    external USB hard disks."""
    letter = (os.environ.get("SystemDrive") or "C:").strip().rstrip("\\").rstrip(":")
    return f"{letter}:\\"


IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000
IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
GENERIC_READ_NONE = 0
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
STORAGE_DEVICE_PROPERTY = 0
PROPERTY_STANDARD_QUERY = 0
BUS_TYPE_USB = 7
BUS_TYPE_SD = 12
BUS_TYPE_MMC = 13
REMOVABLE_BUS_TYPES = {BUS_TYPE_USB, BUS_TYPE_SD, BUS_TYPE_MMC}
_MAX_DISK_EXTENTS = 16


class _DiskExtent(ctypes.Structure):
    _fields_ = [
        ("DiskNumber", wintypes.DWORD),
        ("StartingOffset", ctypes.c_longlong),
        ("ExtentLength", ctypes.c_longlong),
    ]


class _VolumeDiskExtents(ctypes.Structure):
    _fields_ = [
        ("NumberOfDiskExtents", wintypes.DWORD),
        ("Extents", _DiskExtent * _MAX_DISK_EXTENTS),
    ]


class _StoragePropertyQuery(ctypes.Structure):
    _fields_ = [
        ("PropertyId", ctypes.c_int),
        ("QueryType", ctypes.c_int),
        ("AdditionalParameters", ctypes.c_ubyte * 1),
    ]


class _StorageDeviceDescriptor(ctypes.Structure):
    _fields_ = [
        ("Version", wintypes.DWORD),
        ("Size", wintypes.DWORD),
        ("DeviceType", ctypes.c_ubyte),
        ("DeviceTypeModifier", ctypes.c_ubyte),
        ("RemovableMedia", ctypes.c_ubyte),
        ("CommandQueueing", ctypes.c_ubyte),
        ("VendorIdOffset", ctypes.c_long),
        ("ProductIdOffset", ctypes.c_long),
        ("ProductRevisionOffset", ctypes.c_long),
        ("SerialNumberOffset", ctypes.c_long),
        ("BusType", ctypes.c_int),
        ("RawPropertiesLength", wintypes.DWORD),
        ("RawDeviceProperties", ctypes.c_ubyte * 1),
    ]


def _open_device_handle(device_path: str) -> Optional[int]:
    if kernel32 is None:
        return None
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    handle = kernel32.CreateFileW(device_path, GENERIC_READ_NONE, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    value = int(handle or 0)
    if value in (0, -1) or value == (1 << 64) - 1:
        return None
    return value


def _volume_disk_numbers(path: str) -> list[int]:
    """Resolve which physical disk number(s) back a drive letter, so sibling
    partitions of the same USB stick can be grouped, and so the disk's removable
    bus type can be looked up."""
    if kernel32 is None or not path:
        return []
    handle = _open_device_handle(f"\\\\.\\{path[0]}:")
    if handle is None:
        return []
    try:
        buf = _VolumeDiskExtents()
        returned = wintypes.DWORD(0)
        kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.DeviceIoControl.restype = wintypes.BOOL
        ok = kernel32.DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, None, 0, ctypes.byref(buf), ctypes.sizeof(buf), ctypes.byref(returned), None)
        if not ok:
            return []
        count = max(0, min(int(buf.NumberOfDiskExtents), _MAX_DISK_EXTENTS))
        return [int(buf.Extents[i].DiskNumber) for i in range(count)]
    except Exception:
        return []
    finally:
        kernel32.CloseHandle(handle)


def _physical_disk_is_removable(disk_number: int) -> bool:
    """True when the physical disk behind a drive letter genuinely sits on a
    USB/SD/MMC bus (or Windows itself flags it as removable media). Internal
    SATA/NVMe/RAID system disks are never affected, regardless of which drive
    letter Windows assigned them or whether GetDriveTypeW reports them as FIXED
    (many external USB hard disks do report FIXED too)."""
    if kernel32 is None:
        return False
    handle = _open_device_handle(f"\\\\.\\PhysicalDrive{disk_number}")
    if handle is None:
        return False
    try:
        query = _StoragePropertyQuery()
        query.PropertyId = STORAGE_DEVICE_PROPERTY
        query.QueryType = PROPERTY_STANDARD_QUERY
        descriptor = _StorageDeviceDescriptor()
        returned = wintypes.DWORD(0)
        kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.DeviceIoControl.restype = wintypes.BOOL
        ok = kernel32.DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, ctypes.byref(query), ctypes.sizeof(query), ctypes.byref(descriptor), ctypes.sizeof(descriptor), ctypes.byref(returned), None)
        if not ok:
            return False
        return bool(descriptor.RemovableMedia) or int(descriptor.BusType) in REMOVABLE_BUS_TYPES
    except Exception:
        return False
    finally:
        kernel32.CloseHandle(handle)


_DRIVE_CLASSIFICATION_CACHE_TTL = 2.0
_drive_classification_cache: dict[str, tuple[list[int], bool, float]] = {}
_drive_classification_cache_lock = threading.Lock()


def _classify_drive(path: str) -> tuple[list[int], bool]:
    """Cached wrapper around the disk-extent + bus-type IOCTLs in
    _volume_disk_numbers/_physical_disk_is_removable. Those are real Win32
    round-trips (sometimes noticeably slow for a sleeping/spun-down external
    drive). _drive_snapshot() is called synchronously from the single thread
    that pumps WM_DEVICECHANGE messages, so without caching, a rapid unplug ->
    replug burst (each producing its own device-change message) re-runs this
    expensive query for every mounted drive on every single message; if that
    falls behind, Windows can't get the next device-change message delivered
    in time and the GUI update for it appears delayed or swallowed entirely.
    A short per-drive-letter cache absorbs that burst without weakening the
    detection: the bus type of a given physical disk cannot change while it
    keeps the same identity, so reusing a very recent answer is always safe."""
    now = time.monotonic()
    with _drive_classification_cache_lock:
        cached = _drive_classification_cache.get(path)
        if cached is not None and now < cached[2]:
            return cached[0], cached[1]
    disk_numbers = _volume_disk_numbers(path)
    is_removable = any(_physical_disk_is_removable(n) for n in disk_numbers) if disk_numbers else False
    with _drive_classification_cache_lock:
        _drive_classification_cache[path] = (disk_numbers, is_removable, now + _DRIVE_CLASSIFICATION_CACHE_TTL)
        if len(_drive_classification_cache) > 64:
            for key, value in list(_drive_classification_cache.items()):
                if value[2] <= now:
                    _drive_classification_cache.pop(key, None)
    return disk_numbers, is_removable


def _drive_snapshot() -> dict[str, dict[str, Any]]:
    drives: dict[str, dict[str, Any]] = {}
    system_drive = _system_drive_path().upper()
    for path in _logical_drive_paths():
        dtype = _drive_type(path)
        if dtype in {DRIVE_NO_ROOT_DIR, DRIVE_REMOTE, DRIVE_CDROM, DRIVE_RAMDISK}:
            continue
        # Authoritative check: ask Windows what bus the underlying physical disk is
        # actually attached to, instead of guessing from drive type. Cached per
        # drive letter for a couple of seconds; see _classify_drive for why.
        disk_numbers, is_removable_by_bus = _classify_drive(path)
        if disk_numbers:
            is_removable = is_removable_by_bus
        else:
            # The lower-level disk query was unavailable (e.g. a restricted
            # sandbox): fall back to the old heuristic so a single failed IOCTL
            # never makes every drive disappear, but still never trust a FIXED
            # drive blindly and never trust the system drive.
            is_removable = dtype == DRIVE_REMOVABLE and path.upper() != system_drive
        if not is_removable:
            continue
        drives[path] = {
            "path": path,
            "drive_type": DRIVE_TYPE_NAMES.get(dtype, str(dtype)),
            "drive_type_code": dtype,
            "volume_label": _volume_label(path),
            "disk_number": disk_numbers[0] if disk_numbers else None,
        }
    return drives


def _details_from_lparam(lparam: int) -> tuple[dict[str, Any], list[str]]:
    if not lparam:
        return {"kind": "device_change", "has_lparam": False}, []
    header = ctypes.cast(lparam, ctypes.POINTER(DEV_BROADCAST_HDR)).contents
    if header.dbch_devicetype == DBT_DEVTYP_VOLUME:
        volume = ctypes.cast(lparam, ctypes.POINTER(DEV_BROADCAST_VOLUME)).contents
        paths = _windows_drive_paths_from_unitmask(int(volume.dbcv_unitmask))
        return {
            "kind": "volume",
            "unitmask": int(volume.dbcv_unitmask),
            "flags": int(volume.dbcv_flags),
            "media_flag": bool(volume.dbcv_flags & DBTF_MEDIA),
            "drive_paths": paths,
        }, paths
    if header.dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE:
        name_offset = DEV_BROADCAST_DEVICEINTERFACE_W.dbcc_name.offset
        device_path = ctypes.wstring_at(lparam + name_offset)
        return {"kind": "device_interface", "device_path": device_path}, []
    return {"kind": "device_change", "devicetype": int(header.dbch_devicetype)}, []


def _safe_disk_usage(path: str) -> tuple[Optional[int], Optional[int], Optional[int]]:
    try:
        usage = shutil.disk_usage(path)
        return int(usage.total), int(usage.used), int(usage.free)
    except OSError:
        return None, None, None


def _format_bytes(value: Optional[int]) -> str:
    if value is None:
        return "未知"
    size = float(value)
    for unit in ("B", "KB", "MB", "GB", "TB", "PB"):
        if abs(size) < 1024 or unit == "PB":
            return f"{size:.0f} {unit}" if unit in {"B", "KB"} else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


def _usage_percent_raw(total: Optional[int], used: Optional[int]) -> Optional[int]:
    if not total or used is None:
        return None
    return max(0, min(100, round(used / total * 100)))


def _usage_percent(info: VolumeInfo) -> Optional[int]:
    return _usage_percent_raw(info.total, info.used)


def _paths_from_event(event: UsbEvent) -> tuple[str, ...]:
    candidates = list(event.open_paths)
    for key in ("drive_paths", "removed_paths", "removed_volumes"):
        value = event.details.get(key)
        if isinstance(value, list):
            candidates.extend(str(item) for item in value if item)
    path = event.details.get("path")
    if isinstance(path, str) and path:
        candidates.append(path)
    return tuple(dict.fromkeys(candidates))


def _display_name_for_path(path: str) -> str:
    if path.endswith(":\\"):
        return f"移动磁盘 {path}"
    return path


def _build_volume_info(path: str, event: UsbEvent) -> VolumeInfo:
    total, used, free = _safe_disk_usage(path)
    snapshot = _drive_snapshot().get(path, {})
    label = snapshot.get("volume_label") or event.details.get("volume_label") or ""
    dtype = snapshot.get("drive_type") or event.details.get("drive_type") or "unknown"
    title = f"{label} · {path}" if label else _display_name_for_path(path)
    disk_number = snapshot.get("disk_number")
    return VolumeInfo(path=path, title=title, source=str(event.details.get("kind", "usb")), timestamp_utc=event.timestamp_utc, drive_type=str(dtype), total=total, used=used, free=free, disk_number=disk_number)


def _group_volumes_by_device(volumes: Sequence[VolumeInfo]) -> list[list[VolumeInfo]]:
    """Group sibling partitions of the same physical USB device together, so a
    HotPE-style stick with a data partition + an EFI partition shows up as one
    entry instead of two. Entries whose physical disk could not be resolved
    each get their own group rather than being merged blindly."""
    groups: dict[Any, list[VolumeInfo]] = {}
    order: list[Any] = []
    for info in volumes:
        key: Any = ("disk", info.disk_number) if info.disk_number is not None else ("path", info.path)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(info)
    return [groups[key] for key in order]


def _group_title(group: Sequence[VolumeInfo]) -> str:
    if len(group) == 1:
        return group[0].title
    label = group[0].title.split(" · ", 1)[0]
    paths = "、".join(item.path for item in group)
    return f"{label} · {paths}（{len(group)} 个分区）"


def _group_subtitle(group: Sequence[VolumeInfo]) -> str:
    if len(group) == 1:
        info = group[0]
        return f"{info.drive_type} · {info.path}"
    drive_type = group[0].drive_type
    paths = "、".join(item.path for item in group)
    return f"{drive_type} · {paths}"


def _group_preview_label(group: Sequence[VolumeInfo]) -> str:
    if len(group) == 1:
        return _display_name_for_path(group[0].path)
    return "、".join(item.path for item in group)


def open_path(path: str) -> None:
    os.startfile(path)  # type: ignore[attr-defined]


def reveal_in_explorer(path: str) -> None:
    """Open File Explorer at a path without stealing this app's control flow."""
    clean = str(path or "").strip()
    if len(clean) == 2 and clean[1] == ":":
        clean += "\\"
    if not clean:
        raise ValueError("empty path")
    if platform.system() != "Windows":
        open_path(clean)
        return
    # For a drive root, /select is unreliable; opening the root is the expected Explorer behavior.
    if len(clean) <= 3 and clean.endswith("\\") and clean[1:2] == ":":
        open_path(clean)
        return
    subprocess.Popen(["explorer", f"/select,{clean}"], close_fds=True)


def safe_eject_drive(path: str) -> str:
    """Ask Windows Explorer Shell to safely eject a removable drive.

    The shell verb is intentionally best-effort: Windows may refuse to eject when files
    are still open, and that refusal should surface to the user instead of silently
    removing the row from the UI.
    """
    if platform.system() != "Windows":
        raise RuntimeError("安全弹出仅支持 Windows。")
    clean = str(path or "").strip()
    if len(clean) < 2 or clean[1] != ":":
        raise ValueError(f"不是有效的盘符路径：{path}")
    drive = clean[:2].upper()
    try:
        import win32com.client  # type: ignore[import-not-found]
        shell = win32com.client.Dispatch("Shell.Application")
        drives = shell.NameSpace(17)  # ssfDRIVES / This PC
        if drives is None:
            raise RuntimeError("无法访问 Windows 资源管理器的驱动器列表。")
        item = drives.ParseName(drive)
        if item is None:
            raise RuntimeError(f"未找到驱动器 {drive}。")
        verbs = item.Verbs()
        count_attr = getattr(verbs, "Count", 0)
        count = int(count_attr() if callable(count_attr) else count_attr)
        for index in range(count):
            verb = verbs.Item(index)
            name_attr = getattr(verb, "Name", "")
            name = str(name_attr() if callable(name_attr) else name_attr).replace("&", "").strip().lower()
            if any(token in name for token in ("eject", "弹出", "安全删除", "safely remove")):
                verb.DoIt()
                return drive
        item.InvokeVerb("Eject")
        return drive
    except Exception as exc:
        raise RuntimeError(f"安全弹出 {drive} 失败：{exc}") from exc


def remember_recent_volume(config: AppConfig, store: Optional[ConfigStore], info_or_path: Any, opened: bool = False) -> None:
    if isinstance(info_or_path, VolumeInfo):
        path = info_or_path.path
        title = info_or_path.title
        drive_type = info_or_path.drive_type
        total = info_or_path.total
        free = info_or_path.free
    else:
        path = str(info_or_path or "")
        title = ""
        drive_type = _drive_snapshot().get(path, {}).get("drive_type", "unknown")
        total, _, free = _safe_disk_usage(path)
    key = _normalise_drive_key(path)
    if not key:
        return
    path = key if len(key) == 3 and key[1] == ":" else path
    existing = {_normalise_drive_key(item.get("path")): item for item in _normalize_recent_volume_records(config.recent_volumes)}
    old = existing.get(key, {})
    if not title:
        title = str(old.get("title") or _display_name_for_path(path))
    record = {
        "path": path,
        "title": title,
        "drive_type": drive_type or old.get("drive_type") or "unknown",
        "last_seen_utc": _now_utc(),
        "last_seen_local": _now_local(),
        "open_count": max(_safe_int(old.get("open_count"), 0), 0) + (1 if opened else 0),
        "total": total if total is not None else old.get("total"),
        "free": free if free is not None else old.get("free"),
    }
    records = [record] + [item for item in _normalize_recent_volume_records(config.recent_volumes) if _normalise_drive_key(item.get("path")) != key]
    config.recent_volumes = _normalize_recent_volume_records(records)
    if store is not None:
        try:
            store.save(config)
        except Exception as exc:
            log_error("recent_volume_save_failed", {"path": path, "message": str(exc)}, exc_info=True)


def current_volume_infos() -> list[VolumeInfo]:
    event = UsbEvent(action="change", details={"kind": "manual_scan"}, display=False)
    return [_build_volume_info(path, event) for path in sorted(_drive_snapshot())]


# ---------------------------------------------------------------------------
# Startup and single instance
# ---------------------------------------------------------------------------

_SINGLE_INSTANCE_HANDLE: Optional[int] = None


def _pythonw_path() -> Path:
    exe = Path(sys.executable).resolve()
    candidate = exe.with_name("pythonw.exe")
    return candidate if candidate.exists() else exe


def _startup_install_dir() -> Path:
    return app_data_dir() / "startup"


def _frozen_install_dir() -> Path:
    """Stable AppData folder for a relocated --onedir build (exe + adjacent support files)."""
    return _startup_install_dir() / "bin"


def _is_unsafe_startup_location(directory: Path) -> bool:
    """Downloads/temp folders are routinely cleared by the user or Windows, so a
    startup target left there will eventually point at a missing program."""
    try:
        resolved = directory.resolve()
    except Exception:
        resolved = directory
    unsafe_roots = [Path.home() / "Downloads"]
    for env_name in ("TEMP", "TMP"):
        value = os.environ.get(env_name)
        if value:
            unsafe_roots.append(Path(value))
    unsafe_roots.append(Path(tempfile.gettempdir()))
    for root in unsafe_roots:
        try:
            resolved.relative_to(root.resolve())
            return True
        except Exception:
            continue
    return False


def _installed_startup_source_path() -> Path:
    if getattr(sys, "frozen", False):
        current_exe = Path(sys.executable).resolve()
        if not _is_unsafe_startup_location(current_exe.parent):
            # Already running from a stable folder (e.g. an installed Program Files
            # location): point straight at it instead of duplicating the onedir tree.
            return current_exe
        return _frozen_install_dir() / STARTUP_EXE_FILENAME
    return _startup_install_dir() / STARTUP_SCRIPT_FILENAME


def _same_file_or_path(a: Path, b: Path) -> bool:
    try:
        return a.resolve().samefile(b.resolve())
    except Exception:
        return str(a.resolve()).casefold() == str(b.resolve()).casefold()


def _copy_for_startup_if_needed() -> Path:
    """Copy script mode into AppData, or relocate a --onedir build that is currently
    running from Downloads/Temp, so startup never points at a path the user or
    Windows may delete."""
    if getattr(sys, "frozen", False):
        current_exe = Path(sys.executable).resolve()
        source_dir = current_exe.parent
        if not _is_unsafe_startup_location(source_dir):
            return current_exe
        target_dir = _frozen_install_dir()
        target = target_dir / STARTUP_EXE_FILENAME
        if _same_file_or_path(source_dir, target_dir):
            return target
        try:
            if target.exists() and target_dir.exists():
                newest_source_mtime = max((p.stat().st_mtime for p in source_dir.rglob("*") if p.is_file()), default=0.0)
                if newest_source_mtime and newest_source_mtime <= target_dir.stat().st_mtime:
                    return target
        except Exception:
            pass
        tmp_dir = target_dir.with_name(target_dir.name + ".tmp")
        try:
            shutil.rmtree(tmp_dir, ignore_errors=True)
            shutil.copytree(source_dir, tmp_dir)
            copied_exe = tmp_dir / current_exe.name
            if copied_exe.exists() and copied_exe.name != STARTUP_EXE_FILENAME:
                copied_exe.replace(tmp_dir / STARTUP_EXE_FILENAME)
            shutil.rmtree(target_dir, ignore_errors=True)
            tmp_dir.replace(target_dir)
            return target
        except PermissionError:
            # The installed copy may currently be running and locked; keep using it
            # rather than breaking startup registration.
            shutil.rmtree(tmp_dir, ignore_errors=True)
            if target.exists():
                return target
            raise
        except Exception:
            shutil.rmtree(tmp_dir, ignore_errors=True)
            raise
    source = Path(__file__).resolve()
    target = _installed_startup_source_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    if _same_file_or_path(source, target):
        return target
    try:
        if target.exists() and source.stat().st_size == target.stat().st_size and int(source.stat().st_mtime) <= int(target.stat().st_mtime):
            return target
    except Exception:
        pass
    tmp = target.with_suffix(target.suffix + ".tmp")
    try:
        shutil.copy2(source, tmp)
        tmp.replace(target)
        return target
    except PermissionError:
        # If the installed exe is currently running, Windows may lock it. Keep the
        # existing installed copy rather than breaking startup registration.
        try:
            tmp.unlink()
        except Exception:
            pass
        if target.exists():
            return target
        raise
    except Exception:
        try:
            tmp.unlink()
        except Exception:
            pass
        raise


def _startup_payload(install_copy: bool = False) -> tuple[str, list[str], str]:
    """Return target, arguments and working directory for user-login startup."""
    if install_copy:
        app_source = _copy_for_startup_if_needed()
    else:
        app_source = _installed_startup_source_path()
        if not app_source.exists():
            app_source = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve()
    if getattr(sys, "frozen", False):
        return str(app_source), ["--startup"], str(app_source.parent)
    return str(_pythonw_path()), [str(app_source), "--startup"], str(app_source.parent)


def _quote_startup_command(args: Sequence[str]) -> str:
    return subprocess.list2cmdline(list(args))


def _startup_command(install_copy: bool = False) -> str:
    target, arguments, _working_dir = _startup_payload(install_copy=install_copy)
    return _quote_startup_command([target, *arguments])


def _startup_folder_path() -> Path:
    appdata = os.environ.get("APPDATA")
    base = Path(appdata) if appdata else Path.home() / "AppData" / "Roaming"
    return base / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"


def _startup_shortcut_path() -> Path:
    return _startup_folder_path() / f"{APP_DISPLAY_NAME}.lnk"


def _legacy_startup_cmd_path() -> Path:
    return _startup_folder_path() / f"{APP_NAME}.cmd"


def _read_run_value() -> str:
    if platform.system() != "Windows":
        return ""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            try:
                value, _ = winreg.QueryValueEx(key, STARTUP_REG_NAME)
            except FileNotFoundError:
                return ""
            return str(value or "")
    except Exception:
        return ""


def _write_run_value(command: str) -> None:
    if platform.system() != "Windows":
        return
    import winreg
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
        winreg.SetValueEx(key, STARTUP_REG_NAME, 0, winreg.REG_SZ, command)


def _delete_run_value() -> None:
    try:
        import winreg
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            try:
                winreg.DeleteValue(key, STARTUP_REG_NAME)
            except FileNotFoundError:
                pass
    except Exception:
        pass


def _delete_startup_shortcuts() -> None:
    for path in (_startup_shortcut_path(), _legacy_startup_cmd_path()):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except Exception:
            pass


def _startup_paths_from_command() -> dict[str, bool]:
    target, arguments, _working_dir = _startup_payload(install_copy=False)
    target_exists = Path(target).exists()
    script_or_exe = Path(arguments[0]) if arguments and str(arguments[0]).lower().endswith((".py", ".pyw", ".exe")) else Path(target)
    return {
        "target_exists": target_exists,
        "script_or_exe_exists": script_or_exe.exists(),
        "installed_copy_exists": _installed_startup_source_path().exists(),
    }


def startup_status_report() -> dict[str, Any]:
    command = startup_command_preview()
    run_value = _read_run_value()
    shortcut = _startup_shortcut_path()
    legacy_cmd = _legacy_startup_cmd_path()
    path_status = _startup_paths_from_command()
    shortcut_exists = shortcut.exists()
    legacy_cmd_exists = legacy_cmd.exists()
    run_registered = bool(run_value)
    enabled = shortcut_exists or legacy_cmd_exists or run_registered
    healthy = enabled and path_status["target_exists"] and path_status["script_or_exe_exists"] and (shortcut_exists or legacy_cmd_exists) and run_registered
    return {
        "enabled": enabled,
        "healthy": healthy,
        "shortcut_path": str(shortcut),
        "shortcut_exists": shortcut_exists,
        "legacy_cmd_path": str(legacy_cmd),
        "legacy_cmd_exists": legacy_cmd_exists,
        "run_key": RUN_KEY,
        "run_name": STARTUP_REG_NAME,
        "run_value": run_value,
        "run_registered": run_registered,
        "expected_command": command,
        **path_status,
    }


def is_startup_enabled() -> bool:
    return bool(startup_status_report().get("enabled"))


def _create_startup_shortcut_and_run_key() -> str:
    target, arguments, working_dir = _startup_payload(install_copy=True)
    command = _quote_startup_command([target, *arguments])
    shortcut_path = _startup_shortcut_path()
    shortcut_path.parent.mkdir(parents=True, exist_ok=True)
    method_parts: list[str] = []

    try:
        import win32com.client  # type: ignore[import-not-found]
        shell = win32com.client.Dispatch("WScript.Shell")
        shortcut = shell.CreateShortcut(str(shortcut_path))
        shortcut.TargetPath = target
        shortcut.Arguments = subprocess.list2cmdline(arguments)
        shortcut.WorkingDirectory = working_dir
        shortcut.Description = APP_DISPLAY_NAME
        shortcut.IconLocation = f"{target},0"
        shortcut.Save()
        try:
            _legacy_startup_cmd_path().unlink()
        except FileNotFoundError:
            pass
        method_parts.append("startup_shortcut")
    except Exception as exc:
        # Fallback for systems where COM/pywin32 is unavailable. It is less pretty,
        # but it still works when Windows runs Startup folder contents at sign-in.
        cmd_path = _legacy_startup_cmd_path()
        cmd_path.write_text(f"@echo off\r\nchcp 65001 >nul\r\ncd /d {subprocess.list2cmdline([working_dir])}\r\nstart \"{APP_DISPLAY_NAME}\" {command}\r\n", encoding="utf-8")
        log_error("startup_shortcut_failed_fallback_cmd", {"message": str(exc), "shortcut": str(shortcut_path), "cmd": str(cmd_path)}, exc_info=True)
        method_parts.append("startup_folder_cmd_fallback")

    # Keep a Run-key fallback as well. If both fire, the single-instance mutex exits
    # the second process immediately, so reliability improves without duplicate UI.
    try:
        _write_run_value(command)
        method_parts.append("run_key")
    except Exception as exc:
        log_error("startup_run_key_write_failed", {"message": str(exc), "command": command}, exc_info=True)
        if not method_parts:
            raise
    return "+".join(method_parts)


def set_startup_enabled(enabled: bool) -> str:
    """Enable user-login startup. Uses Startup folder plus HKCU Run fallback."""
    if platform.system() != "Windows":
        raise RuntimeError("Only Windows startup is supported.")
    if not enabled:
        _delete_run_value()
        _delete_startup_shortcuts()
        return "disabled"
    return _create_startup_shortcut_and_run_key()


def repair_startup_registration_if_needed() -> Optional[str]:
    """Repair partial or stale startup registration without toggling the UI off."""
    if platform.system() != "Windows":
        return None
    report = startup_status_report()
    if not report.get("enabled"):
        return None
    if report.get("healthy"):
        return None
    return set_startup_enabled(True)


def startup_command_preview() -> str:
    return _startup_command(install_copy=False)

def acquire_single_instance_lock() -> bool:
    global _SINGLE_INSTANCE_HANDLE
    if platform.system() != "Windows" or kernel32 is None:
        return True
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    handle = kernel32.CreateMutexW(None, True, f"Local\\{APP_NAME}_SingleInstance")
    err = ctypes.get_last_error()
    _SINGLE_INSTANCE_HANDLE = int(handle or 0)
    return err != ERROR_ALREADY_EXISTS


# ---------------------------------------------------------------------------
# Win32 detector
# ---------------------------------------------------------------------------


class Win32UsbDetector(threading.Thread):
    """Dedicated hidden-window USB monitor with burst-safe drive reconciliation.

    Windows can report a very fast plug/unplug as device-interface or devnode changes
    before a drive letter is mounted. To avoid stale UI state, we combine immediate
    device messages with several short drive snapshots.
    """

    def __init__(self, sink: EventSink) -> None:
        super().__init__(daemon=True, name="win32-usb-detector")
        self.sink = sink
        self.stop_event = threading.Event()
        self.hwnd: Optional[int] = None
        self.notification_handle: Optional[int] = None
        self.window_class: Optional[str] = None
        self._baseline = _drive_snapshot()
        self._recent: dict[str, float] = {}
        self._wnd_proc_ref: Any = None
        self._scan_lock = threading.Lock()
        self._dedup_lock = threading.Lock()
        self._last_no_path_arrival_ts = 0.0
        self._event_history: list[UsbEvent] = []

    def stop(self) -> None:
        self.stop_event.set()
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
            emit_usb_event("error", {"kind": "error", "message": "Windows backend requires pywin32: py -m pip install pywin32"}, sink=self.sink)
            log_error("pywin32_missing", {"message": str(exc)}, exc_info=True)
            return

        if user32 is None:
            emit_usb_event("error", {"kind": "error", "message": "This build only supports Windows."}, sink=self.sink)
            return

        def wnd_proc(hwnd: int, msg: int, wparam: int, lparam: int) -> int:
            if msg == WM_DEVICECHANGE:
                self._handle_device_change(int(wparam), int(lparam))
                return 0
            if msg == WM_CLOSE:
                self.stop_event.set()
                return 0
            return win32gui.DefWindowProc(hwnd, msg, wparam, lparam)

        self._wnd_proc_ref = wnd_proc
        class_name = f"{APP_NAME}HiddenWindow{os.getpid()}"
        self.window_class = class_name
        hinst = win32gui.GetModuleHandle(None)
        wc = win32gui.WNDCLASS()
        wc.hInstance = hinst
        wc.lpszClassName = class_name
        wc.lpfnWndProc = wnd_proc
        try:
            win32gui.RegisterClass(wc)
        except Exception:
            log_error("register_class_failed", {"class_name": class_name}, exc_info=True)
            raise

        hwnd = win32gui.CreateWindowEx(0, class_name, class_name, 0, 0, 0, 0, 0, 0, 0, hinst, None)
        self.hwnd = int(hwnd)
        self._register_device_notification()
        log_event("detector_started", {"backend": "win32_hidden_window", "initial_drive_count": len(self._baseline)})

        try:
            while not self.stop_event.is_set():
                win32gui.PumpWaitingMessages()
                time.sleep(0.025)
        finally:
            self._cleanup()
            log_event("detector_stopped", {"backend": "win32_hidden_window"})

    def _register_device_notification(self) -> None:
        if user32 is None or not self.hwnd:
            return
        user32.RegisterDeviceNotificationW.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD]
        user32.RegisterDeviceNotificationW.restype = wintypes.HANDLE
        notification_filter = DEV_BROADCAST_DEVICEINTERFACE_W()
        notification_filter.dbcc_size = ctypes.sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)
        notification_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE
        notification_filter.dbcc_reserved = 0
        notification_filter.dbcc_classguid = _usb_device_interface_guid()
        handle = user32.RegisterDeviceNotificationW(self.hwnd, ctypes.byref(notification_filter), DEVICE_NOTIFY_WINDOW_HANDLE)
        if not handle:
            log_error("register_device_notification_failed", {"win32_error": ctypes.get_last_error()})
        else:
            self.notification_handle = int(handle)
            log_event("device_notification_registered", {"class_guid": "GUID_DEVINTERFACE_USB_DEVICE"})

    def _cleanup(self) -> None:
        try:
            import win32gui
        except Exception:
            win32gui = None  # type: ignore[assignment]
        if user32 is not None and self.notification_handle:
            try:
                user32.UnregisterDeviceNotification(wintypes.HANDLE(self.notification_handle))
            except Exception:
                log_error("unregister_device_notification_failed", {}, exc_info=True)
            self.notification_handle = None
        if win32gui is not None and self.hwnd:
            try:
                win32gui.DestroyWindow(self.hwnd)
            except Exception:
                pass
            self.hwnd = None

    def _event_fingerprint(self, action: str, details: dict[str, Any], paths: Sequence[str]) -> str:
        key = json.dumps(
            {
                "action": action,
                "kind": details.get("kind"),
                "paths": list(paths),
                "code": details.get("event_code"),
                "unitmask": details.get("unitmask"),
                "message": details.get("message"),
            },
            sort_keys=True,
            default=str,
        )
        return _hash_id(key)

    def _remember_event(self, event: UsbEvent, max_items: int = 24) -> None:
        with self._dedup_lock:
            self._event_history.append(event)
            if len(self._event_history) > max_items:
                self._event_history = self._event_history[-max_items:]

    def _recent_event_summaries(self, limit: int = 4) -> list[dict[str, Any]]:
        with self._dedup_lock:
            recent_events = list(self._event_history[-limit:])
        summaries: list[dict[str, Any]] = []
        for event in recent_events:
            paths = list(_paths_from_event(event))
            summaries.append(
                {
                    "timestamp_utc": event.timestamp_utc,
                    "timestamp_local": event.timestamp_local,
                    "timezone": event.timezone,
                    "action": event.action,
                    "kind": event.details.get("kind"),
                    "event_name": event.details.get("event_name"),
                    "message": event.details.get("message"),
                    "paths": paths[:4],
                    "path_count": len(paths),
                    "display": event.display,
                }
            )
        return summaries

    def _details_for_emit(self, action: str, details: dict[str, Any]) -> dict[str, Any]:
        # Always enrich remove events with a tiny pre-remove trail. The GUI uses this
        # even when file logging is off, which prevents a fast insert-then-unplug from
        # becoming an unhelpful bare "removed" toast. Structured logs still pass
        # through the normal redaction path.
        if action != "remove":
            return details
        recent_events = self._recent_event_summaries(limit=4)
        if not recent_events:
            return details
        enriched = dict(details)
        enriched["recent_events_before_remove"] = recent_events
        return enriched

    def _emit_once(self, action: str, details: dict[str, Any], paths: Sequence[str], display: bool = True, ttl_s: float = 1.5) -> None:
        fp = self._event_fingerprint(action, details, paths)
        now = time.monotonic()
        with self._dedup_lock:
            for key, ts in list(self._recent.items()):
                if now - ts > 8.0:
                    self._recent.pop(key, None)
            last = self._recent.get(fp)
            duplicate = last is not None and now - last < ttl_s
            if not duplicate:
                self._recent[fp] = now
        if duplicate:
            log_event("usb_event_deduplicated", {"fingerprint": fp, "action": action, "kind": details.get("kind"), "path_count": len(paths)})
            return
        event = emit_usb_event(action, self._details_for_emit(action, details), sink=self.sink, open_paths=paths, display=display)
        self._remember_event(event)

    def _snapshot_delta(self) -> tuple[dict[str, dict[str, Any]], list[str], list[str]]:
        current = _drive_snapshot()
        before = set(self._baseline)
        after = set(current)
        added = sorted(after - before)
        removed = sorted(before - after)
        self._baseline = current
        return current, added, removed

    def _emit_snapshot_delta(self, reason_action: str, details: dict[str, Any], immediate_display_empty_remove: bool = False) -> None:
        with self._scan_lock:
            current, added, removed = self._snapshot_delta()
        if added:
            enriched = dict(details)
            enriched["kind"] = "drive_scan"
            enriched["scan_reason"] = reason_action
            enriched["drive_paths"] = added
            enriched["volumes"] = {path: current.get(path, {}) for path in added}
            self._emit_once("add", enriched, added, display=True, ttl_s=0.25)
        if removed:
            enriched = dict(details)
            enriched["kind"] = "drive_scan"
            enriched["scan_reason"] = reason_action
            enriched["removed_paths"] = removed
            self._emit_once("remove", enriched, removed, display=True, ttl_s=0.25)
        elif immediate_display_empty_remove and not added:
            enriched = dict(details)
            enriched["kind"] = "quick_remove"
            enriched["message"] = "USB 设备已拔出；它可能在系统分配盘符前就被移除。"
            self._emit_once("remove", enriched, [], display=True, ttl_s=0.2)

    def _schedule_drive_scans(self, reason_action: str, details: dict[str, Any], delays: Sequence[float] = (0.05, 0.20, 0.85)) -> None:
        for index, delay in enumerate(delays):
            threading.Thread(
                target=self._delayed_drive_scan,
                args=(reason_action, dict(details), float(delay), index),
                daemon=True,
                name=f"drive-scan-{reason_action}-{index}",
            ).start()

    def _handle_device_change(self, wparam: int, lparam: int) -> None:
        if wparam in (DBT_DEVICEARRIVAL, DBT_DEVICEREMOVECOMPLETE):
            action = "add" if wparam == DBT_DEVICEARRIVAL else "remove"
            details, paths = _details_from_lparam(lparam)
            details["event_code"] = wparam
            details["event_name"] = "DBT_DEVICEARRIVAL" if action == "add" else "DBT_DEVICEREMOVECOMPLETE"

            if action == "remove":
                # Some fast removals arrive without a volume unitmask. Reconcile against the
                # last known snapshot so stale GUI rows are cleared immediately.
                if not paths:
                    self._emit_snapshot_delta("remove", details, immediate_display_empty_remove=True)
                else:
                    self._emit_once("remove", details, paths, display=True, ttl_s=0.2)
                    with self._scan_lock:
                        self._baseline = _drive_snapshot()
                self._schedule_drive_scans("remove", details, delays=(0.10, 0.45))
                return

            if paths:
                with self._scan_lock:
                    self._baseline = _drive_snapshot()
                    current = dict(self._baseline)
                for path in paths:
                    snap = current.get(path)
                    if snap:
                        details.setdefault("volumes", {})[path] = snap
                self._emit_once("add", details, paths, display=True, ttl_s=0.25)
                self._schedule_drive_scans("add", details, delays=(0.25, 0.90))
            else:
                self._last_no_path_arrival_ts = time.monotonic()
                details["message"] = "USB 设备正在枚举，等待系统分配盘符。"
                self._emit_once("change", details, [], display=False, ttl_s=0.25)
                self._schedule_drive_scans("add", details)
            return

        if wparam in (DBT_DEVNODES_CHANGED, DBT_CONFIGCHANGED):
            name = "DBT_DEVNODES_CHANGED" if wparam == DBT_DEVNODES_CHANGED else "DBT_CONFIGCHANGED"
            details = {"kind": "system_device_change", "event_code": wparam, "event_name": name}
            self._emit_once("change", details, [], display=False, ttl_s=0.35)
            self._schedule_drive_scans("change", details, delays=(0.05, 0.25, 0.90))
            return

        self._emit_once("change", {"kind": "unhandled_device_change", "event_code": wparam}, [], display=False, ttl_s=2.0)

    def _delayed_drive_scan(self, action: str, details: dict[str, Any], delay_s: float, pass_index: int = 0) -> None:
        time.sleep(max(0.0, delay_s))
        details = dict(details)
        details["scan_pass"] = pass_index
        self._emit_snapshot_delta(action, details, immediate_display_empty_remove=False)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------


def run_gui(args: argparse.Namespace, app_config: AppConfig, config_store: ConfigStore) -> int:
    try:
        from PySide6.QtCore import QByteArray, QEasingCurve, QObject, QPoint, QRect, QRectF, QTimer, Qt, QEvent, QParallelAnimationGroup, QPropertyAnimation, Signal
        from PySide6.QtGui import QAction, QActionGroup, QColor, QCursor, QIcon, QPalette, QPainter, QPixmap
        from PySide6.QtSvg import QSvgRenderer
        from PySide6.QtWidgets import QApplication, QFrame, QGraphicsDropShadowEffect, QGridLayout, QHBoxLayout, QLabel, QMenu, QPushButton, QProgressBar, QScrollArea, QSizePolicy, QSystemTrayIcon, QVBoxLayout, QWidget
    except ImportError as exc:
        raise SystemExit("GUI mode requires PySide6: py -m pip install PySide6") from exc

    SCALE = 0.85

    def px(value: float) -> int:
        return max(1, int(round(value * SCALE)))

    THEME_LABELS = {"auto": "跟随系统", "dark": "深色", "light": "浅色"}
    LOG_MODE_LABELS = {LogMode.OFF: "关闭", LogMode.REDACTED: "脱敏", LogMode.RAW: "明文/显式"}

    def display_timestamp(event: UsbEvent) -> str:
        try:
            dt = datetime.fromisoformat(event.timestamp_utc).astimezone()
            return dt.strftime("%Y-%m-%d %H:%M:%S %Z")
        except Exception:
            return event.timestamp_local.replace("T", " ")

    GUI_HISTORY_LIMIT = 3
    GUI_HISTORY_MAX_CHARS = 168
    ACTION_SIGNS = {"add": "+", "remove": "-", "change": "±", "error": "!"}
    EVENT_NAME_BY_KIND = {
        "volume": "盘符事件",
        "drive_scan": "盘符刷新",
        "device_interface": "设备接口",
        "system_device_change": "系统设备变更",
        "quick_remove": "快速拔出",
        "manual_scan": "手动扫描",
        "error": "异常",
    }
    EVENT_NAME_BY_CODE = {
        "DBT_DEVICEARRIVAL": "设备插入",
        "DBT_DEVICEREMOVECOMPLETE": "设备拔出",
        "DBT_DEVNODES_CHANGED": "设备节点变更",
        "DBT_CONFIGCHANGED": "配置变更",
    }

    def _fit_gui_text(text: str, max_chars: int = GUI_HISTORY_MAX_CHARS) -> str:
        text = " ".join(str(text).split())
        if len(text) <= max_chars:
            return text
        return text[: max(0, max_chars - 1)].rstrip("；、,，｜| ") + "…"

    def _paths_from_history_item(item: dict[str, Any]) -> list[str]:
        paths = item.get("paths")
        if isinstance(paths, list):
            return [str(path) for path in paths if path]
        return []

    def _event_name_from_parts(action: str, details: dict[str, Any]) -> str:
        event_name = str(details.get("event_name") or "")
        kind = str(details.get("kind") or "")
        if event_name in EVENT_NAME_BY_CODE:
            return EVENT_NAME_BY_CODE[event_name]
        if kind in EVENT_NAME_BY_KIND:
            base = EVENT_NAME_BY_KIND[kind]
        else:
            base = {"add": "设备插入", "remove": "设备拔出", "change": "设备变更", "error": "异常"}.get(action, action or "事件")
        if kind == "drive_scan":
            reason = str(details.get("scan_reason") or "")
            if reason == "add":
                return "盘符新增"
            if reason == "remove":
                return "盘符移除"
            return "盘符刷新"
        return base

    def _event_name_from_item(item: Any) -> str:
        if isinstance(item, UsbEvent):
            return _event_name_from_parts(item.action, item.details)
        if isinstance(item, dict):
            details = {
                "event_name": item.get("event_name"),
                "kind": item.get("kind"),
                "message": item.get("message"),
            }
            return _event_name_from_parts(str(item.get("action") or ""), details)
        return "事件"

    def _change_text(action: str, paths: Sequence[str], empty_hint: str = "无盘符") -> str:
        sign = ACTION_SIGNS.get(action, "±")
        path_list = [str(path) for path in paths if path]
        if path_list:
            detail = "、".join(_display_name_for_path(path) for path in path_list[:2])
            if len(path_list) > 2:
                detail += "等"
        else:
            detail = empty_hint
        return f"{sign} {detail}"

    def compact_history_item(item: Any) -> str:
        if not isinstance(item, dict):
            return _fit_gui_text(str(item), 34)
        action = str(item.get("action") or "change")
        name = _event_name_from_item(item)
        change = _change_text(action, _paths_from_history_item(item))
        return _fit_gui_text(f"{name} {change}", 38)

    def remove_history_text(event: UsbEvent) -> str:
        raw_items = event.details.get("recent_events_before_remove")
        if not isinstance(raw_items, list) or not raw_items:
            return ""
        items = [compact_history_item(item) for item in raw_items[-GUI_HISTORY_LIMIT:]]
        return _fit_gui_text("前序：" + "；".join(item for item in items if item), 92)

    def gui_event_summary(event: UsbEvent, include_history: bool = False) -> str:
        paths = _paths_from_event(event)
        name = _event_name_from_item(event)
        change = _change_text(event.action, paths)
        text = f"事件：{name}｜变动：{change}"
        history = remove_history_text(event) if include_history else ""
        if history:
            text = f"{text}｜{history}"
        return _fit_gui_text(text)

    class Theme:
        def __init__(self, name: str, app: QApplication) -> None:
            requested = name
            if name == "auto":
                bg = app.palette().color(QPalette.Window)
                name = "dark" if bg.lightness() < 128 else "light"
            self.requested = requested
            self.name = name
            self.border_radius = px(14)
            self.row_radius = px(11)
            self.button_radius = px(9)
            if name == "light":
                self.bg = "rgba(0,0,0,0)"
                self.panel = "#fbfcff"
                self.panel2 = "#f3f6fb"
                self.text = "#111827"
                self.muted = "#687386"
                self.border = "#dbe3ef"
                self.accent = "#1769e0"
                self.accent_hover = "#0f5ed2"
                self.progress_bg = "#e6ecf5"
                self.icon_shell = "#ffffff"
                self.icon_socket = "#dce6f4"
                self.icon_line = "#1769e0"
                self.icon_shadow = "#a9b7c8"
                self.shadow_color = QColor(15, 23, 42, 55)
            else:
                self.bg = "rgba(0,0,0,0)"
                self.panel = "#1f2530"
                self.panel2 = "#29313d"
                self.text = "#f6f8fc"
                self.muted = "#b6c0cf"
                self.border = "#3a4656"
                self.accent = "#6ea2ff"
                self.accent_hover = "#8ab6ff"
                self.progress_bg = "#3a4656"
                self.icon_shell = "#2d3746"
                self.icon_socket = "#3d4a5c"
                self.icon_line = "#8ab6ff"
                self.icon_shadow = "#111827"
                self.shadow_color = QColor(0, 0, 0, 145)
            self.ok = "#34c759"
            self.warn = "#ffb020"
            self.err = "#ff5c5c"

        def style_sheet(self) -> str:
            return f"""
            QWidget {{
                background: {self.bg};
                color: {self.text};
                font-family: "Segoe UI", "Microsoft YaHei UI", Arial, sans-serif;
                font-size: {px(13)}px;
            }}
            QFrame#root {{
                background: {self.panel};
                border: 1px solid {self.border};
                border-radius: {self.border_radius}px;
            }}
            QLabel {{ background: transparent; color: {self.text}; }}
            QLabel#headline {{ font-size: {px(16)}px; font-weight: 700; }}
            QLabel#muted, QLabel#summary, QLabel#count, QLabel#pathLabel, QLabel#capacityLabel {{
                color: {self.muted}; font-size: {px(12)}px;
            }}
            QLabel#appIcon, QLabel#driveIcon {{ background: transparent; }}
            QFrame#volumeRow {{
                background: {self.panel2};
                border: 1px solid {self.border};
                border-radius: {self.row_radius}px;
                margin: 1px 0px;
            }}
            QLabel#rowTitle {{
                color: {self.text}; font-size: {px(13)}px; font-weight: 650; background: transparent;
            }}
            QPushButton {{
                background: transparent;
                color: {self.text};
                border: 1px solid {self.border};
                border-radius: {self.button_radius}px;
                padding: {px(8)}px {px(15)}px;
                font-weight: 650;
            }}
            QPushButton:hover {{ background: {self.panel2}; }}
            QPushButton#primaryButton, QPushButton#openButton {{
                background: {self.accent}; color: white; border-color: {self.accent};
            }}
            QPushButton#primaryButton:hover, QPushButton#openButton:hover {{
                background: {self.accent_hover}; border-color: {self.accent_hover};
            }}
            QScrollArea {{ background: transparent; border: 0; }}
            QScrollArea > QWidget > QWidget {{ background: transparent; }}
            QScrollBar:vertical {{ background: transparent; width: {px(8)}px; margin: 1px; }}
            QScrollBar::handle:vertical {{ background: {self.border}; border-radius: {px(4)}px; }}
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
            QProgressBar#usage {{ background: {self.progress_bg}; border: 0; border-radius: {px(4)}px; }}
            QProgressBar#usage::chunk {{ background: {self.accent}; border-radius: {px(4)}px; }}
            """

    def usb_svg(theme: Theme, kind: str) -> str:
        status = {
            "usb": theme.accent,
            "drive": theme.accent,
            "add": theme.ok,
            "change": theme.warn,
            "remove": theme.err,
            "error": theme.err,
        }.get(kind, theme.accent)
        mark = ""
        if kind in {"add", "change", "remove", "error"}:
            if kind == "add":
                mark = '<path d="M44 45l4 4 8-10" fill="none" stroke="white" stroke-width="3.1" stroke-linecap="round" stroke-linejoin="round"/>'
            elif kind == "change":
                mark = '<path d="M43 45h11M50 39l6 6-6 6" fill="none" stroke="white" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>'
            else:
                mark = '<path d="M44 40l10 10M54 40L44 50" fill="none" stroke="white" stroke-width="3" stroke-linecap="round"/>'
        return f'''
        <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
          <defs>
            <linearGradient id="g" x1="12" y1="10" x2="52" y2="58" gradientUnits="userSpaceOnUse">
              <stop offset="0" stop-color="{theme.icon_shell}"/>
              <stop offset="1" stop-color="{theme.panel2}"/>
            </linearGradient>
          </defs>
          <rect x="19" y="5" width="26" height="18" rx="5" fill="{theme.icon_socket}" stroke="{theme.border}" stroke-width="2"/>
          <rect x="24" y="9" width="5" height="7" rx="1.5" fill="{theme.icon_line}" opacity="0.95"/>
          <rect x="35" y="9" width="5" height="7" rx="1.5" fill="{theme.icon_line}" opacity="0.95"/>
          <rect x="13" y="20" width="38" height="35" rx="10" fill="url(#g)" stroke="{theme.border}" stroke-width="2"/>
          <path d="M32 26v16M24 34h16M24 34l-5-5M40 34l5-5" fill="none" stroke="{theme.icon_line}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
          <rect x="20" y="48" width="24" height="3" rx="1.5" fill="{theme.icon_shadow}" opacity="0.35"/>
          <circle cx="50" cy="45" r="11" fill="{status}" stroke="{theme.panel}" stroke-width="3"/>
          {mark}
        </svg>
        '''

    class IconFactory:
        def __init__(self, app: QApplication) -> None:
            self.app = app
            self.cache: dict[tuple[str, str, int, int], QPixmap] = {}

        def dpr(self) -> float:
            screen = self.app.primaryScreen()
            return float(screen.devicePixelRatio() if screen is not None else 1.0) or 1.0

        def pixmap(self, kind: str, theme: Theme, logical_size: int) -> QPixmap:
            ratio = max(1.0, self.dpr())
            physical_size = max(1, int(round(logical_size * ratio)))
            key = (theme.name, kind, int(logical_size), physical_size)
            if key in self.cache:
                return self.cache[key]
            pixmap = QPixmap(physical_size, physical_size)
            pixmap.setDevicePixelRatio(ratio)
            pixmap.fill(Qt.transparent)
            renderer = QSvgRenderer(QByteArray(usb_svg(theme, kind).encode("utf-8")))
            painter = QPainter(pixmap)
            painter.setRenderHint(QPainter.Antialiasing, True)
            painter.setRenderHint(QPainter.SmoothPixmapTransform, True)
            renderer.render(painter, QRectF(0, 0, logical_size, logical_size))
            painter.end()
            self.cache[key] = pixmap
            return pixmap

        def app_icon(self, theme: Theme) -> QIcon:
            icon = QIcon()
            for size in (16, 20, 24, 32, 48, 64, 96, 128, 256):
                icon.addPixmap(self.pixmap("usb", theme, size))
            return icon

    class Bridge(QObject):
        event_received = Signal(object)

    class VolumeRow(QFrame):
        def __init__(
            self,
            group: Sequence[VolumeInfo],
            opener: Callable[[str], None],
            revealer: Callable[[str], None],
            copier: Callable[[str], None],
            ejecter: Callable[[str], None],
            theme: Theme,
            icons: IconFactory,
        ) -> None:
            super().__init__()
            self.group = list(group)
            self.info = self.group[0]
            self.opener = opener
            self.revealer = revealer
            self.copier = copier
            self.ejecter = ejecter
            self.setObjectName("volumeRow")
            self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            self.setMinimumHeight(px(94))
            self.setMaximumHeight(px(101))
            self.setContextMenuPolicy(Qt.CustomContextMenu)
            self.customContextMenuRequested.connect(self.show_context_menu)
            grid = QGridLayout(self)
            grid.setContentsMargins(px(12), px(9), px(12), px(9))
            grid.setHorizontalSpacing(px(10))
            grid.setVerticalSpacing(px(4))

            icon = QLabel()
            icon.setObjectName("driveIcon")
            icon.setAlignment(Qt.AlignCenter)
            icon.setFixedSize(px(34), px(34))
            icon.setPixmap(icons.pixmap("drive", theme, px(31)))
            grid.addWidget(icon, 0, 0, 2, 1)

            row_title = _group_title(self.group)
            title = QLabel(row_title)
            title.setObjectName("rowTitle")
            title.setWordWrap(False)
            title.setToolTip(row_title)
            title.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
            grid.addWidget(title, 0, 1, 1, 1)

            row_subtitle = _group_subtitle(self.group)
            path_label = QLabel(row_subtitle)
            path_label.setObjectName("pathLabel")
            path_label.setToolTip(row_subtitle)
            path_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
            grid.addWidget(path_label, 1, 1, 1, 1)

            button = QPushButton("打开")
            button.setObjectName("openButton")
            button.setMinimumWidth(px(88))
            button.setMinimumHeight(px(42))
            button.clicked.connect(lambda: opener(self.info.path))
            grid.addWidget(button, 0, 2, 2, 1)

            group_total = sum(item.total or 0 for item in self.group) or None
            group_used = sum(item.used or 0 for item in self.group) if all(item.used is not None for item in self.group) else None
            group_free = sum(item.free or 0 for item in self.group) or None
            capacity = QLabel(f"容量 {_format_bytes(group_total)} · 可用 {_format_bytes(group_free)}")
            capacity.setObjectName("capacityLabel")
            grid.addWidget(capacity, 2, 0, 1, 3)

            progress = QProgressBar()
            progress.setObjectName("usage")
            progress.setTextVisible(False)
            progress.setFixedHeight(px(8))
            percent = _usage_percent_raw(group_total, group_used)
            if percent is None:
                progress.setRange(0, 0)
            else:
                progress.setRange(0, 100)
                progress.setValue(percent)
            grid.addWidget(progress, 3, 0, 1, 3)

        def show_context_menu(self, pos: QPoint) -> None:
            menu = QMenu(self)
            open_action = QAction("打开", menu)
            open_action.triggered.connect(lambda: self.opener(self.info.path))
            menu.addAction(open_action)

            reveal_action = QAction("在资源管理器中显示", menu)
            reveal_action.triggered.connect(lambda: self.revealer(self.info.path))
            menu.addAction(reveal_action)

            copy_action = QAction("复制路径", menu)
            all_paths = "、".join(item.path for item in self.group)
            copy_action.triggered.connect(lambda: self.copier(all_paths))
            menu.addAction(copy_action)

            menu.addSeparator()
            eject_label = "安全弹出" if len(self.group) == 1 else "安全弹出（整个设备）"
            eject_action = QAction(eject_label, menu)
            eject_action.triggered.connect(lambda: self.ejecter(self.info.path))
            menu.addAction(eject_action)
            menu.exec(self.mapToGlobal(pos))

    class Toast(QWidget):
        AUTO_HIDE_MS = 10_000
        DEBOUNCE_MS = 140
        FAST_FLUSH_ACTIONS = {"remove", "error"}
        ANIM_MS = 260

        def __init__(self, app: QApplication, theme: Theme, icons: IconFactory, topmost: bool) -> None:
            super().__init__(None)
            self.app = app
            self.theme = theme
            self.icons = icons
            self.events: list[UsbEvent] = []
            self.pending_events: list[UsbEvent] = []
            self.volumes: dict[str, VolumeInfo] = {}
            self.last_paths: list[str] = []
            self.collapsed = True
            self.keep_topmost = topmost
            self._recent_ui: dict[str, float] = {}
            self._animation: Optional[QParallelAnimationGroup] = None
            self._animating_size = False
            self._hiding = False

            self.hide_timer = QTimer(self)
            self.hide_timer.setSingleShot(True)
            self.hide_timer.timeout.connect(self.animate_hide)
            self.debounce_timer = QTimer(self)
            self.debounce_timer.setSingleShot(True)
            self.debounce_timer.timeout.connect(self.flush_pending_events)

            self.setWindowTitle(APP_DISPLAY_NAME)
            self.setWindowIcon(self.icons.app_icon(theme))
            flags = Qt.Tool | Qt.FramelessWindowHint
            if self.keep_topmost:
                flags |= Qt.WindowStaysOnTopHint
            if hasattr(Qt, "WindowDoesNotAcceptFocus"):
                flags |= Qt.WindowDoesNotAcceptFocus
            self.setWindowFlags(flags)
            self.setAttribute(Qt.WA_TranslucentBackground, True)
            self.setStyleSheet(theme.style_sheet())
            self.setFixedSize(*self.target_size())

            outer = QVBoxLayout(self)
            outer.setContentsMargins(px(12), px(12), px(12), px(12))
            self.root = QFrame()
            self.root.setObjectName("root")
            outer.addWidget(self.root)
            shadow = QGraphicsDropShadowEffect(self.root)
            shadow.setBlurRadius(px(28))
            shadow.setOffset(0, px(8))
            shadow.setColor(theme.shadow_color)
            self.root.setGraphicsEffect(shadow)

            layout = QVBoxLayout(self.root)
            layout.setContentsMargins(px(16), px(14), px(16), px(14))
            layout.setSpacing(px(10))

            top = QHBoxLayout()
            self.app_icon = QLabel()
            self.app_icon.setObjectName("appIcon")
            self.app_icon.setAlignment(Qt.AlignCenter)
            self.app_icon.setFixedSize(px(34), px(34))
            self.app_icon.setPixmap(self.icons.pixmap("usb", theme, px(32)))
            top.addWidget(self.app_icon)

            title_box = QVBoxLayout()
            title_box.setSpacing(px(2))
            self.headline = QLabel("USB 设备监控")
            self.headline.setObjectName("headline")
            self.subtitle = QLabel("等待 USB 设备事件")
            self.subtitle.setObjectName("muted")
            title_box.addWidget(self.headline)
            title_box.addWidget(self.subtitle)
            top.addLayout(title_box, 1)

            self.count_label = QLabel("")
            self.count_label.setObjectName("count")
            self.count_label.setAlignment(Qt.AlignRight | Qt.AlignTop)
            top.addWidget(self.count_label)
            layout.addLayout(top)

            self.summary = QLabel("插入 U 盘后会显示可打开位置。")
            self.summary.setObjectName("summary")
            self.summary.setWordWrap(True)
            self.summary.setMaximumHeight(px(40))
            self.summary.setTextInteractionFlags(Qt.TextSelectableByMouse)
            layout.addWidget(self.summary)

            self.scroll = QScrollArea()
            self.scroll.setWidgetResizable(True)
            self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
            self.scroll.setMinimumHeight(px(108))
            self.scroll.setMaximumHeight(px(250))
            self.rows_widget = QWidget()
            self.rows_layout = QVBoxLayout(self.rows_widget)
            self.rows_layout.setContentsMargins(0, 0, 0, 0)
            self.rows_layout.setSpacing(px(8))
            self.scroll.setWidget(self.rows_widget)
            layout.addWidget(self.scroll, 1)

            buttons = QHBoxLayout()
            buttons.setSpacing(px(8))
            self.toggle = QPushButton("展开")
            self.toggle.setMinimumHeight(px(42))
            self.toggle.setMinimumWidth(px(78))
            self.toggle.clicked.connect(self.toggle_expanded)
            self.close_btn = QPushButton("关闭")
            self.close_btn.setMinimumHeight(px(42))
            self.close_btn.setMinimumWidth(px(78))
            self.close_btn.clicked.connect(lambda: self._log_and_hide("toast_close_button"))
            self.primary_open = QPushButton("打开U盘")
            self.primary_open.setObjectName("primaryButton")
            self.primary_open.setMinimumHeight(px(42))
            self.primary_open.setMinimumWidth(px(110))
            self.primary_open.clicked.connect(self.open_first)
            buttons.addWidget(self.toggle)
            buttons.addWidget(self.close_btn)
            buttons.addStretch(1)
            buttons.addWidget(self.primary_open)
            layout.addLayout(buttons)

            self.installEventFilter(self)
            self.root.installEventFilter(self)
            self.refresh()

        def target_size(self) -> tuple[int, int]:
            return (px(510), px(274)) if self.collapsed else (px(510), px(432))

        def _rect_for_size(self, size: tuple[int, int], bottom_right: Optional[QPoint] = None) -> QRect:
            width, height = size
            if bottom_right is None:
                screen = self.app.screenAt(QCursor.pos()) or self.app.primaryScreen()
                if screen is None:
                    return QRect(0, 0, width, height)
                rect = screen.availableGeometry()
                margin = px(18)
                x = rect.right() - width - margin
                y = rect.bottom() - height - margin
                return QRect(max(rect.left() + margin, x), max(rect.top() + margin, y), width, height)
            return QRect(bottom_right.x() - width + 1, bottom_right.y() - height + 1, width, height)

        def _animate_geometry_opacity(self, start: QRect, end: QRect, start_opacity: float, end_opacity: float, finished: Optional[Callable[[], None]] = None, duration: Optional[int] = None) -> None:
            if self._animation is not None:
                self._animation.stop()
            group = QParallelAnimationGroup(self)
            geo = QPropertyAnimation(self, b"geometry", self)
            geo.setDuration(duration or self.ANIM_MS)
            geo.setStartValue(start)
            geo.setEndValue(end)
            geo.setEasingCurve(QEasingCurve.OutCubic)
            opacity = QPropertyAnimation(self, b"windowOpacity", self)
            opacity.setDuration(duration or self.ANIM_MS)
            opacity.setStartValue(start_opacity)
            opacity.setEndValue(end_opacity)
            opacity.setEasingCurve(QEasingCurve.OutCubic)
            group.addAnimation(geo)
            group.addAnimation(opacity)
            if finished is not None:
                group.finished.connect(finished)
            self._animation = group
            group.start()

        def eventFilter(self, obj: QObject, event: QEvent) -> bool:
            if event.type() in (QEvent.MouseButtonPress, QEvent.MouseMove, QEvent.MouseButtonRelease, QEvent.Enter):
                self.hide_timer.start(self.AUTO_HIDE_MS)
            return super().eventFilter(obj, event)

        def _log_and_hide(self, action: str) -> None:
            log_action(action, {"visible": self.isVisible(), "path_count": len(self.last_paths)})
            self.animate_hide()

        def apply_theme(self, theme: Theme) -> None:
            self.theme = theme
            self.setWindowIcon(self.icons.app_icon(theme))
            self.setStyleSheet(theme.style_sheet())
            effect = self.root.graphicsEffect()
            if isinstance(effect, QGraphicsDropShadowEffect):
                effect.setColor(theme.shadow_color)
            self.app_icon.setPixmap(self.icons.pixmap("usb", theme, px(32)))
            self.refresh()

        def set_topmost(self, enabled: bool) -> None:
            was_visible = self.isVisible()
            old_geometry = self.geometry()
            self.keep_topmost = bool(enabled)
            flags = Qt.Tool | Qt.FramelessWindowHint
            if self.keep_topmost:
                flags |= Qt.WindowStaysOnTopHint
            if hasattr(Qt, "WindowDoesNotAcceptFocus"):
                flags |= Qt.WindowDoesNotAcceptFocus
            self.setWindowFlags(flags)
            if was_visible:
                self.setGeometry(old_geometry)
                self.show()
                self.raise_()
                self.ensure_topmost_without_focus()

        def reveal_path(self, path: str) -> None:
            try:
                reveal_in_explorer(path)
                remember_recent_volume(app_config, config_store, path, opened=True)
                log_action("reveal_path", {"path": path, "path_hash": _hash_id(path)})
            except Exception as exc:
                log_error("reveal_path_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": f"定位失败：{exc}"}))

        def copy_path(self, path: str) -> None:
            self.app.clipboard().setText(path)
            self.summary.setText(f"已复制路径：{path}")
            self.summary.setToolTip(path)
            self.hide_timer.start(self.AUTO_HIDE_MS)
            log_action("copy_path", {"path": path, "path_hash": _hash_id(path)})

        def safe_eject_path(self, path: str) -> None:
            try:
                drive = safe_eject_drive(path)
            except Exception as exc:
                log_error("safe_eject_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": str(exc)}))
                return
            log_action("safe_eject_requested", {"path": path, "drive": drive, "path_hash": _hash_id(path)})
            self.summary.setText(f"已请求安全弹出 {drive}，请等待 Windows 完成提示。")
            self.summary.setToolTip(self.summary.text())
            self.hide_timer.start(self.AUTO_HIDE_MS)

        def _set_status_icon(self, action: Optional[str]) -> None:
            kind = {"error": "error", "remove": "remove", "change": "change", "add": "add"}.get(action or "", "usb")
            self.app_icon.setPixmap(self.icons.pixmap(kind, self.theme, px(32)))

        def _fingerprint(self, event: UsbEvent) -> str:
            return _hash_id(json.dumps({"a": event.action, "p": _paths_from_event(event), "k": event.details.get("kind")}, sort_keys=True, default=str))

        def queue_event(self, event: UsbEvent) -> None:
            fp = self._fingerprint(event)
            now = time.monotonic()
            if fp in self._recent_ui and now - self._recent_ui[fp] < 0.45:
                log_event("ui_event_deduplicated", {"fingerprint": fp, "action": event.action})
                return
            self._recent_ui[fp] = now
            self.pending_events.append(event)
            # Removal/error events must not wait behind a debounce window; otherwise a
            # very fast plug-unplug can leave an already-removed drive row visible.
            if event.action in self.FAST_FLUSH_ACTIONS:
                self.debounce_timer.stop()
                QTimer.singleShot(0, self.flush_pending_events)
            else:
                self.debounce_timer.start(self.DEBOUNCE_MS)

        def flush_pending_events(self) -> None:
            if not self.pending_events:
                return
            events = self.pending_events[:]
            self.pending_events.clear()
            self.apply_events(events)
            # Final GUI-side reconciliation: delayed Windows messages can arrive after
            # the physical unplug, so prune stale rows immediately before painting.
            self.prune_missing_volumes()
            self.refresh()
            self.show_toast()
            if any(_paths_from_event(e) for e in events):
                QTimer.singleShot(900, self.refresh_capacity_after_mount)

        def apply_events(self, events: Sequence[UsbEvent]) -> None:
            snapshot: Optional[dict[str, dict[str, Any]]] = None
            for event in events:
                self.events.insert(0, event)
                self.events = self.events[:32]
                paths = _paths_from_event(event)
                if event.action == "remove":
                    if paths:
                        for path in paths:
                            self.volumes.pop(path, None)
                    else:
                        # A no-path removal is common in very fast unplug bursts. Re-scan
                        # current drives and remove any stale rows that no longer exist.
                        current_paths = set(_drive_snapshot())
                        for path in list(self.volumes):
                            if path not in current_paths:
                                self.volumes.pop(path, None)
                    continue
                if event.action in {"add", "change"}:
                    if snapshot is None:
                        snapshot = _drive_snapshot()
                    for path in paths:
                        if path in snapshot:
                            info = _build_volume_info(path, event)
                            self.volumes[path] = info
                            remember_recent_volume(app_config, config_store, info, opened=False)
            if len(self.volumes) > 12:
                self.volumes = dict(list(self.volumes.items())[-12:])
            self.last_paths = list(self.volumes.keys())

        def prune_missing_volumes(self) -> None:
            current_paths = set(_drive_snapshot())
            changed = False
            for path in list(self.volumes):
                if path not in current_paths:
                    self.volumes.pop(path, None)
                    changed = True
            if changed:
                log_event("ui_stale_volume_pruned", {"remaining_count": len(self.volumes)})
            self.last_paths = list(self.volumes.keys())

        def refresh_capacity_after_mount(self) -> None:
            current_paths = set(_drive_snapshot())
            for path, info in list(self.volumes.items()):
                if path not in current_paths:
                    self.volumes.pop(path, None)
                    continue
                total, used, free = _safe_disk_usage(path)
                self.volumes[path] = VolumeInfo(path=info.path, title=info.title, source=info.source, timestamp_utc=info.timestamp_utc, drive_type=info.drive_type, total=total, used=used, free=free, disk_number=info.disk_number)
            self.last_paths = list(self.volumes.keys())
            self.refresh()
            if self.isVisible():
                self.move_to_bottom_right()

        def clear_rows(self) -> None:
            while self.rows_layout.count():
                item = self.rows_layout.takeAt(0)
                widget = item.widget()
                if widget is not None:
                    widget.deleteLater()

        def current_paths(self) -> list[str]:
            return list(self.volumes.keys())

        def refresh(self) -> None:
            self.clear_rows()
            volume_list = list(self.volumes.values())
            groups = _group_volumes_by_device(volume_list)
            latest = self.events[0] if self.events else None
            volume_count = len(groups)
            if latest:
                action_text = {"add": "已连接", "change": "已更新", "remove": "已移除", "error": "异常"}.get(latest.action, latest.action)
                self.headline.setText(f"USB {action_text}")
                self._set_status_icon(latest.action)
            else:
                self.headline.setText("USB 设备监控")
                self._set_status_icon(None)
            if volume_count:
                total = sum(item.total or 0 for item in volume_list)
                free = sum(item.free or 0 for item in volume_list)
                path_preview = "、".join(_group_preview_label(group) for group in groups[:2])
                more = f"，另有 {volume_count - 2} 个" if volume_count > 2 else ""
                if latest and latest.action == "remove":
                    # For unplug events, keep the event line directly under the GUI
                    # timestamp. The remaining connected drives are still shown below.
                    self.subtitle.setText(f"最近事件：{display_timestamp(latest)}")
                    summary = gui_event_summary(latest, include_history=True)
                else:
                    self.subtitle.setText(f"{volume_count} 个可打开位置 · 总计 {_format_bytes(total) if total else '读取中'} · 可用 {_format_bytes(free) if free else '读取中'}")
                    summary = f"可打开：{path_preview}{more}"
                self.summary.setText(summary)
                self.summary.setToolTip(summary)
            elif latest:
                self.subtitle.setText(f"最近事件：{display_timestamp(latest)}")
                if latest.action in {"remove", "add", "change", "error"}:
                    summary = gui_event_summary(latest, include_history=latest.action == "remove")
                else:
                    summary = str(latest.details.get("message") or latest.details.get("kind") or "USB 设备事件")
                self.summary.setText(summary)
                self.summary.setToolTip(summary)
            else:
                self.subtitle.setText("等待 USB 设备事件")
                self.summary.setText("插入 U 盘后会显示可打开位置。")
                self.summary.setToolTip("")
            shown_groups = groups if not self.collapsed else groups[:1]
            for group in shown_groups:
                self.rows_layout.addWidget(VolumeRow(group, self.open_and_hide, self.reveal_path, self.copy_path, self.safe_eject_path, self.theme, self.icons))
            self.rows_layout.addStretch(1)
            self.scroll.setVisible(bool(shown_groups))
            self.primary_open.setVisible(volume_count > 0)
            self.primary_open.setText("打开U盘" if volume_count <= 1 else "打开第一个")
            self.toggle.setVisible(volume_count > 1)
            self.toggle.setText("展开" if self.collapsed else "折叠")
            self.count_label.setText(f"{volume_count} 个" if volume_count else "")
            if not self._animating_size:
                self.setFixedSize(*self.target_size())

        def show_toast(self) -> None:
            self._hiding = False
            end = self._rect_for_size(self.target_size())
            if self.isVisible():
                start = self.geometry()
                start_opacity = max(0.25, self.windowOpacity())
            else:
                start = QRect(end.x(), end.y() + px(16), end.width(), end.height())
                start_opacity = 0.0
                self.setGeometry(start)
                self.setWindowOpacity(0.0)
            self.show()
            self.raise_()
            self.ensure_topmost_without_focus()
            self._animate_geometry_opacity(start, end, start_opacity, 1.0, duration=240)
            self.hide_timer.start(self.AUTO_HIDE_MS)
            log_action("toast_shown", {"volume_count": len(self.volumes), "event_count": len(self.events)})

        def animate_hide(self) -> None:
            if not self.isVisible() or self._hiding:
                return
            self._hiding = True
            self.hide_timer.stop()
            start = self.geometry()
            end = QRect(start.x(), start.y() + px(14), start.width(), start.height())

            def finish() -> None:
                QWidget.hide(self)
                self.setWindowOpacity(1.0)
                self._hiding = False

            self._animate_geometry_opacity(start, end, self.windowOpacity(), 0.0, finish, duration=210)

        def ensure_topmost_without_focus(self) -> None:
            if not self.keep_topmost or user32 is None:
                return
            try:
                hwnd = int(self.winId())
                HWND_TOPMOST = -1
                SWP_NOMOVE = 0x0002
                SWP_NOSIZE = 0x0001
                SWP_NOACTIVATE = 0x0010
                SWP_SHOWWINDOW = 0x0040
                user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)
            except Exception:
                log_error("topmost_failed", {}, exc_info=True)

        def move_to_bottom_right(self) -> None:
            self.setGeometry(self._rect_for_size(self.target_size()))

        def toggle_expanded(self) -> None:
            old_rect = self.geometry() if self.isVisible() else self._rect_for_size(self.target_size())
            old_bottom_right = old_rect.bottomRight()
            self.collapsed = not self.collapsed
            log_action("toast_toggle", {"collapsed": self.collapsed, "volume_count": len(self.volumes)})
            self._animating_size = True
            self.setMinimumSize(1, 1)
            self.setMaximumSize(16777215, 16777215)
            self.refresh()
            target = self._rect_for_size(self.target_size(), bottom_right=old_bottom_right)
            geo = QPropertyAnimation(self, b"geometry", self)
            geo.setDuration(260)
            geo.setStartValue(old_rect)
            geo.setEndValue(target)
            geo.setEasingCurve(QEasingCurve.OutCubic)

            def finish() -> None:
                self._animating_size = False
                self.setFixedSize(*self.target_size())
                self.setGeometry(target)
                self.hide_timer.start(self.AUTO_HIDE_MS)

            geo.finished.connect(finish)
            self._animation = QParallelAnimationGroup(self)
            self._animation.addAnimation(geo)
            self._animation.start()

        def open_first(self) -> None:
            current = set(_drive_snapshot())
            self.last_paths = [path for path in self.current_paths() if path in current]
            self.volumes = {path: info for path, info in self.volumes.items() if path in current}
            paths = self.current_paths()
            if paths:
                self.open_and_hide(paths[0])
            else:
                self.refresh()

        def open_and_hide(self, path: str) -> None:
            self.hide_timer.stop()
            try:
                open_path(path)
            except Exception as exc:
                log_error("open_path_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": f"打开失败：{exc}"}))
                return
            remember_recent_volume(app_config, config_store, path, opened=True)
            log_action("open_path", {"path": path, "path_hash": _hash_id(path)})
            self.animate_hide()

    class TrayOnly(QObject):
        def __init__(self, tray: QSystemTrayIcon) -> None:
            super().__init__()
            self.tray = tray
            self.last_paths: list[str] = []
            self.pending_events: list[UsbEvent] = []
            self.debounce_timer = QTimer(self)
            self.debounce_timer.setSingleShot(True)
            self.debounce_timer.timeout.connect(self.flush)

        def queue_event(self, event: UsbEvent) -> None:
            self.pending_events.append(event)
            if event.action in {"remove", "error"}:
                self.debounce_timer.stop()
                QTimer.singleShot(0, self.flush)
            else:
                self.debounce_timer.start(140)

        def flush(self) -> None:
            if not self.pending_events:
                return
            events = self.pending_events[:]
            self.pending_events.clear()
            paths: list[str] = []
            latest = events[-1]
            for event in events:
                paths.extend(_paths_from_event(event))
            snapshot = _drive_snapshot()
            self.last_paths = [path for path in dict.fromkeys(paths) if path in snapshot]
            for info in current_volume_infos():
                remember_recent_volume(app_config, config_store, info, opened=False)
            title = "USB 设备已更新"
            if latest.action == "remove":
                message = gui_event_summary(latest, include_history=True)
            elif self.last_paths:
                message = "检测到可打开位置：" + "、".join(_display_name_for_path(p) for p in self.last_paths[:3])
            elif latest.action in {"add", "change", "error"}:
                message = gui_event_summary(latest, include_history=False)
            else:
                message = str(latest.details.get("message") or latest.details.get("kind") or "USB 设备事件")
            self.tray.showMessage(title, message, QSystemTrayIcon.Information, 10_000)

    class TrayMenuController(QObject):
        def __init__(self, tray: QSystemTrayIcon, receiver: Any, app: QApplication, theme: Theme, icons: IconFactory) -> None:
            super().__init__()
            self.tray = tray
            self.receiver = receiver
            self.app = app
            self.theme = theme
            self.icons = icons
            self.menu = QMenu()
            self.startup_action: Optional[QAction] = None
            self.log_mode_actions: dict[LogMode, QAction] = {}
            self.reset_on_start_action: Optional[QAction] = None
            self.recent_menu: Optional[QMenu] = None
            self.build_menu()
            # Do not rely on QSystemTrayIcon's native auto-popup: this app keeps a
            # notification window forcefully topmost (SetWindowPos/HWND_TOPMOST),
            # and that can win the Z-order race against the native popup, leaving
            # the part of the menu that overlaps the notification invisible/
            # unclickable. Popping the menu ourselves lets us re-assert topmost on
            # the menu's own window right after showing it, so it always wins.
            tray.activated.connect(self._on_tray_activated)
            self.update_tooltip()

        def _on_tray_activated(self, reason: Any) -> None:
            if reason != QSystemTrayIcon.Context:
                return
            self._popup_menu_on_top()

        def _popup_menu_on_top(self) -> None:
            self.menu.popup(QCursor.pos())
            if user32 is None:
                return
            try:
                hwnd = int(self.menu.winId())
                HWND_TOPMOST = -1
                SWP_NOMOVE = 0x0002
                SWP_NOSIZE = 0x0001
                SWP_NOACTIVATE = 0x0010
                user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            except Exception:
                log_error("menu_topmost_failed", {}, exc_info=True)

        def save_config(self) -> None:
            config_store.save(app_config)

        def update_tooltip(self) -> None:
            current_count = len(_drive_snapshot())
            self.tray.setToolTip(
                f"{APP_DISPLAY_NAME} · 当前：{current_count} 个 · 主题：{THEME_LABELS.get(app_config.theme, app_config.theme)} · 日志：{LOG_MODE_LABELS.get(app_config.log_mode)}"
            )

        def build_menu(self) -> None:
            if app_config.gui_backend == "tray-only":
                open_action = QAction("打开最近的U盘")
                open_action.triggered.connect(self.safe_open_recent)
                self.menu.addAction(open_action)
            else:
                show_action = QAction("显示通知")
                show_action.triggered.connect(lambda: (self.receiver.refresh(), self.receiver.show_toast()))
                open_action = QAction("打开第一个U盘")
                open_action.triggered.connect(self.receiver.open_first)
                self.menu.addAction(show_action)
                self.menu.addAction(open_action)

            rescan_action = QAction("重新扫描当前U盘")
            rescan_action.triggered.connect(self.manual_rescan)
            self.menu.addAction(rescan_action)

            self.recent_menu = self.menu.addMenu("U盘快捷操作")
            self.recent_menu.aboutToShow.connect(self.refresh_recent_menu)
            self.refresh_recent_menu()

            self.menu.addSeparator()
            self.add_logs_menu()
            self.add_theme_menu()

            topmost_action = QAction("通知置顶")
            topmost_action.setCheckable(True)
            topmost_action.setChecked(bool(app_config.topmost))
            topmost_action.toggled.connect(self.apply_topmost)
            self.menu.addAction(topmost_action)

            self.startup_action = QAction("随系统启动")
            self.startup_action.setCheckable(True)
            self.startup_action.setChecked(is_startup_enabled())
            self.startup_action.toggled.connect(self.toggle_startup)
            self.menu.addAction(self.startup_action)

            repair_startup_action = QAction("检查/修复开机启动")
            repair_startup_action.triggered.connect(self.repair_startup_now)
            self.menu.addAction(repair_startup_action)

            open_startup_folder_action = QAction("打开启动目录")
            open_startup_folder_action.triggered.connect(self.open_startup_folder)
            self.menu.addAction(open_startup_folder_action)

            self.menu.addSeparator()
            quit_action = QAction("退出")
            quit_action.triggered.connect(self.app.quit)
            self.menu.addAction(quit_action)

        def _add_disabled_action(self, menu: QMenu, text: str) -> QAction:
            action = QAction(text, menu)
            action.setEnabled(False)
            menu.addAction(action)
            return action

        def _short_recent_title(self, item: dict[str, Any]) -> str:
            title = str(item.get("title") or _display_name_for_path(str(item.get("path") or "")))
            if len(title) > 36:
                title = title[:35].rstrip() + "…"
            return title

        def refresh_recent_menu(self) -> None:
            if self.recent_menu is None:
                return
            self.recent_menu.clear()
            snapshot = _drive_snapshot()
            current_infos = current_volume_infos()
            if current_infos:
                self._add_disabled_action(self.recent_menu, "当前已连接")
                for info in current_infos:
                    open_action = QAction(f"打开 {info.title}", self.recent_menu)
                    open_action.triggered.connect(partial(self.open_volume_path, info.path))
                    self.recent_menu.addAction(open_action)
                    eject_action = QAction(f"安全弹出 {info.path}", self.recent_menu)
                    eject_action.triggered.connect(partial(self.safe_eject_volume, info.path))
                    self.recent_menu.addAction(eject_action)
            else:
                self._add_disabled_action(self.recent_menu, "当前没有检测到U盘")

            self.recent_menu.addSeparator()
            recent = _normalize_recent_volume_records(app_config.recent_volumes)
            if recent:
                self._add_disabled_action(self.recent_menu, "最近使用")
                for item in recent:
                    path = str(item.get("path") or "")
                    attached = path in snapshot
                    suffix = "" if attached else "（未连接）"
                    action = QAction(f"{self._short_recent_title(item)}{suffix}", self.recent_menu)
                    action.setEnabled(attached)
                    action.triggered.connect(partial(self.open_volume_path, path))
                    self.recent_menu.addAction(action)
                self.recent_menu.addSeparator()
                clear_action = QAction("清空最近记录", self.recent_menu)
                clear_action.triggered.connect(self.clear_recent_records)
                self.recent_menu.addAction(clear_action)
            else:
                self._add_disabled_action(self.recent_menu, "暂无最近记录")
            self.update_tooltip()

        def _receiver_paths(self) -> list[str]:
            if hasattr(self.receiver, "current_paths"):
                try:
                    return list(self.receiver.current_paths())
                except Exception:
                    pass
            return list(getattr(self.receiver, "last_paths", []) or [])

        def manual_rescan(self) -> None:
            infos = current_volume_infos()
            for info in infos:
                remember_recent_volume(app_config, config_store, info, opened=False)
            paths = [info.path for info in infos]
            message = "未检测到可打开的U盘。" if not paths else "重新扫描完成：" + "、".join(_display_name_for_path(path) for path in paths[:3])
            event = UsbEvent(action="add" if paths else "change", details={"kind": "manual_scan", "message": message, "drive_paths": paths}, open_paths=tuple(paths))
            if hasattr(self.receiver, "queue_event"):
                self.receiver.queue_event(event)
            self.tray.showMessage(APP_DISPLAY_NAME, message, QSystemTrayIcon.Information, 3500)
            self.refresh_recent_menu()
            log_action("manual_rescan", {"path_count": len(paths), "paths": paths})

        def open_volume_path(self, path: str) -> None:
            snapshot = _drive_snapshot()
            if path not in snapshot:
                self.tray.showMessage(APP_DISPLAY_NAME, f"{path} 当前未连接，无法打开。", QSystemTrayIcon.Warning, 3500)
                log_action("tray_open_skipped_missing", {"path": path})
                self.refresh_recent_menu()
                return
            try:
                open_path(path)
                remember_recent_volume(app_config, config_store, path, opened=True)
                log_action("tray_open_volume", {"path": path, "path_hash": _hash_id(path)})
            except Exception as exc:
                log_error("tray_open_volume_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, f"打开失败：{exc}", QSystemTrayIcon.Warning, 6000)

        def safe_eject_volume(self, path: str) -> None:
            try:
                drive = safe_eject_drive(path)
                self.tray.showMessage(APP_DISPLAY_NAME, f"已请求安全弹出 {drive}，请等待 Windows 完成提示。", QSystemTrayIcon.Information, 3500)
                log_action("tray_safe_eject_requested", {"path": path, "drive": drive, "path_hash": _hash_id(path)})
            except Exception as exc:
                log_error("tray_safe_eject_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, str(exc), QSystemTrayIcon.Warning, 6000)

        def clear_recent_records(self) -> None:
            app_config.recent_volumes = []
            self.save_config()
            self.refresh_recent_menu()
            self.tray.showMessage(APP_DISPLAY_NAME, "最近U盘记录已清空", QSystemTrayIcon.Information, 2500)
            log_action("recent_volumes_cleared", {})

        def add_logs_menu(self) -> None:
            logs_menu = self.menu.addMenu("日志")
            open_logs_action = QAction("打开日志目录")
            open_logs_action.triggered.connect(self.open_logs)
            logs_menu.addAction(open_logs_action)

            reset_now_action = QAction("立即清空日志")
            reset_now_action.triggered.connect(self.reset_logs_now)
            logs_menu.addAction(reset_now_action)
            logs_menu.addSeparator()

            group = QActionGroup(logs_menu)
            group.setExclusive(True)
            for mode, label in ((LogMode.OFF, "关闭日志"), (LogMode.REDACTED, "脱敏日志"), (LogMode.RAW, "明文/显式日志")):
                action = QAction(label, group)
                action.setCheckable(True)
                action.setChecked(app_config.log_mode == mode)
                action.triggered.connect(partial(self.apply_log_mode, mode))
                logs_menu.addAction(action)
                self.log_mode_actions[mode] = action

            logs_menu.addSeparator()
            self.reset_on_start_action = QAction("每次程序打开时重置日志")
            self.reset_on_start_action.setCheckable(True)
            self.reset_on_start_action.setChecked(bool(app_config.reset_logs_on_start))
            self.reset_on_start_action.toggled.connect(self.apply_reset_on_start)
            logs_menu.addAction(self.reset_on_start_action)

        def add_theme_menu(self) -> None:
            theme_menu = self.menu.addMenu("主题")
            theme_group = QActionGroup(theme_menu)
            theme_group.setExclusive(True)
            for key, label in (("auto", "跟随系统"), ("light", "浅色"), ("dark", "深色")):
                action = QAction(label, theme_group)
                action.setCheckable(True)
                action.setChecked(app_config.theme == key)
                action.triggered.connect(partial(self.apply_theme, key))
                theme_menu.addAction(action)

        def safe_open_recent(self) -> None:
            snapshot = _drive_snapshot()
            paths = [path for path in self._receiver_paths() if path in snapshot]
            if not paths:
                paths = [info.path for info in current_volume_infos()]
            if not paths:
                for item in _normalize_recent_volume_records(app_config.recent_volumes):
                    path = str(item.get("path") or "")
                    if path in snapshot:
                        paths.append(path)
                        break
            if paths:
                self.open_volume_path(paths[0])
            else:
                self.tray.showMessage(APP_DISPLAY_NAME, "当前没有可打开的U盘。", QSystemTrayIcon.Information, 3000)

        def open_logs(self) -> None:
            path = str(app_config.log_dir)
            try:
                app_config.log_dir.mkdir(parents=True, exist_ok=True)
                open_path(path)
                log_action("open_log_dir", {"path": path})
            except Exception as exc:
                log_error("open_log_dir_failed", {"path": path, "message": str(exc)}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, f"打开日志目录失败：{exc}", QSystemTrayIcon.Warning, 6000)

        def reset_logs_now(self) -> None:
            try:
                LOGGER_MANAGER.stop()
                LOGGER_MANAGER.reset_log_files(app_config.log_dir)
                LOGGER_MANAGER.configure(LogConfig(app_config.log_dir, app_config.log_mode, app_config.log_max_bytes, app_config.log_backups, app_config.console_log), reset_logs=False)
                self.tray.showMessage(APP_DISPLAY_NAME, "日志已清空", QSystemTrayIcon.Information, 2500)
                log_action("logs_reset_now", {"mode": app_config.log_mode.value})
            except Exception as exc:
                self.tray.showMessage(APP_DISPLAY_NAME, f"清空日志失败：{exc}", QSystemTrayIcon.Warning, 6000)

        def apply_log_mode(self, mode: LogMode) -> None:
            app_config.log_mode = mode
            self.save_config()
            LOGGER_MANAGER.set_mode(mode)
            for item_mode, action in self.log_mode_actions.items():
                action.setChecked(item_mode == mode)
            self.update_tooltip()
            message = "日志已关闭" if mode == LogMode.OFF else f"日志模式：{LOG_MODE_LABELS[mode]}"
            self.tray.showMessage(APP_DISPLAY_NAME, message, QSystemTrayIcon.Information, 2500)
            log_action("log_mode_changed", {"mode": mode.value})

        def apply_reset_on_start(self, enabled: bool) -> None:
            app_config.reset_logs_on_start = bool(enabled)
            self.save_config()
            log_action("reset_logs_on_start_changed", {"enabled": bool(enabled)})

        def apply_theme(self, theme_name: str) -> None:
            app_config.theme = theme_name
            self.save_config()
            new_theme = Theme(theme_name, self.app)
            self.theme = new_theme
            if hasattr(self.receiver, "apply_theme"):
                self.receiver.apply_theme(new_theme)
            self.tray.setIcon(self.icons.app_icon(new_theme))
            self.app.setWindowIcon(self.icons.app_icon(new_theme))
            self.update_tooltip()
            log_action("theme_changed", {"theme": theme_name})

        def apply_topmost(self, enabled: bool) -> None:
            app_config.topmost = bool(enabled)
            self.save_config()
            if hasattr(self.receiver, "set_topmost"):
                self.receiver.set_topmost(bool(enabled))
            elif hasattr(self.receiver, "keep_topmost"):
                self.receiver.keep_topmost = bool(enabled)
            log_action("topmost_changed", {"enabled": bool(enabled)})

        def open_startup_folder(self) -> None:
            try:
                _startup_folder_path().mkdir(parents=True, exist_ok=True)
                open_path(str(_startup_folder_path()))
                log_action("open_startup_folder", {"path": str(_startup_folder_path())})
            except Exception as exc:
                log_error("open_startup_folder_failed", {"message": str(exc)}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, f"打开启动目录失败：{exc}", QSystemTrayIcon.Warning, 6000)

        def repair_startup_now(self) -> None:
            try:
                before = startup_status_report()
                method = set_startup_enabled(True)
                after = startup_status_report()
                message = "开机启动已修复" if after.get("healthy") else "已重新写入启动项，请在任务管理器中确认已启用"
                self.tray.showMessage(APP_DISPLAY_NAME, message, QSystemTrayIcon.Information, 5000)
                log_action("startup_repair_clicked", {"method": method, "before": before, "after": after})
            except Exception as exc:
                log_error("startup_repair_failed", {"message": str(exc), "command": startup_command_preview()}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, f"修复开机启动失败：{exc}", QSystemTrayIcon.Warning, 7000)
            finally:
                if self.startup_action is not None:
                    self.startup_action.blockSignals(True)
                    self.startup_action.setChecked(is_startup_enabled())
                    self.startup_action.blockSignals(False)

        def toggle_startup(self, enabled: bool) -> None:
            try:
                method = set_startup_enabled(bool(enabled))
            except Exception as exc:
                log_error("startup_toggle_failed", {"enabled": enabled, "message": str(exc), "command": startup_command_preview()}, exc_info=True)
                self.tray.showMessage(APP_DISPLAY_NAME, f"设置开机启动失败：{exc}", QSystemTrayIcon.Warning, 6000)
            else:
                report = startup_status_report()
                log_action("startup_changed", {"enabled": is_startup_enabled(), "method": method, "command": startup_command_preview(), "status": report})
                if enabled:
                    text = "已开启开机启动：快捷方式 + Run 兜底" if report.get("healthy") else "已写入开机启动，请检查任务管理器是否启用"
                else:
                    text = "已关闭开机启动"
                self.tray.showMessage(APP_DISPLAY_NAME, text, QSystemTrayIcon.Information, 3500)
            finally:
                if self.startup_action is not None:
                    self.startup_action.blockSignals(True)
                    self.startup_action.setChecked(is_startup_enabled())
                    self.startup_action.blockSignals(False)

    app = QApplication(sys.argv[:1])
    app.setApplicationName(APP_DISPLAY_NAME)
    app.setOrganizationName(APP_ORG)
    app.setQuitOnLastWindowClosed(False)

    theme = Theme(app_config.theme, app)
    icons = IconFactory(app)
    app.setWindowIcon(icons.app_icon(theme))
    bridge = Bridge()

    tray: Optional[QSystemTrayIcon] = None
    if QSystemTrayIcon.isSystemTrayAvailable():
        tray = QSystemTrayIcon(icons.app_icon(theme))
        tray.setToolTip(APP_DISPLAY_NAME)
        tray.show()
        app._usb_monitor_tray = tray  # type: ignore[attr-defined]

    if app_config.gui_backend == "tray-only":
        if tray is None:
            raise SystemExit("tray-only backend requires a system tray")
        receiver: Any = TrayOnly(tray)
        try:
            bridge.event_received.connect(receiver.queue_event, type=Qt.ConnectionType.QueuedConnection)
        except TypeError:
            bridge.event_received.connect(receiver.queue_event)
    else:
        toast = Toast(app, theme, icons, app_config.topmost)
        try:
            bridge.event_received.connect(toast.queue_event, type=Qt.ConnectionType.QueuedConnection)
        except TypeError:
            bridge.event_received.connect(toast.queue_event)
        app._usb_monitor_toast = toast  # type: ignore[attr-defined]
        receiver = toast

    if tray is not None:
        app._usb_monitor_menu = TrayMenuController(tray, receiver, app, theme, icons)  # type: ignore[attr-defined]

    sink = lambda event: bridge.event_received.emit(event)
    detector = Win32UsbDetector(sink=sink)
    app._usb_monitor_detector = detector  # type: ignore[attr-defined]
    detector.start()

    def stop_detector() -> None:
        detector.stop()
        detector.join(timeout=1.0)
        log_action("app_quit", {"reason": "qt_about_to_quit"})

    app.aboutToQuit.connect(stop_detector)
    log_action("app_started", {"gui_backend": app_config.gui_backend, "theme": app_config.theme, "topmost": app_config.topmost, "log_mode": app_config.log_mode.value, "ui_scale": SCALE, "startup_arg": bool(args.startup)})
    return int(app.exec())


# ---------------------------------------------------------------------------
# Console mode
# ---------------------------------------------------------------------------


def run_console(args: argparse.Namespace) -> int:
    if platform.system() != "Windows":
        print("This Windows-only branch must run on Windows.", file=sys.stderr)
        return 2

    def sink(event: UsbEvent) -> None:
        print(json.dumps(sanitize_for_log({"action": event.action, "details": event.details, "open_paths": event.open_paths}, raw=LOGGER_MANAGER.raw_logs), ensure_ascii=False, default=str))

    detector = Win32UsbDetector(sink=sink)
    detector.start()
    log_action("console_started", {})
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        detector.stop()
        detector.join(timeout=1.0)
        log_action("console_stopped", {"reason": "keyboard_interrupt"})
    return 0


# ---------------------------------------------------------------------------
# CLI and entry point
# ---------------------------------------------------------------------------


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Windows USB monitor with rounded PySide6 GUI and runtime-switchable logging.")
    parser.add_argument("--log-dir", type=Path, default=None, help="Directory for events/actions/errors/crash logs.")
    parser.add_argument("--log-mode", choices=("off", "redacted", "raw"), default=None, help="Logging mode: off, redacted, or raw/explicit.")
    parser.add_argument("--log-raw", action="store_true", help="Compatibility alias for --log-mode raw.")
    parser.add_argument("--log-off", action="store_true", help="Compatibility alias for --log-mode off.")
    parser.add_argument("--reset-logs-on-start", action="store_true", default=None, help="Clear log files when the program starts.")
    parser.add_argument("--no-reset-logs-on-start", dest="reset_logs_on_start", action="store_false", help="Do not clear log files when the program starts.")
    parser.add_argument("--log-max-bytes", type=int, default=None, help="Max bytes per rotating log file.")
    parser.add_argument("--log-backups", type=int, default=None, help="Number of rotated log files to keep.")
    parser.add_argument("--console-log", action="store_true", default=None, help="Also log to stderr, useful during debugging.")
    parser.add_argument("--no-gui", action="store_true", help="Run in console diagnostic mode.")
    parser.add_argument("--gui-backend", choices=("qt-toast", "tray-only"), default=None, help="GUI backend.")
    parser.add_argument("--theme", choices=("auto", "dark", "light"), default=None, help="Toast theme.")
    parser.add_argument("--topmost", dest="topmost", action="store_true", default=None, help="Keep toast above normal windows.")
    parser.add_argument("--no-topmost", dest="topmost", action="store_false", help="Do not keep toast always on top.")
    parser.add_argument("--allow-multiple", action="store_true", help="Allow multiple monitor instances. Not recommended.")
    parser.add_argument("--install-startup", action="store_true", help="Install/repair Windows user-login startup and exit.")
    parser.add_argument("--uninstall-startup", action="store_true", help="Remove Windows user-login startup and exit.")
    parser.add_argument("--startup-status", action="store_true", help="Print Windows startup registration status and exit.")
    parser.add_argument("--startup", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def merge_cli_config(args: argparse.Namespace, stored: AppConfig) -> AppConfig:
    cfg = AppConfig(**stored.__dict__)
    if args.log_dir is not None:
        cfg.log_dir = args.log_dir
    if args.log_mode is not None:
        cfg.log_mode = LogMode.normalize(args.log_mode)
    if args.log_raw:
        cfg.log_mode = LogMode.RAW
    if args.log_off:
        cfg.log_mode = LogMode.OFF
    if args.reset_logs_on_start is not None:
        cfg.reset_logs_on_start = bool(args.reset_logs_on_start)
    if args.log_max_bytes is not None:
        cfg.log_max_bytes = max(int(args.log_max_bytes), 10_000)
    if args.log_backups is not None:
        cfg.log_backups = max(int(args.log_backups), 0)
    if args.console_log is not None:
        cfg.console_log = bool(args.console_log)
    if args.gui_backend is not None:
        cfg.gui_backend = args.gui_backend
    if args.theme is not None:
        cfg.theme = args.theme
    if args.topmost is not None:
        cfg.topmost = bool(args.topmost)
    return cfg


def _notify_second_instance_blocked() -> None:
    if user32 is None:
        return
    try:
        MB_ICONINFORMATION = 0x00000040
        MB_SETFOREGROUND = 0x00010000
        MB_TOPMOST = 0x00040000
        user32.MessageBoxW(
            None,
            f"{APP_DISPLAY_NAME} 已经在运行。\n\n请在任务栏右下角系统托盘（可能需要点击“显示隐藏的图标”）找到它的图标并右键查看菜单，"
            "而不是再启动一份新的实例。",
            APP_DISPLAY_NAME,
            MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST,
        )
    except Exception:
        pass


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    store = ConfigStore(config_path())
    app_config = merge_cli_config(args, store.load())
    try:
        store.save(app_config)
    except Exception as exc:
        print(f"[{APP_DISPLAY_NAME}] warning: failed to save initial config: {exc}", file=sys.stderr)

    LOGGER_MANAGER.configure(
        LogConfig(
            log_dir=app_config.log_dir,
            mode=app_config.log_mode,
            max_bytes=max(int(app_config.log_max_bytes), 10_000),
            backup_count=max(int(app_config.log_backups), 0),
            console_log=bool(app_config.console_log),
        ),
        reset_logs=bool(app_config.reset_logs_on_start),
    )
    atexit.register(stop_logging)

    if platform.system() != "Windows":
        log_error("unsupported_os", {"os": platform.system()})
        print("This branch is Windows-only.", file=sys.stderr)
        return 2

    if args.startup_status:
        print(json.dumps(startup_status_report(), ensure_ascii=False, indent=2, default=str))
        return 0
    if args.install_startup:
        method = set_startup_enabled(True)
        print(json.dumps({"enabled": True, "method": method, "status": startup_status_report()}, ensure_ascii=False, indent=2, default=str))
        return 0
    if args.uninstall_startup:
        method = set_startup_enabled(False)
        print(json.dumps({"enabled": False, "method": method, "status": startup_status_report()}, ensure_ascii=False, indent=2, default=str))
        return 0

    try:
        migrated = repair_startup_registration_if_needed()
        if migrated:
            log_action("startup_migrated", {"method": migrated})
    except Exception as exc:
        log_error("startup_migration_failed", {"message": str(exc)}, exc_info=True)
    if not args.allow_multiple and not acquire_single_instance_lock():
        log_event("second_instance_blocked", {})
        _notify_second_instance_blocked()
        return 0
    try:
        if args.no_gui:
            return run_console(args)
        return run_gui(args, app_config, store)
    except Exception as exc:
        log_error("main_failed", {"message": str(exc)}, exc_info=True)
        return 1
    finally:
        stop_logging()


if __name__ == "__main__":
    sys.exit(main())
