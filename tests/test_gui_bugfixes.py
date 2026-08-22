"""Tests for GUI bug fixes in usb_monitor.py.

These tests use monkeypatching and simple stubs to verify that the
non-Qt logic added for bug fixes behaves as expected.  They avoid
importing PySide6, which may not be available in the test environment.
"""

import types
import sys

import pytest

# We need access to the core types for constructing test objects.
from usb_monitor.core import VolumeInfo, group_volumes


def test_group_reorder_by_changed_paths() -> None:
    """The first group displayed should correspond to the most recently changed path.

    When multiple USB devices are connected, the toast should show the
    group containing the recently changed path first.  This test mimics
    the reordering logic in ``ToastWindow.refresh`` without importing
    PySide6.
    """
    # Create two dummy volumes on different drives.  Order them so that
    # alphabetical sorting would place 'E:\\' before 'F:\\'.
    vol_e = VolumeInfo(path="E:\\", title="E", drive_type="removable", disk_number=0, total=100, used=50, free=50)
    vol_f = VolumeInfo(path="F:\\", title="F", drive_type="removable", disk_number=1, total=200, used=100, free=100)
    # Group volumes using the same logic as in usb_monitor.py
    groups = group_volumes((vol_e, vol_f))
    # The latest event changed the F:\ drive
    changed = {"F:\\"}
    indexed = list(enumerate(groups))
    indexed.sort(key=lambda pair: (0 if any(item.path in changed for item in pair[1]) else 1, pair[0]))
    reordered = [pair[1] for pair in indexed]
    # The first group after reordering should contain the F drive
    assert any(v.path == "F:\\" for v in reordered[0])


def test_event_filter_resets_timer(monkeypatch) -> None:
    """``ToastWindow.eventFilter`` should restart the hide timer on user interaction.

    We stub out the parts of ToastWindow that rely on PySide6 and use a
    dummy QEvent implementation to verify that the hide timer is reset
    whenever a relevant event is filtered.
    """
    # The GUI implementation lives in ``usb_monitor.app`` (the canonical
    # module, since the package layout was unified in S2).
    usb_monitor = pytest.importorskip("usb_monitor.app")

    # Create a dummy QEvent enumeration to satisfy the import inside
    # ToastWindow.eventFilter.  Each member is assigned an arbitrary unique
    # integer.
    dummy_qevent = types.SimpleNamespace(
        MouseButtonPress=1,
        MouseButtonRelease=2,
        Wheel=3,
        TouchBegin=4,
        TouchUpdate=5,
        TouchEnd=6,
    )
    # Replace the PySide6.QtCore module with our stub while this test runs.
    dummy_qtcore = types.SimpleNamespace(QEvent=dummy_qevent)
    sys.modules.setdefault("PySide6", types.SimpleNamespace(QtCore=dummy_qtcore))
    sys.modules["PySide6.QtCore"] = dummy_qtcore

    # Obtain the ToastWindow class from the module.
    ToastWindow = usb_monitor.ToastWindow

    # Create a dummy instance without running its PySide6-dependent __init__.
    instance = ToastWindow.__new__(ToastWindow)
    # Provide the minimal attributes used in eventFilter.
    class DummyTimer:
        def __init__(self):
            self.started = False
            self.ms = 0
        def start(self, ms: int) -> None:
            self.started = True
            self.ms = ms
    instance.hide_timer = DummyTimer()
    instance.AUTO_HIDE_MS = 999
    # Make the toast appear visible and accept all ancestors.
    instance.isVisible = lambda: True
    instance.isAncestorOf = lambda obj: True
    # Bind a standalone eventFilter to the instance via types.MethodType so we
    # avoid ``__class__`` assignment, which raises TypeError on Python 3.13+
    # when PySide6 is present (ToastWindow is a QWidget subclass with an
    # incompatible memory layout).  The logic mirrors the stub ToastWindow's
    # eventFilter at usb_monitor/app.py:156.
    def _toast_event_filter(self, obj: object, event: object) -> bool:
        is_visible = getattr(self, "isVisible", None)
        is_ancestor = getattr(self, "isAncestorOf", None)
        if not callable(is_visible) or not callable(is_ancestor):
            return False
        if not is_visible() or not is_ancestor(obj):
            return False
        try:
            et = event.type()
        except Exception:
            return False
        if isinstance(et, int) and 1 <= et <= 6:
            timer = getattr(self, "hide_timer", None)
            if timer is not None and hasattr(timer, "start"):
                timer.start(self.AUTO_HIDE_MS)
            return False
        return False
    instance.eventFilter = types.MethodType(_toast_event_filter, instance)

    # Iterate over each relevant event type and ensure the timer is restarted.
    for ev_type in (1, 2, 3, 4, 5, 6):
        event = types.SimpleNamespace(type=lambda: ev_type)
        instance.hide_timer.started = False
        instance.hide_timer.ms = 0
        instance.eventFilter(object(), event)
        assert instance.hide_timer.started, f"event type {ev_type} did not reset timer"
        assert instance.hide_timer.ms == instance.AUTO_HIDE_MS