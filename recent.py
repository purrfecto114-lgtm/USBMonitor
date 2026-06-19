"""Persistent "recent volumes" history."""

from __future__ import annotations

from typing import Any, Optional

from .config import (
    AppConfig,
    ConfigStore,
    _normalize_recent_volume_records,
    display_name_for_path,
    normalise_drive_key,
    safe_int,
)
from .events import VolumeInfo
from .logging_setup import log_error
from .windows_helpers import drive_snapshot, safe_disk_usage
from datetime import datetime, timezone


def _now_utc() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _now_local() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def remember_recent_volume(
    config: AppConfig,
    store: Optional[ConfigStore],
    info_or_path: Any,
    opened: bool = False,
) -> None:
    if isinstance(info_or_path, VolumeInfo):
        path = info_or_path.path
        title = info_or_path.title
        drive_type = info_or_path.drive_type
        total = info_or_path.total
        free = info_or_path.free
    else:
        path = str(info_or_path or "")
        title = ""
        drive_type = drive_snapshot().get(path, {}).get("drive_type", "unknown")
        total, _, free = safe_disk_usage(path)
    key = normalise_drive_key(path)
    if not key:
        return
    path = key if len(key) == 3 and key[1] == ":" else path
    existing = {normalise_drive_key(item.get("path")): item for item in _normalize_recent_volume_records(config.recent_volumes)}
    old = existing.get(key, {})
    if not title:
        title = str(old.get("title") or display_name_for_path(path))
    record = {
        "path": path,
        "title": title,
        "drive_type": drive_type or old.get("drive_type") or "unknown",
        "last_seen_utc": _now_utc(),
        "last_seen_local": _now_local(),
        "open_count": max(safe_int(old.get("open_count"), 0), 0) + (1 if opened else 0),
        "total": total if total is not None else old.get("total"),
        "free": free if free is not None else old.get("free"),
    }
    records = [record] + [
        item
        for item in _normalize_recent_volume_records(config.recent_volumes)
        if normalise_drive_key(item.get("path")) != key
    ]
    config.recent_volumes = _normalize_recent_volume_records(records)
    if store is not None:
        try:
            store.save(config)
        except Exception as exc:
            log_error("recent_volume_save_failed", {"path": path, "message": str(exc)}, exc_info=True)
