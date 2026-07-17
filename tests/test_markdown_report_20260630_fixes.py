"""Regression tests for the 2026-06-30 markdown report implementation."""

from __future__ import annotations

from pathlib import Path

from usb_monitor.app import (
    APP_NAME,
    APP_ORG,
    LogConfig,
    LoggingManager,
    LogMode,
    make_device_interface_filter,
    merge_cli_config,
    parse_args,
    single_instance_mutex_name,
)
from usb_monitor.core import AppConfig, VolumeInfo
from usb_monitor.hooks import HookRule, HookRunner


ROOT = Path(__file__).resolve().parents[1]


def test_log_reset_preserves_crash_log_by_default(tmp_path: Path) -> None:
    for name in ("events.log", "actions.log", "errors.log", "crash.log"):
        (tmp_path / name).write_text("x", encoding="utf-8")

    manager = LoggingManager()
    manager.configure(LogConfig(tmp_path, LogMode.REDACTED, 10_000, 0, False), reset_logs=False)
    manager.reset_files(tmp_path)

    assert not (tmp_path / "events.log").exists()
    assert not (tmp_path / "actions.log").exists()
    assert not (tmp_path / "errors.log").exists()
    assert (tmp_path / "crash.log").exists()


def test_hooks_match_paths_and_labels_case_insensitively() -> None:
    rule = HookRule(name="case", match_paths=("E:*",), match_labels=("backup*",))
    volume = VolumeInfo(path="e:/", title="USB", drive_type="removable", disk_number=None, total=None, used=None, free=None, label="BACKUP-01")

    assert HookRunner._matches(rule, volume)


def test_startup_implies_silent_tray_backend(tmp_path: Path) -> None:
    args = parse_args(["--startup"])
    config = merge_cli_config(args, AppConfig(log_dir=tmp_path))

    assert config.gui_backend == "tray-only"


def test_single_instance_mutex_name_is_sanitized() -> None:
    name = single_instance_mutex_name()
    suffix = name.removeprefix("Local\\")

    assert name.startswith("Local\\")
    assert suffix == f"{APP_ORG}.{APP_NAME}.SingleInstance"
    assert all(ch.isalnum() or ch in "_.-" for ch in suffix)


def test_device_interface_filter_can_use_widened_buffer() -> None:
    fixed = make_device_interface_filter(1)
    widened = make_device_interface_filter(260)

    assert widened.size > fixed.size
    assert fixed.device_type == widened.device_type


def test_main_module_dead_usage_helper_removed() -> None:
    source = (ROOT / "usb_monitor" / "__main__.py").read_text(encoding="utf-8")

    assert "_print_usage" not in source
