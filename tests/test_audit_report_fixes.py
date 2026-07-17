"""Regression tests for the 2026-06-29 markdown audit fixes."""

from pathlib import Path


APP_SOURCE = Path(__file__).resolve().parents[1] / "usb_monitor" / "app.py"


def _source() -> str:
    return APP_SOURCE.read_text(encoding="utf-8")


def test_tray_menu_never_monkey_patches_threading_event_wait() -> None:
    source = _source()
    assert "scan_completed.wait =" not in source
    assert "scan_completed_is_set" not in source
    assert "_mark_rescan_idle_blocking" not in source


def test_toast_opacity_animation_dead_code_is_removed() -> None:
    source = _source()
    assert "_animate_opacity" not in source
    assert "_stop_fade_animation" not in source
    assert "QPropertyAnimation" not in source
    assert "QEasingCurve" not in source


def test_startup_copy_compiled_does_not_silently_ignore_rmtree_errors() -> None:
    source = _source()
    assert "ignore_errors=True" not in source
    assert "startup_remove_tree_failed" in source
