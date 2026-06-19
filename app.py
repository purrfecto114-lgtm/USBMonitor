"""GUI bootstrap, console mode, and main entry point."""

from __future__ import annotations

import atexit
import json
import platform
import sys
import time
from typing import Optional

from .cli import merge_cli_config, parse_args
from .config import APP_DISPLAY_NAME, AppConfig, ConfigStore, LogConfig, LogMode, config_path
from .detector import Win32UsbDetector
from .events import UsbEvent
from .logging_setup import (
    LOGGER_MANAGER,
    log_action,
    log_error,
    log_event,
    sanitize_for_log,
    stop_logging,
)
from .startup import (
    repair_startup_registration_if_needed,
    set_startup_enabled,
    startup_status_report,
)
from .windows_helpers import acquire_single_instance_lock, user32


# ---------------------------------------------------------------------------
# GUI mode
# ---------------------------------------------------------------------------

def run_gui(args, app_config: AppConfig, config_store: ConfigStore) -> int:
    try:
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QApplication, QSystemTrayIcon
    except ImportError as exc:
        raise SystemExit("GUI mode requires PySide6: py -m pip install PySide6") from exc

    from .gui.theme import IconFactory, Theme, make_px
    from .gui.toast import Bridge, Toast
    from .gui.tray import TrayMenuController, TrayOnly

    SCALE = 0.85
    px = make_px(SCALE)

    app = QApplication(sys.argv[:1])
    app.setApplicationName(APP_DISPLAY_NAME)
    app.setOrganizationName("BellaKipping")
    app.setQuitOnLastWindowClosed(False)

    theme = Theme(app_config.theme, app, px)
    icons = IconFactory(app, px)
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
        receiver = TrayOnly(tray, app_config, config_store)
        try:
            bridge.event_received.connect(receiver.queue_event, type=Qt.ConnectionType.QueuedConnection)
        except TypeError:
            bridge.event_received.connect(receiver.queue_event)
    else:
        toast = Toast(app, theme, icons, app_config.topmost, app_config, config_store)
        try:
            bridge.event_received.connect(toast.queue_event, type=Qt.ConnectionType.QueuedConnection)
        except TypeError:
            bridge.event_received.connect(toast.queue_event)
        app._usb_monitor_toast = toast  # type: ignore[attr-defined]
        receiver = toast

    menu_controller: Optional[TrayMenuController] = None
    if tray is not None:
        menu_controller = TrayMenuController(tray, receiver, app, theme, icons, app_config, config_store)
        app._usb_monitor_menu = menu_controller  # type: ignore[attr-defined]

    sink = lambda event: bridge.event_received.emit(event)
    detector = Win32UsbDetector(sink=sink)
    app._usb_monitor_detector = detector  # type: ignore[attr-defined]
    detector.start()

    # --- Clean shutdown wiring -----------------------------------------
    # ``app.quit`` alone doesn't always release the tray icon on Windows;
    # we hide it explicitly and stop the detector before letting Qt exit.
    def _shutdown() -> None:
        try:
            detector.stop()
            detector.join(timeout=1.0)
        except Exception:
            pass
        if tray is not None:
            try:
                tray.hide()
            except Exception:
                pass
        log_action("app_quit", {"reason": "tray_quit"})
        stop_logging()
        app.quit()

    if menu_controller is not None:
        menu_controller.set_quit_callback(_shutdown)

    def stop_detector() -> None:
        detector.stop()
        detector.join(timeout=1.0)
        log_action("app_quit", {"reason": "qt_about_to_quit"})

    app.aboutToQuit.connect(stop_detector)
    log_action("app_started", {
        "gui_backend": app_config.gui_backend,
        "theme": app_config.theme,
        "topmost": app_config.topmost,
        "log_mode": app_config.log_mode.value,
        "ui_scale": SCALE,
        "startup_arg": bool(args.startup),
    })
    return int(app.exec())


# ---------------------------------------------------------------------------
# Console mode (diagnostic)
# ---------------------------------------------------------------------------

def run_console(args) -> int:
    if platform.system() != "Windows":
        print("This Windows-only branch must run on Windows.", file=sys.stderr)
        return 2

    def sink(event: UsbEvent) -> None:
        print(json.dumps(
            sanitize_for_log(
                {"action": event.action, "details": event.details, "open_paths": event.open_paths},
                raw=LOGGER_MANAGER.raw_logs,
            ),
            ensure_ascii=False,
            default=str,
        ))

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
# Second-instance notification
# ---------------------------------------------------------------------------

def _notify_second_instance_blocked() -> None:
    if user32 is None:
        return
    try:
        MB_ICONINFORMATION = 0x00000040
        MB_SETFOREGROUND = 0x00010000
        MB_TOPMOST = 0x00040000
        user32.MessageBoxW(
            None,
            f"{APP_DISPLAY_NAME} 已经在运行。\n\n请在任务栏右下角系统托盘（可能需要点击\"显示隐藏的图标\"）找到它的图标并右键查看菜单，"
            "而不是再启动一份新的实例。",
            APP_DISPLAY_NAME,
            MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST,
        )
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

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
