"""Tray icon, tray-only backend, and tray context menu controller.

Architecture note
-----------------
The original menu had 13 items at the top level, plus three submenus and
three separators. On smaller Windows DPI/resolution settings Windows
silently truncated that list, so the "退出" item at the very bottom
disappeared. This rewrite keeps the **top-level** menu short (≤8 entries)
and pushes everything optional into submenus. "退出" is always the final
visible item.

Multi-partition devices are merged: when a single physical USB disk
exposes several drive letters, they are shown as one entry in the "USB
设备" submenu, with each partition as a sub-item.
"""

from __future__ import annotations

import sys  # noqa: F401 (kept for parity with original module-level helpers)
from functools import partial
from typing import Any, Optional

from PySide6.QtCore import QObject, QTimer  # noqa: F401  (Qt/Signal kept for future use)
from PySide6.QtGui import QAction, QActionGroup
from PySide6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

from ..config import APP_DISPLAY_NAME, AppConfig, ConfigStore, LogMode, display_name_for_path, hash_id
from ..device_classifier import invalidate_cache
from ..events import UsbEvent, paths_from_event
from ..logging_setup import LOGGER_MANAGER, LogConfig, log_action, log_error
from ..recent import remember_recent_volume
from ..startup import (
    is_startup_enabled,
    set_startup_enabled,
    startup_command_preview,
    startup_folder_path,
    startup_status_report,
)
from ..volume_grouping import group_volumes_by_physical_device, PhysicalDeviceGroup
from ..windows_helpers import (
    format_bytes,
    open_path,
    safe_eject_drive,
)
from .helpers import THEME_LABELS, gui_event_summary
from .theme import IconFactory, Theme


# ---------------------------------------------------------------------------
# Tray-only backend (no toast window)
# ---------------------------------------------------------------------------

class TrayOnly(QObject):
    def __init__(self, tray: QSystemTrayIcon, app_config: AppConfig, config_store: ConfigStore) -> None:
        super().__init__()
        self.tray = tray
        self.app_config = app_config
        self.config_store = config_store
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
            paths.extend(paths_from_event(event))
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        self.last_paths = [vol.path for g in groups for vol in g.volumes]
        for group in groups:
            for vol in group.volumes:
                remember_recent_volume(self.app_config, self.config_store, vol, opened=False)
        title = "USB 设备已更新"
        if latest.action == "remove":
            message = gui_event_summary(latest, include_history=True)
        elif self.last_paths:
            message = "检测到可打开位置：" + "、".join(display_name_for_path(p) for p in self.last_paths[:3])
        elif latest.action in {"add", "change", "error"}:
            message = gui_event_summary(latest, include_history=False)
        else:
            message = str(latest.details.get("message") or latest.details.get("kind") or "USB 设备事件")
        self.tray.showMessage(title, message, QSystemTrayIcon.Information, 10_000)


# ---------------------------------------------------------------------------
# Tray menu controller
# ---------------------------------------------------------------------------

class TrayMenuController(QObject):
    """Builds and owns the tray context menu.

    The menu structure is intentionally shallow at the top level:

        打开最近的 USB 设备
        重新扫描
        USB 设备 ▶              ← per-device submenu, multi-partition merged
        ---
        设置 ▶                  ← theme, topmost, startup, logs
        ---
        退出

    "退出" is always visible because the top level has at most 8 entries.
    """

    def __init__(
        self,
        tray: QSystemTrayIcon,
        receiver: Any,
        app: QApplication,
        theme: Theme,
        icons: IconFactory,
        app_config: AppConfig,
        config_store: ConfigStore,
    ) -> None:
        super().__init__()
        self.tray = tray
        self.receiver = receiver
        self.app = app
        self.theme = theme
        self.icons = icons
        self.app_config = app_config
        self.config_store = config_store
        self.menu = QMenu()
        self.devices_menu: Optional[QMenu] = None
        self.recent_menu: Optional[QMenu] = None
        self.startup_action: Optional[QAction] = None
        self.log_mode_actions: dict[LogMode, QAction] = {}
        self.reset_on_start_action: Optional[QAction] = None
        self._quit_callback: Optional[Any] = None

        self.build_menu()
        tray.setContextMenu(self.menu)
        self.update_tooltip()

    # ------------------------------------------------------------------
    # Quit wiring
    # ------------------------------------------------------------------

    def set_quit_callback(self, callback: Any) -> None:
        """Register the real shutdown handler.

        The controller does not call ``app.quit()`` directly. The app
        module installs a callback that hides the tray, stops the
        detector, drains the logging queue, and *then* quits — otherwise
        the tray icon can linger and keep the process alive on Windows.
        """
        self._quit_callback = callback

    def _quit_app(self) -> None:
        log_action("tray_quit_clicked", {})
        if self._quit_callback is not None:
            try:
                self._quit_callback()
                return
            except Exception as exc:
                log_error("quit_callback_failed", {"message": str(exc)}, exc_info=True)
        # Fallback: try hard to quit.
        try:
            self.tray.hide()
        except Exception:
            pass
        self.app.quit()

    # ------------------------------------------------------------------
    # Config persistence
    # ------------------------------------------------------------------

    def save_config(self) -> None:
        self.config_store.save(self.app_config)

    def update_tooltip(self) -> None:
        from ..device_classifier import usb_drive_snapshot
        from ..logging_setup import LOGGER_MANAGER

        current_count = len(usb_drive_snapshot())
        log_mode_label = "关闭" if not LOGGER_MANAGER.enabled else ("明文" if LOGGER_MANAGER.raw_logs else "脱敏")
        self.tray.setToolTip(
            f"{APP_DISPLAY_NAME} · 当前：{current_count} 个 USB 设备 · 主题：{THEME_LABELS.get(self.app_config.theme, self.app_config.theme)} · 日志：{log_mode_label}"
        )

    # ------------------------------------------------------------------
    # Menu construction
    # ------------------------------------------------------------------

    def build_menu(self) -> None:
        self.menu.clear()

        # --- Top-level quick actions -----------------------------------
        if self.app_config.gui_backend == "tray-only":
            open_action = QAction("打开最近的 USB 设备", self.menu)
            open_action.triggered.connect(self.safe_open_recent)
            self.menu.addAction(open_action)
        else:
            show_action = QAction("显示通知", self.menu)
            show_action.triggered.connect(self._show_toast)
            self.menu.addAction(show_action)

            open_first_action = QAction("打开第一个 USB 设备", self.menu)
            open_first_action.triggered.connect(self._open_first)
            self.menu.addAction(open_first_action)

        rescan_action = QAction("重新扫描 USB 设备", self.menu)
        rescan_action.triggered.connect(self.manual_rescan)
        self.menu.addAction(rescan_action)

        # --- USB devices submenu (multi-partition merged) -------------
        self.devices_menu = self.menu.addMenu("USB 设备")
        self.devices_menu.aboutToShow.connect(self._refresh_devices_menu)

        # --- Settings submenu (everything optional) -------------------
        settings_menu = self.menu.addMenu("设置")
        self._build_settings_menu(settings_menu)

        # --- Separator + Quit (always visible) ------------------------
        self.menu.addSeparator()
        quit_action = QAction("退出", self.menu)
        quit_action.setMenuRole(QAction.NoRole)  # don't let macOS hijack it
        quit_action.triggered.connect(self._quit_app)
        self.menu.addAction(quit_action)

        # Refresh now so the USB devices submenu has content immediately.
        self._refresh_devices_menu()

    def _show_toast(self) -> None:
        if hasattr(self.receiver, "refresh"):
            self.receiver.refresh()
        if hasattr(self.receiver, "show_toast"):
            self.receiver.show_toast()

    def _open_first(self) -> None:
        if hasattr(self.receiver, "open_first"):
            self.receiver.open_first()
        else:
            self.safe_open_recent()

    def _build_settings_menu(self, parent: QMenu) -> None:
        # --- Theme submenu ---
        theme_menu = parent.addMenu("主题")
        theme_group = QActionGroup(theme_menu)
        theme_group.setExclusive(True)
        for key, label in (("auto", "跟随系统"), ("light", "浅色"), ("dark", "深色")):
            action = QAction(label, theme_group)
            action.setCheckable(True)
            action.setChecked(self.app_config.theme == key)
            action.triggered.connect(partial(self.apply_theme, key))
            theme_menu.addAction(action)

        # --- Topmost toggle ---
        topmost_action = QAction("通知置顶", parent)
        topmost_action.setCheckable(True)
        topmost_action.setChecked(bool(self.app_config.topmost))
        topmost_action.toggled.connect(self.apply_topmost)
        parent.addAction(topmost_action)

        parent.addSeparator()

        # --- Startup submenu ---
        startup_menu = parent.addMenu("开机启动")
        self.startup_action = QAction("随系统启动", startup_menu)
        self.startup_action.setCheckable(True)
        self.startup_action.setChecked(is_startup_enabled())
        self.startup_action.toggled.connect(self.toggle_startup)
        startup_menu.addAction(self.startup_action)

        repair_startup_action = QAction("检查 / 修复开机启动", startup_menu)
        repair_startup_action.triggered.connect(self.repair_startup_now)
        startup_menu.addAction(repair_startup_action)

        open_startup_folder_action = QAction("打开启动目录", startup_menu)
        open_startup_folder_action.triggered.connect(self.open_startup_folder)
        startup_menu.addAction(open_startup_folder_action)

        parent.addSeparator()

        # --- Logs submenu ---
        logs_menu = parent.addMenu("日志")
        open_logs_action = QAction("打开日志目录", logs_menu)
        open_logs_action.triggered.connect(self.open_logs)
        logs_menu.addAction(open_logs_action)

        reset_now_action = QAction("立即清空日志", logs_menu)
        reset_now_action.triggered.connect(self.reset_logs_now)
        logs_menu.addAction(reset_now_action)
        logs_menu.addSeparator()

        log_group = QActionGroup(logs_menu)
        log_group.setExclusive(True)
        for mode, label in (
            (LogMode.OFF, "关闭日志"),
            (LogMode.REDACTED, "脱敏日志"),
            (LogMode.RAW, "明文 / 显式日志"),
        ):
            action = QAction(label, log_group)
            action.setCheckable(True)
            action.setChecked(self.app_config.log_mode == mode)
            action.triggered.connect(partial(self.apply_log_mode, mode))
            logs_menu.addAction(action)
            self.log_mode_actions[mode] = action

        logs_menu.addSeparator()
        self.reset_on_start_action = QAction("每次程序打开时重置日志", logs_menu)
        self.reset_on_start_action.setCheckable(True)
        self.reset_on_start_action.setChecked(bool(self.app_config.reset_logs_on_start))
        self.reset_on_start_action.toggled.connect(self.apply_reset_on_start)
        logs_menu.addAction(self.reset_on_start_action)

    # ------------------------------------------------------------------
    # USB devices submenu (multi-partition merging happens here)
    # ------------------------------------------------------------------

    def _refresh_devices_menu(self) -> None:
        if self.devices_menu is None:
            return
        self.devices_menu.clear()
        invalidate_cache()
        groups = group_volumes_by_physical_device()

        if not groups:
            empty_action = self.devices_menu.addAction("当前没有检测到 USB 设备")
            empty_action.setEnabled(False)
            self.update_tooltip()
            return

        for group in groups:
            self._add_device_submenu(self.devices_menu, group)

        self.devices_menu.addSeparator()
        eject_all_action = self.devices_menu.addAction("安全弹出全部…")
        eject_all_action.triggered.connect(self._eject_all_devices)

        self.update_tooltip()

    def _add_device_submenu(self, parent: QMenu, group: PhysicalDeviceGroup) -> None:
        """Add one submenu for a single physical device, with one entry per partition."""
        title = group.label
        if len(group.volumes) > 1:
            title = f"💾 {title}  ·  {group.detail}"
        else:
            title = f"💾 {title}"

        device_menu = parent.addMenu(title)

        # "Open primary partition" — opens the largest partition.
        open_primary = device_menu.addAction(f"打开（默认：{group.primary_path.rstrip(chr(92))}）")
        open_primary.triggered.connect(partial(self.open_volume_path, group.primary_path))

        if len(group.volumes) > 1:
            device_menu.addSeparator()
            partitions_label = device_menu.addAction(f"分区（{len(group.volumes)} 个）")
            partitions_label.setEnabled(False)
            for vol in group.volumes:
                letter = vol.path.rstrip("\\")
                label_text = vol.title.split("·")[0].strip() or "分区"
                action_text = f"打开 {letter}  ·  {label_text}"
                if vol.total:
                    action_text += f"  ·  {format_bytes(vol.total)}"
                action = device_menu.addAction(action_text)
                action.triggered.connect(partial(self.open_volume_path, vol.path))

        device_menu.addSeparator()
        eject_action = device_menu.addAction("安全弹出此设备")
        eject_action.triggered.connect(partial(self._eject_device_group, group))

    def _eject_device_group(self, group: PhysicalDeviceGroup) -> None:
        """Eject every partition of a device.

        Windows will typically reject the second eject call because the
        device is already gone, but that's fine — the first call removes
        the device and the rest become no-ops. We surface a single
        combined message to the user.
        """
        ejected: list[str] = []
        last_error: Optional[str] = None
        for vol in group.volumes:
            try:
                drive = safe_eject_drive(vol.path)
                ejected.append(drive)
                log_action("tray_safe_eject_requested", {
                    "path": vol.path,
                    "drive": drive,
                    "physical_disk": group.physical_disk,
                    "path_hash": hash_id(vol.path),
                })
            except Exception as exc:
                last_error = str(exc)
                log_error("tray_safe_eject_failed", {
                    "path": vol.path,
                    "physical_disk": group.physical_disk,
                    "message": str(exc),
                })
        if ejected:
            self.tray.showMessage(
                APP_DISPLAY_NAME,
                f"已请求安全弹出 {', '.join(ejected)}，请等待 Windows 完成提示。",
                QSystemTrayIcon.Information,
                4000,
            )
        elif last_error:
            self.tray.showMessage(APP_DISPLAY_NAME, last_error, QSystemTrayIcon.Warning, 6000)

    def _eject_all_devices(self) -> None:
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        if not groups:
            self.tray.showMessage(APP_DISPLAY_NAME, "当前没有可弹出的 USB 设备。", QSystemTrayIcon.Information, 2500)
            return
        for group in groups:
            self._eject_device_group(group)

    # ------------------------------------------------------------------
    # Tray actions
    # ------------------------------------------------------------------

    def manual_rescan(self) -> None:
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        for group in groups:
            for vol in group.volumes:
                remember_recent_volume(self.app_config, self.config_store, vol, opened=False)
        paths = [vol.path for group in groups for vol in group.volumes]
        if not paths:
            message = "未检测到 USB 设备。"
            event_action = "change"
        else:
            label_parts = []
            for group in groups[:3]:
                label_parts.append(group.label)
            more = f"  另有 {len(groups) - 3} 个设备" if len(groups) > 3 else ""
            message = f"重新扫描完成：检测到 {len(groups)} 个设备" + more + "  " + "、".join(label_parts)
            event_action = "add"
        event = UsbEvent(
            action=event_action,
            details={"kind": "manual_scan", "message": message, "drive_paths": paths},
            open_paths=tuple(paths),
        )
        if hasattr(self.receiver, "queue_event"):
            self.receiver.queue_event(event)
        self.tray.showMessage(APP_DISPLAY_NAME, message, QSystemTrayIcon.Information, 3500)
        self._refresh_devices_menu()
        log_action("manual_rescan", {"path_count": len(paths), "paths": paths, "device_count": len(groups)})

    def open_volume_path(self, path: str) -> None:
        from ..device_classifier import is_likely_usb_drive
        invalidate_cache()
        if not is_likely_usb_drive(path):
            self.tray.showMessage(
                APP_DISPLAY_NAME,
                f"{path} 当前不是 USB 设备，无法打开。",
                QSystemTrayIcon.Warning,
                3500,
            )
            log_action("tray_open_skipped_not_usb", {"path": path})
            self._refresh_devices_menu()
            return
        try:
            open_path(path)
            remember_recent_volume(self.app_config, self.config_store, path, opened=True)
            log_action("tray_open_volume", {"path": path, "path_hash": hash_id(path)})
        except Exception as exc:
            log_error("tray_open_volume_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.tray.showMessage(APP_DISPLAY_NAME, f"打开失败：{exc}", QSystemTrayIcon.Warning, 6000)

    def safe_eject_volume(self, path: str) -> None:
        try:
            drive = safe_eject_drive(path)
            self.tray.showMessage(
                APP_DISPLAY_NAME,
                f"已请求安全弹出 {drive}，请等待 Windows 完成提示。",
                QSystemTrayIcon.Information,
                3500,
            )
            log_action("tray_safe_eject_requested", {"path": path, "drive": drive, "path_hash": hash_id(path)})
        except Exception as exc:
            log_error("tray_safe_eject_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.tray.showMessage(APP_DISPLAY_NAME, str(exc), QSystemTrayIcon.Warning, 6000)

    def safe_open_recent(self) -> None:
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        if groups:
            self.open_volume_path(groups[0].primary_path)
            return
        # Fall back to recent volumes that are currently attached.
        from ..config import _normalize_recent_volume_records
        from ..device_classifier import usb_drive_snapshot
        snapshot = usb_drive_snapshot()
        for item in _normalize_recent_volume_records(self.app_config.recent_volumes):
            path = str(item.get("path") or "")
            if path in snapshot:
                self.open_volume_path(path)
                return
        self.tray.showMessage(APP_DISPLAY_NAME, "当前没有可打开的 USB 设备。", QSystemTrayIcon.Information, 3000)

    # ------------------------------------------------------------------
    # Settings handlers
    # ------------------------------------------------------------------

    def apply_log_mode(self, mode: LogMode) -> None:
        self.app_config.log_mode = mode
        self.save_config()
        LOGGER_MANAGER.set_mode(mode)
        for item_mode, action in self.log_mode_actions.items():
            action.setChecked(item_mode == mode)
        self.update_tooltip()
        log_mode_label = "关闭" if mode == LogMode.OFF else ("明文" if mode == LogMode.RAW else "脱敏")
        self.tray.showMessage(APP_DISPLAY_NAME, f"日志模式：{log_mode_label}", QSystemTrayIcon.Information, 2500)
        log_action("log_mode_changed", {"mode": mode.value})

    def apply_reset_on_start(self, enabled: bool) -> None:
        self.app_config.reset_logs_on_start = bool(enabled)
        self.save_config()
        log_action("reset_logs_on_start_changed", {"enabled": bool(enabled)})

    def apply_theme(self, theme_name: str) -> None:
        self.app_config.theme = theme_name
        self.save_config()
        new_theme = Theme(theme_name, self.app, self.theme.px)
        self.theme = new_theme
        if hasattr(self.receiver, "apply_theme"):
            self.receiver.apply_theme(new_theme)
        self.tray.setIcon(self.icons.app_icon(new_theme))
        self.app.setWindowIcon(self.icons.app_icon(new_theme))
        self.update_tooltip()
        log_action("theme_changed", {"theme": theme_name})

    def apply_topmost(self, enabled: bool) -> None:
        self.app_config.topmost = bool(enabled)
        self.save_config()
        if hasattr(self.receiver, "set_topmost"):
            self.receiver.set_topmost(bool(enabled))
        elif hasattr(self.receiver, "keep_topmost"):
            self.receiver.keep_topmost = bool(enabled)
        log_action("topmost_changed", {"enabled": bool(enabled)})

    # ------------------------------------------------------------------
    # Log / startup handlers
    # ------------------------------------------------------------------

    def open_logs(self) -> None:
        path = str(self.app_config.log_dir)
        try:
            self.app_config.log_dir.mkdir(parents=True, exist_ok=True)
            open_path(path)
            log_action("open_log_dir", {"path": path})
        except Exception as exc:
            log_error("open_log_dir_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.tray.showMessage(APP_DISPLAY_NAME, f"打开日志目录失败：{exc}", QSystemTrayIcon.Warning, 6000)

    def reset_logs_now(self) -> None:
        try:
            LOGGER_MANAGER.stop()
            LOGGER_MANAGER.reset_log_files(self.app_config.log_dir)
            LOGGER_MANAGER.configure(
                LogConfig(
                    self.app_config.log_dir,
                    self.app_config.log_mode,
                    self.app_config.log_max_bytes,
                    self.app_config.log_backups,
                    self.app_config.console_log,
                ),
                reset_logs=False,
            )
            self.tray.showMessage(APP_DISPLAY_NAME, "日志已清空", QSystemTrayIcon.Information, 2500)
            log_action("logs_reset_now", {"mode": self.app_config.log_mode.value})
        except Exception as exc:
            self.tray.showMessage(APP_DISPLAY_NAME, f"清空日志失败：{exc}", QSystemTrayIcon.Warning, 6000)

    def open_startup_folder(self) -> None:
        try:
            startup_folder_path().mkdir(parents=True, exist_ok=True)
            open_path(str(startup_folder_path()))
            log_action("open_startup_folder", {"path": str(startup_folder_path())})
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
            log_action("startup_changed", {
                "enabled": is_startup_enabled(),
                "method": method,
                "command": startup_command_preview(),
                "status": report,
            })
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
