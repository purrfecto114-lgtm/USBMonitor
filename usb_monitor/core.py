"""Pure functions, models, and config — testable without Qt/Win32.

This module is the dependency-free core of USB Monitor. Anything that touches
``ctypes``, ``PySide6``, the registry, or the filesystem stays in
``usb_monitor.py``. Functions and classes here must be:

  * deterministic for given inputs (no hidden IO)
  * thread-safe (or immutable)
  * free of platform-specific imports

The split keeps a fast test loop and gives future refactors a safety net.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any, Iterable, Mapping, Optional, Sequence


# ---------------------------------------------------------------------------
# Time helpers
# ---------------------------------------------------------------------------


def now_utc() -> str:
    """ISO-8601 UTC timestamp with second precision."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def now_local() -> str:
    """ISO-8601 local timestamp with second precision."""
    return datetime.now().astimezone().isoformat(timespec="seconds")


# ---------------------------------------------------------------------------
# Enums and models
# ---------------------------------------------------------------------------


class LogMode(str, Enum):
    OFF = "off"
    REDACTED = "redacted"
    RAW = "raw"

    @classmethod
    def parse(cls, value: Any) -> "LogMode":
        text = str(value or cls.REDACTED.value).strip().lower()
        if text in {"off", "0", "false", "none", "disabled"}:
            return cls.OFF
        if text in {"raw", "plain", "full", "unredacted", "明文", "显式"}:
            return cls.RAW
        return cls.REDACTED


@dataclass(frozen=True)
class VolumeInfo:
    path: str
    title: str
    drive_type: str
    disk_number: Optional[int]
    total: Optional[int]
    used: Optional[int]
    free: Optional[int]
    label: str = ""


@dataclass(frozen=True)
class UsbEvent:
    action: str
    changed_paths: tuple[str, ...]
    snapshot: tuple[VolumeInfo, ...]
    details: Mapping[str, Any] = field(default_factory=dict)
    display: bool = True
    timestamp_utc: str = field(default_factory=lambda: now_utc())

    @property
    def timestamp_local(self) -> str:
        try:
            return datetime.fromisoformat(self.timestamp_utc).astimezone().isoformat(timespec="seconds")
        except ValueError:
            return now_local()


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------


@dataclass
class AppConfig:
    # fields with non-core defaults are filled by ConfigStore.load — we
    # declare them here so core stays self-contained.
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
    hooks: list[dict[str, Any]] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Coercion helpers
# ---------------------------------------------------------------------------


def as_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in {"1", "true", "yes", "y", "on", "enable", "enabled"}


def as_int(value: Any, default: int, minimum: Optional[int] = None) -> int:
    try:
        result = int(value)
    except (TypeError, ValueError, OverflowError):
        result = default
    return max(result, minimum) if minimum is not None else result


def hash_id(value: Any) -> str:
    return hashlib.sha256(str(value).encode("utf-8", errors="replace")).hexdigest()[:12]


# ---------------------------------------------------------------------------
# Path normalization
# ---------------------------------------------------------------------------


def normalize_drive_path(value: Any) -> str:
    """Return canonical form for a Windows drive path.

    Examples
    --------
    >>> normalize_drive_path("e:/")
    'E:\\\\'
    >>> normalize_drive_path("E:\\\\Some\\\\Path")
    'E:\\\\'
    >>> normalize_drive_path(None)
    ''
    """
    text = str(value or "").strip().replace("/", "\\")
    if len(text) >= 2 and text[1] == ":":
        return text[:2].upper() + "\\"
    return text.rstrip("\\").casefold()


def display_name_for_path(path: str) -> str:
    return f"移动磁盘 {path}" if path.endswith(":\\") else path




def normalize_hook_rules(value: Any, limit: int = 20) -> list[dict[str, Any]]:
    """Return a JSON-safe, bounded set of opt-in automation hook rules.

    Invalid or incomplete rules are ignored rather than breaking application
    startup.  Commands remain argv arrays so callers can execute them with
    ``shell=False`` semantics.
    """
    if not isinstance(value, list) or int(limit) <= 0:
        return []
    result: list[dict[str, Any]] = []
    seen_names: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "").strip()[:80]
        if not name or name.casefold() in seen_names:
            continue
        command_raw = item.get("command")
        if not isinstance(command_raw, (list, tuple)):
            continue
        command = [str(token).strip() for token in command_raw if str(token).strip()]
        if not command or len(command) > 32 or any(len(token) > 2048 for token in command):
            continue

        def patterns(key: str) -> list[str]:
            raw = item.get(key)
            if not isinstance(raw, (list, tuple)):
                return []
            return [str(pattern).strip()[:260] for pattern in raw if str(pattern).strip()][:20]

        try:
            debounce = float(item.get("debounce_seconds", 2.0))
        except (TypeError, ValueError):
            debounce = 2.0
        debounce = min(max(debounce, 0.1), 3600.0)
        normalized = {
            "name": name,
            "match_paths": patterns("match_paths"),
            "match_labels": patterns("match_labels"),
            "command": command,
            "debounce_seconds": debounce,
            "enabled": as_bool(item.get("enabled"), True),
        }
        result.append(normalized)
        seen_names.add(name.casefold())
        if len(result) >= max(0, int(limit)):
            break
    return result

def normalize_recent_records(value: Any, limit: int = 10) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        return []
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, dict):
            continue
        path = normalize_drive_path(item.get("path"))
        if not path or path in seen:
            continue
        seen.add(path)
        result.append(
            {
                "path": path,
                "title": str(item.get("title") or display_name_for_path(path)),
                "drive_type": str(item.get("drive_type") or "unknown"),
                "last_seen_utc": str(item.get("last_seen_utc") or ""),
                "last_seen_local": str(item.get("last_seen_local") or ""),
                "open_count": as_int(item.get("open_count"), 0, 0),
                "total": item.get("total") if isinstance(item.get("total"), int) else None,
                "free": item.get("free") if isinstance(item.get("free"), int) else None,
            }
        )
    result.sort(key=lambda item: item["last_seen_utc"], reverse=True)
    return result[:limit]


# ---------------------------------------------------------------------------
# Display formatting
# ---------------------------------------------------------------------------


def format_bytes(value: Optional[int]) -> str:
    """Human-readable byte size. None -> '未知'."""
    if value is None:
        return "未知"
    size = float(value)
    for unit in ("B", "KB", "MB", "GB", "TB", "PB"):
        if abs(size) < 1024 or unit == "PB":
            return f"{size:.0f} {unit}" if unit in {"B", "KB"} else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


def group_volumes(volumes: Sequence[VolumeInfo]) -> list[list[VolumeInfo]]:
    """Group volumes that share the same underlying physical disk.

    Volumes without a disk_number are grouped by their own path so they always
    render as standalone entries.
    """
    groups: dict[tuple, list[VolumeInfo]] = {}
    order: list[tuple] = []
    for info in volumes:
        key = ("disk", info.disk_number) if info.disk_number is not None else ("path", info.path)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(info)
    return [groups[key] for key in order]


def group_title(group: Sequence[VolumeInfo]) -> str:
    if len(group) == 1:
        return group[0].title
    label = group[0].label or "USB 存储设备"
    return f"{label} · {'、'.join(item.path for item in group)}（{len(group)} 个分区）"


def event_summary(event: UsbEvent) -> str:
    names = {"add": "已连接", "remove": "已移除", "change": "已更新", "error": "异常"}
    name = names.get(event.action, event.action)
    if event.changed_paths:
        paths = "、".join(event.changed_paths[:3])
        suffix = " 等" if len(event.changed_paths) > 3 else ""
        return f"USB {name}：{paths}{suffix}"
    message = str(event.details.get("message") or "")
    return message or f"USB {name}"


# ---------------------------------------------------------------------------
# Sensitive field redaction
# ---------------------------------------------------------------------------


SENSITIVE_KEYS = frozenset(
    {
        "serial",
        "serial_num",
        "uuid",
        "label",
        "volume_label",
        "device_path",
        "path",
        "paths",
        "changed_paths",
        "snapshot",
        "open_paths",
        "run_value",
        "expected_command",
    }
)


def redact(value: Any) -> Any:
    """Replace ``value`` with a stable but opaque fingerprint."""
    if value is None:
        return None
    if isinstance(value, Mapping):
        return {str(key): redact(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [f"redacted:{hash_id(item)}" for item in value]
    text = str(value)
    return "" if not text else f"redacted:{hash_id(text)}"


def sanitize_for_log(value: Any, raw: bool = False) -> Any:
    """Recursively replace values of sensitive keys when ``raw`` is False."""
    if raw:
        return value
    if isinstance(value, Mapping):
        clean: dict[str, Any] = {}
        for key, item in value.items():
            name = str(key)
            if name.casefold() in SENSITIVE_KEYS:
                clean[name] = redact(item)
            else:
                clean[name] = sanitize_for_log(item, raw=False)
        return clean
    if isinstance(value, (list, tuple, set)):
        return [sanitize_for_log(item, raw=False) for item in value]
    return value


# ---------------------------------------------------------------------------
# Window geometry
# ---------------------------------------------------------------------------


def precise_percent(used: Optional[int], total: Optional[int]) -> Optional[float]:
    """Return usage percentage with one decimal place, clamped to [0, 100].

    Returns ``None`` when total is missing/zero/negative so callers can fall
    back to indeterminate UI (e.g. marquee progress bars). Negative ``used``
    is treated as 0 because filesystem usage is never negative.
    """
    if total is None or total <= 0 or used is None:
        return None
    ratio = max(0.0, float(used)) / float(total)
    return round(min(1.0, ratio) * 100.0, 1)


def progress_tooltip_text(total: Optional[int], used: Optional[int], free: Optional[int]) -> str:
    """Compose a hover tooltip describing a volume's capacity usage.

    Layout: 3 lines so a Windows tooltip can wrap cleanly.
      Line 1: 已用 X · 剩余 Y
      Line 2: 占总量 Z%
      Line 3: 总量 N (raw bytes if user wants the exact figure)
    """
    if total is None or total <= 0:
        return "容量：未知"
    pct = precise_percent(used, total)
    pct_text = f"{pct:.1f}%" if pct is not None else "未知占比"
    free_text = format_bytes(free) if free is not None else "未知"
    used_text = format_bytes(used) if used is not None else "未知"
    return (
        f"已用 {used_text} · 剩余 {free_text}\n"
        f"占总量 {pct_text}\n"
        f"总量 {total:,} 字节"
    )


def countdown_label(remaining_ms: int) -> str:
    """Render remaining milliseconds as a short 'Ns 后关闭' label.

    Cutover: at >= 60s remaining we show 'N 分钟后…', otherwise 'N 秒后…'.
    The ceiling rounds sub-second values up so 500ms still reads as '1 秒后'.
    """
    if remaining_ms <= 0:
        return "即将关闭"
    if remaining_ms >= 60_000:
        minutes = (remaining_ms + 59_999) // 60_000
        return f"{minutes} 分钟后自动关闭"
    seconds = (remaining_ms + 999) // 1000
    return f"{seconds} 秒后自动关闭"


def anchored_window_geometry(
    work_area: tuple[int, int, int, int],
    window_size: tuple[int, int],
    margin: int,
) -> tuple[int, int, int, int]:
    """Clamp a bottom-right notification window inside a monitor work area.

    Coordinates are expressed in the same logical coordinate system as Qt.
    The returned tuple is ``(x, y, width, height)`` and never extends into
    taskbar-reserved space, including monitors with negative virtual-desktop
    coordinates or taskbars placed on the top/left edges.
    """
    wx, wy, ww, wh = (int(value) for value in work_area)
    requested_width, requested_height = (int(value) for value in window_size)
    if ww <= 0 or wh <= 0:
        raise ValueError(f"invalid work area: {work_area!r}")

    margin = max(0, int(margin))
    usable_width = max(1, ww - margin * 2)
    usable_height = max(1, wh - margin * 2)
    width = min(max(1, requested_width), usable_width)
    height = min(max(1, requested_height), usable_height)
    x = wx + max(0, ww - width - margin)
    y = wy + max(0, wh - height - margin)
    x = min(max(x, wx), wx + ww - width)
    y = min(max(y, wy), wy + wh - height)
    return x, y, width, height


__all__ = [
    "now_utc",
    "now_local",
    "LogMode",
    "VolumeInfo",
    "UsbEvent",
    "AppConfig",
    "as_bool",
    "as_int",
    "hash_id",
    "normalize_drive_path",
    "display_name_for_path",
    "normalize_recent_records",
    "normalize_hook_rules",
    "format_bytes",
    "group_volumes",
    "group_title",
    "event_summary",
    "precise_percent",
    "progress_tooltip_text",
    "countdown_label",
    "SENSITIVE_KEYS",
    "redact",
    "sanitize_for_log",
    "anchored_window_geometry",
]