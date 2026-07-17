"""Regression tests for the second markdown diagnostic pass."""

from __future__ import annotations

from pathlib import Path

from usb_monitor.app import (
    DBT_DEVICEREMOVECOMPLETE,
    DriveReconciler,
    RawDeviceChange,
    VolumeState,
)
from usb_monitor.core import UsbEvent, VolumeInfo


APP_SOURCE = Path(__file__).resolve().parents[1] / "usb_monitor" / "app.py"
HOOKS_SOURCE = Path(__file__).resolve().parents[1] / "usb_monitor" / "hooks.py"


def _volume(path: str) -> VolumeInfo:
    return VolumeInfo(path=path, title=path, drive_type="removable", disk_number=None, total=100, used=50, free=50)


class FakeScanner:
    def __init__(self) -> None:
        self.scan_result: dict[str, VolumeInfo] = {}
        self.invalidated: list[tuple[str, ...] | None] = []

    def invalidate(self, paths=None) -> None:
        self.invalidated.append(tuple(paths) if paths is not None else None)

    def scan(self, focus=()):
        return dict(self.scan_result)


def test_reconciler_keeps_all_scan_lanes_instead_of_overwriting() -> None:
    scanner = FakeScanner()
    state = VolumeState()
    events: list[UsbEvent] = []
    reconciler = DriveReconciler(scanner, state, events.append)

    reconciler._handle_raw(RawDeviceChange(DBT_DEVICEREMOVECOMPLETE, "volume", paths=("E:\\",)))

    assert len(reconciler._scheduled) == 3
    assert all(":" in key for key in reconciler._scheduled)


def test_pending_remove_is_confirmed_by_settle_scan_before_state_changes() -> None:
    scanner = FakeScanner()
    state = VolumeState()
    events: list[UsbEvent] = []
    reconciler = DriveReconciler(scanner, state, events.append)
    reconciler._baseline = {"E:\\": _volume("E:\\")}
    state.replace(reconciler._baseline)

    reconciler._handle_raw(RawDeviceChange(DBT_DEVICEREMOVECOMPLETE, "volume", paths=("E:\\",)))
    assert state.get("E:\\") is not None
    assert not events

    reconciler._removed_hold_until["E:\\"] = 0.0
    settle = list(reconciler._scheduled.values())[-1]
    reconciler._run_scan(settle)

    assert state.get("E:\\") is None
    assert events and events[-1].action == "remove"


def test_global_event_filter_is_not_installed_in_real_toast_code() -> None:
    source = APP_SOURCE.read_text(encoding="utf-8")
    assert "self.app.installEventFilter(self)" not in source
    assert "focusChanged.connect" in source


def test_hooks_use_bounded_thread_pool_not_unbounded_threads() -> None:
    source = HOOKS_SOURCE.read_text(encoding="utf-8")
    assert "ThreadPoolExecutor" in source
    assert "threading.Thread(target=run" not in source
