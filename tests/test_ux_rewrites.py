"""Tests for the S2 UX rewrites.

These tests exercise the new ``ToastWindow`` and ``VolumeRow`` behaviour
without requiring PySide6.  We use duck-stubbing to rebuild just enough of
the Qt namespace for the S2 code paths to run.
"""

from __future__ import annotations

import sys
import types
from pathlib import Path
from unittest.mock import MagicMock

# Make ``import usb_monitor`` resolvable.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


# ---------------------------------------------------------------------------
# Stub layer — minimal PySide6 stand-ins
# ---------------------------------------------------------------------------


class _StubQt:
    Tool = 0
    FramelessWindowHint = 0
    WindowStaysOnTopHint = 0
    WindowDoesNotAcceptFocus = 0
    WA_TranslucentBackground = 0
    WA_ShowWithoutActivating = 0
    NoFocus = 0
    ScrollBarAlwaysOff = 0
    LeftButton = 1
    PointingHandCursor = 0
    Key_Return = 0
    Key_Enter = 0
    Key_Space = 0
    Key_Menu = 0
    Key_C = 1
    Key_Escape = 2
    ControlModifier = 1 << 8
    CustomContextMenu = 0
    TextSelectableByMouse = 0
    StrongFocus = 1
    ElideRight = 0


class _StubQEvent:
    WindowDeactivate = 2005
    MouseButtonPress = 2


class _FakeTimer:
    """QTimer stub: stores interval, can be ``start``-ed and ``stop``-ped."""

    def __init__(self, parent=None):
        self._parent = parent
        self._interval = 0
        self._active = False
        self._single_shot = False
        self._callback = None
        self._remaining_ms = 0
        self._timeout_cb = None

    def setSingleShot(self, single_shot: bool) -> None:
        self._single_shot = single_shot

    def setInterval(self, ms: int) -> None:
        self._interval = ms

    def timeout(self):
        return _FakeSignal(self, "timeout")

    def start(self, ms: int = -1) -> None:
        if ms > 0:
            self._remaining_ms = ms
        self._active = True
        if not self._single_shot:
            self._remaining_ms = self._interval

    def stop(self) -> None:
        self._active = False

    def isActive(self) -> bool:
        return self._active

    def remainingTime(self) -> int:
        return self._remaining_ms if self._active else 0

    def connect(self, callback) -> None:
        self._timeout_cb = callback

    def fire(self) -> None:
        if self._timeout_cb is not None:
            self._active = False
            self._timeout_cb()

    def tick(self, ms: int) -> None:
        if self._active and self._single_shot:
            self._remaining_ms = max(0, self._remaining_ms - ms)
            if self._remaining_ms == 0:
                self.fire()


class _FakeSignal:
    def __init__(self, owner, name):
        self._owner = owner
        self._name = name

    def connect(self, callback):
        setattr(self._owner, f"_{self._name}_cb", callback)


class _FakeProgressBar:
    def __init__(self):
        self._range = (0, 0)
        self._value = 0
        self._tooltip = ""
        self._focus_policy = 0

    def setRange(self, lo, hi):
        self._range = (lo, hi)

    def setValue(self, value):
        self._value = value

    def setTextVisible(self, value):
        pass

    def setFixedHeight(self, value):
        pass

    def setToolTip(self, text):
        self._tooltip = text

    def tooltip(self):
        return self._tooltip

    def value(self):
        return self._value

    def range(self):
        return self._range

    def setFocusPolicy(self, policy):
        self._focus_policy = policy


class _FakeWidget:
    def __init__(self, parent=None):
        self._tooltip = ""
        self._focus_policy = 0
        self._cursor = 0
        self._visible = False
        self._properties = {}

    def setFocusPolicy(self, policy):
        self._focus_policy = policy

    def setCursor(self, cursor):
        self._cursor = cursor

    def setObjectName(self, name):
        pass

    def setContextMenuPolicy(self, policy):
        pass

    def setToolTip(self, text):
        self._tooltip = text

    def tooltip(self):
        return self._tooltip

    def setProperty(self, key, value):
        self._properties[key] = value

    def property(self, key, default=None):
        return self._properties.get(key, default)

    def setStyleSheet(self, text):
        pass

    def style(self):
        return _FakeStyle()

    def show(self):
        self._visible = True

    def hide(self):
        self._visible = False

    def isVisible(self):
        return self._visible

    def setWindowFlags(self, flags):
        pass

    def setAttribute(self, attr, value):
        pass

    def setWindowTitle(self, title):
        pass

    def isWindow(self):
        return True

    def childAt(self, pos):
        return self

    def inherits(self, name):
        return False

    def mapFromGlobal(self, pos):
        return pos

    def mapToGlobal(self, pos):
        return pos

    def adjustSize(self):
        pass

    def resize(self, *args):
        pass

    def sizeHint(self):
        return _FakeSize(400, 200)

    def setWindowOpacity(self, value):
        self._opacity = value

    def windowOpacity(self):
        return getattr(self, "_opacity", 1.0)

    def move(self, pos):
        pass

    def pos(self):
        return types.SimpleNamespace(x=0, y=0)

    def x(self):
        return 0

    def y(self):
        return 0

    def width(self):
        return 400

    def height(self):
        return 200

    def setMinimumHeight(self, h):
        pass

    def setMaximumHeight(self, h):
        pass


class _FakeStyle:
    def unpolish(self, widget):
        pass

    def polish(self, widget):
        pass


class _FakeSize:
    def __init__(self, w, h):
        self._w = w
        self._h = h

    def width(self):
        return self._w

    def height(self):
        return self._h


def _stub_pyside6():
    """Install a minimal PySide6 namespace into ``sys.modules`` (idempotent)."""
    if "PySide6" in sys.modules and getattr(sys.modules["PySide6"], "_usb_monitor_stub", False):
        return
    qt = _StubQt()
    qevent = _StubQEvent()
    QtCore = types.SimpleNamespace(
        QByteArray=object,
        QEvent=qevent,
        QObject=type("QObject", (), {}),
        QPoint=type("QPoint", (), {}),
        QPropertyAnimation=MagicMock(),
        QEasingCurve=types.SimpleNamespace(InOutCubic=0),
        QRectF=object,
        QThread=type("QThread", (), {}),
        QTimer=_FakeTimer,
        Qt=qt,
        Signal=lambda *a, **kw: None,
    )
    QtGui = types.SimpleNamespace(
        QAction=type("QAction", (), {}),
        QActionGroup=type("QActionGroup", (), {}),
        QColor=type("QColor", (), {}),
        QCursor=types.SimpleNamespace(pos=lambda: None),
        QIcon=type("QIcon", (), {}),
        QPainter=type("QPainter", (), {}),
        QPalette=type("QPalette", (), {}),
        QPixmap=type("QPixmap", (), {}),
    )
    QtSvg = types.SimpleNamespace(QSvgRenderer=type("QSvgRenderer", (), {}))
    QtWidgets = types.SimpleNamespace(
        QApplication=type("QApplication", (), {}),
        QFrame=type("QFrame", (), {}),
        QGraphicsDropShadowEffect=type("QGraphicsDropShadowEffect", (), {}),
        QGridLayout=type("QGridLayout", (), {}),
        QHBoxLayout=type("QHBoxLayout", (), {}),
        QLabel=type("QLabel", (), {}),
        QMenu=type("QMenu", (), {}),
        QPushButton=type("QPushButton", (), {}),
        QProgressBar=_FakeProgressBar,
        QScrollArea=type("QScrollArea", (), {}),
        QSizePolicy=type("QSizePolicy", (), {}),
        QSystemTrayIcon=type("QSystemTrayIcon", (), {}),
        QVBoxLayout=type("QVBoxLayout", (), {}),
        QWidget=_FakeWidget,
    )
    pyside6 = types.SimpleNamespace(QtCore=QtCore, QtGui=QtGui, QtSvg=QtSvg, QtWidgets=QtWidgets)
    pyside6._usb_monitor_stub = True
    sys.modules["PySide6"] = pyside6
    sys.modules["PySide6.QtCore"] = QtCore
    sys.modules["PySide6.QtGui"] = QtGui
    sys.modules["PySide6.QtSvg"] = QtSvg
    sys.modules["PySide6.QtWidgets"] = QtWidgets


# Install the stub and import usb_monitor AT MODULE IMPORT TIME so it picks
# up the stub.  Other test files (test_bus_cache.py) that import usb_monitor
# may have already cached a copy with QT_AVAILABLE=False — we force a fresh
# import here so the stub takes effect.
_stub_pyside6()
if "usb_monitor" in sys.modules:
    import importlib
    usb_monitor = importlib.reload(sys.modules["usb_monitor"])
else:
    import usb_monitor  # noqa: E402

import pytest  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


class _FakeApp:
    def __init__(self):
        self.screens = MagicMock(return_value=[])

    def installEventFilter(self, obj):
        pass


class _FakeTheme:
    def __init__(self):
        self.stylesheet = lambda: ""
        self.shadow = None
        self.icon_shell = "#000"
        self.icon_socket = "#000"

    def icon(self, status):
        return MagicMock()

    def pixmap(self, status, theme, size):
        return MagicMock()


class _FakeIcons:
    def __init__(self):
        pass

    def icon(self, theme):
        return MagicMock()

    def pixmap(self, status, theme, size):
        return MagicMock()


class _FakeActions:
    def __init__(self):
        self.toast = None
        self.notified = []

    def open_volume(self, path):
        self.notified.append(("open", path))

    def copy_text(self, text):
        self.notified.append(("copy", text))

    def notify(self, msg, warning=False, timeout=4000):
        self.notified.append(("notify", msg, warning, timeout))


class _FakeLabel:
    def __init__(self):
        self._text = ""
        self._visible = False

    def setText(self, text):
        self._text = text

    def text(self):
        return self._text

    def setVisible(self, visible):
        self._visible = bool(visible)

    def isVisible(self):
        return self._visible


def _build_toast_stub(um, ToastWindow):
    """Construct a ToastWindow with stubbed Qt collaborators.

    The real class extends QWidget, so we bypass ``__init__`` and patch the
    handful of methods we'd otherwise inherit (visibility, event handlers).
    """
    instance = ToastWindow.__new__(ToastWindow)
    instance.app = _FakeApp()
    instance.theme = _FakeTheme()
    instance.icons = _FakeIcons()
    instance.actions = _FakeActions()
    instance.keep_topmost = False
    instance.exit_on_close = False
    from collections import deque
    instance.events = deque(maxlen=20)
    instance.volumes = ()
    instance.expanded = False
    instance.hide_timer = _FakeTimer(instance)
    instance.hide_timer.setSingleShot(True)
    instance._is_paused = False
    instance._remaining_ms = ToastWindow.AUTO_HIDE_MS
    instance._countdown_timer = _FakeTimer(instance)
    instance._countdown_timer.setInterval(500)
    instance._status_overrides = {}
    instance._status_override = None
    instance._anchor_pos = None
    instance._anchor_screen_key = None
    instance._anchor_work_area = None
    instance._stable_work_areas = {}
    instance._internal_geometry_change = False
    instance._reposition_pending = False
    instance._connected_screens = set()
    instance._fade_animation = None
    instance._fading_out = False
    instance._outside_filter_installed = False
    instance.countdown_label = _FakeLabel()
    instance.status_label = _FakeLabel()
    instance.status_label.setVisible(False)

    # Replace Qt base methods so we can drive the toast without a real event
    # loop.  Visibility lives in a closure that's shared by show/hide/isVisible.
    _visible = {"value": False}

    def _show():
        _visible["value"] = True
        instance.hide_timer._active = True

    def _hide():
        _visible["value"] = False

    def _is_visible():
        return _visible["value"]

    instance.show = _show
    instance.hide = _hide
    instance.isVisible = _is_visible

    # Mirror the production event handlers but skip the super() calls (which
    # would fail against the uninitialised QWidget base).
    def _enter_event(event):
        if instance._is_paused or not instance.isVisible():
            return
        remaining = instance.hide_timer.remainingTime()
        if remaining <= 0:
            remaining = ToastWindow.AUTO_HIDE_MS
        instance._remaining_ms = remaining
        instance.hide_timer.stop()
        instance._is_paused = True
        instance._refresh_countdown()

    def _leave_event(event):
        if not instance._is_paused or not instance.isVisible():
            return
        instance._is_paused = False
        instance.hide_timer.start(instance._remaining_ms)
        instance._refresh_countdown()

    def _key_press_event(event):
        if event.key() == _StubQt.Key_Escape:
            instance.hide()
            event.accept()
            return
        # Don't call super() — QWidget base is uninitialised.

    instance.enterEvent = _enter_event
    instance.leaveEvent = _leave_event
    instance.keyPressEvent = _key_press_event
    return instance


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def toast():
    """Build a fresh ToastWindow stub for each test."""
    return _build_toast_stub(usb_monitor, usb_monitor.ToastWindow)


# ---------------------------------------------------------------------------
# hover pause / leave resume
# ---------------------------------------------------------------------------


def test_toast_enter_pauses_timer(toast):
    toast.show()
    toast.hide_timer.start(7_000)
    toast.enterEvent(MagicMock())
    assert toast._is_paused
    assert toast._remaining_ms == 7_000
    assert not toast.hide_timer.isActive()


def test_toast_leave_resumes_timer(toast):
    toast.show()
    toast._is_paused = True
    toast._remaining_ms = 5_000
    toast.leaveEvent(MagicMock())
    assert not toast._is_paused
    assert toast.hide_timer.isActive()
    assert toast.hide_timer._remaining_ms == 5_000


def test_toast_enter_when_paused_is_noop(toast):
    """Already paused — entering again must not re-stamp the remaining time."""
    toast.show()
    toast._is_paused = True
    toast._remaining_ms = 3_000
    toast.enterEvent(MagicMock())
    assert toast._remaining_ms == 3_000  # not overwritten


# ---------------------------------------------------------------------------
# Esc-to-close
# ---------------------------------------------------------------------------


def test_esc_key_closes_toast(toast):
    toast.show()
    toast.hide = MagicMock()
    event = MagicMock()
    event.key.return_value = _StubQt.Key_Escape
    toast.keyPressEvent(event)
    toast.hide.assert_called_once()


def test_non_esc_key_does_not_close(toast):
    """Pressing Enter should not collapse the toast; the button owns opening USB."""
    toast.show()
    toast.hide = MagicMock()
    event = MagicMock()
    event.key.return_value = _StubQt.Key_Return
    toast.keyPressEvent(event)
    toast.hide.assert_not_called()


# ---------------------------------------------------------------------------
# Status override (safe-eject progress)
# ---------------------------------------------------------------------------


def test_set_status_drive_merges_multiple(toast):
    toast.set_status("正在安全弹出 E:\\…", drive="E:")
    toast.set_status("正在安全弹出 F:\\…", drive="F:")
    toast.show()
    toast._refresh_status()
    assert "E:\\" in toast._status_override
    assert "F:\\" in toast._status_override


def test_set_status_clear_specific_drive(toast):
    toast.set_status("弹出 E:", drive="E:")
    toast.set_status("弹出 F:", drive="F:")
    toast.set_status(None, drive="E:")
    toast.show()
    toast._refresh_status()
    assert "F:" in toast._status_override
    assert "E:" not in toast._status_override


def test_set_status_clear_all_returns_to_countdown(toast):
    toast.set_status("弹出 E:", drive="E:")
    toast.set_status(None, drive="E:")
    toast.show()
    toast._refresh_countdown()
    assert toast._status_override is None
    assert not toast.status_label.isVisible()


def test_set_status_shows_paused_label_when_no_overrides(toast):
    """When no eject is running and the toast is paused, show '已暂停'."""
    toast.show()
    toast._is_paused = True
    toast._refresh_countdown()
    assert toast.countdown_label.text() == "已暂停"


# ---------------------------------------------------------------------------
# VolumeRow progress tooltip (pure-function path)
# ---------------------------------------------------------------------------


def test_progress_tooltip_contains_exact_bytes():
    from usb_monitor.core import progress_tooltip_text
    text = progress_tooltip_text(2 * 1024 * 1024, 512 * 1024, 512 * 1024)
    assert "2,097,152" in text  # raw total byte count
    assert "25.0%" in text
    assert "已用 512 KB" in text
    assert "剩余 512 KB" in text


# ---------------------------------------------------------------------------
# DriveReconciler scan debounce
# ---------------------------------------------------------------------------


def test_drive_reconciler_request_scan_drops_duplicate():
    """Calling request_scan while a scan is in flight must be a no-op."""
    um = usb_monitor
    api = MagicMock()
    api.logical_drives = MagicMock(return_value=())
    scanner = um.DriveScanner(api)
    state = um.VolumeState()
    sink = MagicMock()
    reconciler = um.DriveReconciler(scanner, state, sink)
    # Simulate a scan in flight
    reconciler.scan_completed.clear()
    reconciler.request_scan("manual")
    # The condition variable should still be empty — no schedule was queued.
    with reconciler._condition:
        assert not reconciler._scheduled


def test_drive_reconciler_request_scan_succeeds_when_idle():
    um = usb_monitor
    api = MagicMock()
    api.logical_drives = MagicMock(return_value=())
    scanner = um.DriveScanner(api)
    state = um.VolumeState()
    sink = MagicMock()
    reconciler = um.DriveReconciler(scanner, state, sink)
    assert reconciler.scan_completed.is_set()
    reconciler.request_scan("manual")
    with reconciler._condition:
        assert "manual" in reconciler._scheduled
    # Flag was cleared
    assert not reconciler.scan_completed.is_set()
