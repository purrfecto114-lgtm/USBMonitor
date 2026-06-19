"""Event types emitted by USB detectors."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Optional, Sequence

from .config import hash_id
from .logging_setup import LOGGER_MANAGER, log_event, sanitize_for_log

EventSink = Callable[["UsbEvent"], None]


def _now_utc() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _now_local() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def _local_tz_name() -> str:
    tz = datetime.now().astimezone().tzinfo
    return tz.tzname(None) if tz else "local"


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
    # New in v7: identify the physical device backing this volume.
    physical_disk: Optional[int] = None
    is_usb: bool = True
    bus_type: str = "unknown"
    device_model: str = ""


@dataclass(frozen=True)
class UsbEvent:
    action: str
    details: dict[str, Any]
    open_paths: tuple[str, ...] = field(default_factory=tuple)
    display: bool = True
    timestamp_utc: str = field(default_factory=lambda: _now_utc())
    timestamp_local: str = field(default_factory=lambda: _now_local())
    timezone: str = field(default_factory=lambda: _local_tz_name())


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
        payload["open_path_hashes"] = [hash_id(path) for path in event.open_paths]
    log_event("usb_event", payload)


def emit_usb_event(
    action: str,
    details: dict[str, Any],
    sink: Optional[EventSink] = None,
    open_paths: Sequence[str] = (),
    display: bool = True,
) -> UsbEvent:
    unique_paths = tuple(dict.fromkeys(str(path) for path in open_paths if path))
    event = UsbEvent(action=action, details=details, open_paths=unique_paths, display=display)
    log_usb_event(event)
    if sink is not None and display:
        sink(event)
    return event


def paths_from_event(event: UsbEvent) -> tuple[str, ...]:
    """Collect every drive path mentioned in an event, from any field.

    Used by the GUI/detector to figure out which volumes an event touches
    even when the event itself only carries a unitmask or no paths at all.
    """
    candidates = list(event.open_paths)
    for key in ("drive_paths", "removed_paths", "removed_volumes"):
        value = event.details.get(key)
        if isinstance(value, list):
            candidates.extend(str(item) for item in value if item)
    path = event.details.get("path")
    if isinstance(path, str) and path:
        candidates.append(path)
    return tuple(dict.fromkeys(candidates))


# Legacy alias used by the original codebase.
_paths_from_event = paths_from_event
