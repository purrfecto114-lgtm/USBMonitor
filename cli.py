"""CLI argument parsing and config merge."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

from .config import AppConfig, LogMode


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Windows USB monitor with PySide6 toast and runtime-switchable logging.",
    )
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
