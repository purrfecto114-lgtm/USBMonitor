"""Win32 USB detector with burst-safe drive reconciliation."""

from __future__ import annotations

import ctypes
import json
import threading
import time
from typing import Any, Optional, Sequence

from .config import APP_NAME, hash_id
from .device_classifier import (
    classify_volume,
    invalidate_cache,
    is_likely_usb_drive,
)
from .events import EventSink, UsbEvent, emit_usb_event, paths_from_event
from .logging_setup import log_error, log_event
from .windows_helpers import (
    DBT_CONFIGCHANGED,
    DBT_DEVICEARRIVAL,
    DBT_DEVICEREMOVECOMPLETE,
    DBT_DEVNODES_CHANGED,
    DBT_DEVTYP_VOLUME,
    WM_CLOSE,
    WM_DEVICECHANGE,
    details_from_lparam,
    drive_snapshot as raw_drive_snapshot,
    kernel32,
    user32,
)
from .volume_grouping import usb_drive_snapshot as filtered_drive_snapshot


# Re-export paths_from_event for compat.
def _paths_from_event_local(event: UsbEvent) -> tuple[str, ...]:  # pragma: no cover - thin shim
    return paths_from_event(event)


class Win32UsbDetector(threading.Thread):
    """Dedicated hidden-window USB monitor with burst-safe reconciliation.

    Compared to the v6 detector this version filters every drive snapshot
    through :func:`device_classifier.is_likely_usb_drive` so internal SD
    readers and other false positives never reach the GUI.
    """

    def __init__(self, sink: EventSink) -> None:
        super().__init__(daemon=True, name="win32-usb-detector")
        self.sink = sink
        self.stop_event = threading.Event()
        self.hwnd: Optional[int] = None
        self.notification_handle: Optional[int] = None
        self.window_class: Optional[str] = None
        # Baseline is *filtered* — only USB drives count.
        self._baseline = filtered_drive_snapshot()
        self._recent: dict[str, float] = {}
        self._wnd_proc_ref: Any = None
        self._scan_lock = threading.Lock()
        self._dedup_lock = threading.Lock()
        self._last_no_path_arrival_ts = 0.0
        self._event_history: list[UsbEvent] = []

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

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
            emit_usb_event(
                "error",
                {"kind": "error", "message": "Windows backend requires pywin32: py -m pip install pywin32"},
                sink=self.sink,
            )
            log_error("pywin32_missing", {"message": str(exc)}, exc_info=True)
            return

        if user32 is None:
            emit_usb_event(
                "error",
                {"kind": "error", "message": "This build only supports Windows."},
                sink=self.sink,
            )
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
        class_name = f"{APP_NAME}HiddenWindow{__import__('os').getpid()}"
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
        log_event("detector_started", {
            "backend": "win32_hidden_window",
            "initial_drive_count": len(self._baseline),
            "initial_drives": list(self._baseline.keys()),
        })

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
        from .windows_helpers import (
            DEV_BROADCAST_DEVICEINTERFACE_W,
            DEVICE_NOTIFY_WINDOW_HANDLE,
            DBT_DEVTYP_DEVICEINTERFACE,
            usb_device_interface_guid,
        )
        from ctypes import wintypes
        user32.RegisterDeviceNotificationW.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD]
        user32.RegisterDeviceNotificationW.restype = wintypes.HANDLE
        notification_filter = DEV_BROADCAST_DEVICEINTERFACE_W()
        notification_filter.dbcc_size = ctypes.sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)
        notification_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE
        notification_filter.dbcc_reserved = 0
        notification_filter.dbcc_classguid = usb_device_interface_guid()
        handle = user32.RegisterDeviceNotificationW(
            self.hwnd,
            ctypes.byref(notification_filter),
            DEVICE_NOTIFY_WINDOW_HANDLE,
        )
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
        from ctypes import wintypes
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

    # ------------------------------------------------------------------
    # Dedup / event history
    # ------------------------------------------------------------------

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
        return hash_id(key)

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
            paths = list(_paths_from_event_local(event))
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
        if action != "remove":
            return details
        recent_events = self._recent_event_summaries(limit=4)
        if not recent_events:
            return details
        enriched = dict(details)
        enriched["recent_events_before_remove"] = recent_events
        return enriched

    def _emit_once(
        self,
        action: str,
        details: dict[str, Any],
        paths: Sequence[str],
        display: bool = True,
        ttl_s: float = 1.5,
    ) -> None:
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
            log_event("usb_event_deduplicated", {
                "fingerprint": fp,
                "action": action,
                "kind": details.get("kind"),
                "path_count": len(paths),
            })
            return
        event = emit_usb_event(
            action,
            self._details_for_emit(action, details),
            sink=self.sink,
            open_paths=paths,
            display=display,
        )
        self._remember_event(event)

    # ------------------------------------------------------------------
    # Drive-snapshot reconciliation
    # ------------------------------------------------------------------

    def _snapshot_delta(self) -> tuple[dict[str, dict[str, Any]], list[str], list[str]]:
        # Drop the cache so freshly-inserted volumes get re-classified.
        invalidate_cache()
        current = filtered_drive_snapshot()
        before = set(self._baseline)
        after = set(current)
        added = sorted(after - before)
        removed = sorted(before - after)
        self._baseline = current
        return current, added, removed

    def _emit_snapshot_delta(
        self,
        reason_action: str,
        details: dict[str, Any],
        immediate_display_empty_remove: bool = False,
    ) -> None:
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

    def _schedule_drive_scans(
        self,
        reason_action: str,
        details: dict[str, Any],
        delays: Sequence[float] = (0.05, 0.20, 0.85),
    ) -> None:
        for index, delay in enumerate(delays):
            threading.Thread(
                target=self._delayed_drive_scan,
                args=(reason_action, dict(details), float(delay), index),
                daemon=True,
                name=f"drive-scan-{reason_action}-{index}",
            ).start()

    # ------------------------------------------------------------------
    # WM_DEVICECHANGE handler
    # ------------------------------------------------------------------

    def _handle_device_change(self, wparam: int, lparam: int) -> None:
        if wparam in (DBT_DEVICEARRIVAL, DBT_DEVICEREMOVECOMPLETE):
            action = "add" if wparam == DBT_DEVICEARRIVAL else "remove"
            details, paths = details_from_lparam(lparam)
            details["event_code"] = wparam
            details["event_name"] = "DBT_DEVICEARRIVAL" if action == "add" else "DBT_DEVICEREMOVECOMPLETE"

            if action == "remove":
                if not paths:
                    self._emit_snapshot_delta("remove", details, immediate_display_empty_remove=True)
                else:
                    # A just-removed volume cannot always be re-opened for
                    # classification. Use the filtered baseline captured before
                    # removal so non-USB volume removals do not produce toasts.
                    baseline_paths = set(self._baseline)
                    usb_paths = [path for path in paths if path in baseline_paths]
                    if usb_paths:
                        self._emit_once("remove", details, usb_paths, display=True, ttl_s=0.2)
                    else:
                        log_event("non_usb_removal_ignored", {
                            "event_name": details["event_name"],
                            "paths": list(paths),
                        })
                    with self._scan_lock:
                        invalidate_cache()
                        self._baseline = filtered_drive_snapshot()
                self._schedule_drive_scans("remove", details, delays=(0.10, 0.45))
                return

            if paths:
                # Filter the raw volume list through the classifier so an
                # internal SD-card reader that just got a drive letter
                # doesn't trigger a toast. Clear the cache first because a
                # drive letter can be reused within the previous 5-second TTL.
                invalidate_cache()
                usb_paths = [path for path in paths if is_likely_usb_drive(path)]
                with self._scan_lock:
                    invalidate_cache()
                    self._baseline = filtered_drive_snapshot()
                    current = dict(self._baseline)
                if usb_paths:
                    for path in usb_paths:
                        snap = current.get(path)
                        if snap:
                            details.setdefault("volumes", {})[path] = snap
                    self._emit_once("add", details, usb_paths, display=True, ttl_s=0.25)
                    self._schedule_drive_scans("add", details, delays=(0.25, 0.90))
                else:
                    # The DBT_DEVICEARRIVAL was for a non-USB volume (e.g.
                    # a fixed disk partition showed up). Skip the toast.
                    log_event("non_usb_arrival_ignored", {
                        "event_name": details["event_name"],
                        "paths": list(paths),
                    })
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

        self._emit_once(
            "change",
            {"kind": "unhandled_device_change", "event_code": wparam},
            [],
            display=False,
            ttl_s=2.0,
        )

    def _delayed_drive_scan(
        self,
        action: str,
        details: dict[str, Any],
        delay_s: float,
        pass_index: int = 0,
    ) -> None:
        time.sleep(max(0.0, delay_s))
        details = dict(details)
        details["scan_pass"] = pass_index
        self._emit_snapshot_delta(action, details, immediate_display_empty_remove=False)
