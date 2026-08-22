"""Regression tests for tray left/right menu split and larger open button."""

from __future__ import annotations

from pathlib import Path

APP_SOURCE = Path(__file__).resolve().parents[1] / "usb_monitor" / "app.py"


def test_tray_left_click_uses_device_menu_and_right_menu_has_no_usb_device_submenu() -> None:
    source = APP_SOURCE.read_text(encoding="utf-8")
    assert 'self.device_menu = QMenu("USB 设备")' in source
    assert 'tray.activated.connect(self._on_tray_activated)' in source
    assert 'self._activation_reason_is(reason, "Trigger", "DoubleClick")' in source
    assert 'self.volume_menu = self.device_menu' in source
    assert 'self.volume_menu = self.menu.addMenu("USB 设备")' not in source


def test_open_usb_button_is_larger_than_regular_row_button() -> None:
    source = APP_SOURCE.read_text(encoding="utf-8")
    assert 'self.open_button.setMinimumWidth(px(118))' in source
    assert 'self.open_button.setMinimumHeight(px(42))' in source
    assert 'self.open_button.setMinimumWidth(px(68))' in source
    assert 'self.open_button.setMinimumHeight(px(34))' in source
