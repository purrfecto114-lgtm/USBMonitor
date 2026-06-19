"""Application constants, paths, and persistent configuration."""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any

APP_NAME = "USBMonitor"
APP_ORG = "BellaKipping"
APP_DISPLAY_NAME = "USB Monitor"
STARTUP_REG_NAME = APP_NAME
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
STARTUP_SCRIPT_FILENAME = f"{APP_NAME}.py"
STARTUP_EXE_FILENAME = f"{APP_NAME}.exe"

# Keys whose values are redacted in non-raw logs.
SENSITIVE_KEYS = {
    "serial", "serial_num", "id_serial", "id_serial_short", "uuid", "id_fs_uuid",
    "label", "volume_label", "device_path", "device_node", "sys_path", "mount_point",
    "drive_paths", "removed_volumes", "removed_paths", "open_paths", "path", "paths",
    "raw_path", "_name", "location_id", "volume_serial", "device_instance_id",
}


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
class LogConfig:
    log_dir: Path
    mode: LogMode = LogMode.REDACTED
    max_bytes: int = 1_000_000
    backup_count: int = 5
    console_log: bool = False


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


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

def app_data_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA")
    if base:
        return Path(base) / APP_NAME
    return Path.home() / "AppData" / "Local" / APP_NAME


def default_log_dir() -> Path:
    return app_data_dir() / "logs"


def config_path() -> Path:
    return app_data_dir() / "config.json"


# ---------------------------------------------------------------------------
# Small utilities used across modules
# ---------------------------------------------------------------------------

def hash_id(value: Any) -> str:
    return hashlib.sha256(str(value).encode("utf-8", errors="replace")).hexdigest()[:12]


def coerce_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in {"1", "true", "yes", "y", "on", "enable", "enabled"}


def safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def normalise_drive_key(path: Any) -> str:
    text = str(path or "").strip().replace("/", "\\")
    if len(text) >= 2 and text[1] == ":":
        return text[:2].upper() + "\\"
    return text.rstrip("\\").lower()


# ---------------------------------------------------------------------------
# Config persistence
# ---------------------------------------------------------------------------

def display_name_for_path(path: str) -> str:
    """Return a stable, user-facing label for a drive path."""
    text = str(path or "").strip()
    if text.endswith(":\\"):
        return f"移动磁盘 {text}"
    return text


def _normalize_recent_volume_records(value: Any, limit: int = 8) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        return []
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            continue
        original_path = str(item.get("path") or "").strip()
        key = normalise_drive_key(original_path)
        if not key or key in seen:
            continue
        seen.add(key)
        path = key if len(key) == 3 and key[1] == ":" else original_path
        fallback_title = display_name_for_path(path)
        records.append(
            {
                "path": path,
                "title": str(item.get("title") or fallback_title),
                "drive_type": str(item.get("drive_type") or "unknown"),
                "last_seen_utc": str(item.get("last_seen_utc") or item.get("timestamp_utc") or ""),
                "last_seen_local": str(item.get("last_seen_local") or ""),
                "open_count": max(safe_int(item.get("open_count"), 0), 0),
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
                loaded = json.loads(self.path.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    raw = loaded
        except Exception:
            raw = {}

        theme = str(raw.get("theme") or "auto")
        if theme not in {"auto", "light", "dark"}:
            theme = "auto"

        backend = str(raw.get("gui_backend") or "qt-toast")
        if backend not in {"qt-toast", "tray-only"}:
            backend = "qt-toast"

        return AppConfig(
            log_dir=Path(raw.get("log_dir") or default_log_dir()),
            log_mode=LogMode.normalize(raw.get("log_mode", LogMode.REDACTED.value)),
            reset_logs_on_start=coerce_bool(raw.get("reset_logs_on_start"), False),
            log_max_bytes=max(safe_int(raw.get("log_max_bytes"), 1_000_000), 10_000),
            log_backups=max(safe_int(raw.get("log_backups"), 5), 0),
            console_log=coerce_bool(raw.get("console_log"), False),
            theme=theme,
            topmost=coerce_bool(raw.get("topmost"), True),
            gui_backend=backend,
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
