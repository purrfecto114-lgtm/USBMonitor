"""TDD regression tests for the 8 bugs fixed in stage2.

Every test here is annotated with the specific breakage it would catch if
the corresponding fix were reverted (i.e. if the original USBMonitor-main
code were re-installed).  Run against the original tree these tests must
FAIL (red); run against USBMonitor-stage2 they must PASS (green) — that is
the red→green evidence required by the test-driven-development skill.

Design choices (per writing-good-tests.md):

* "Point at the breakage": each docstring states which line/behaviour would
  regress and how the assertion would fail.  No opaque boolean checks.
* "Run the real thing": behavioural tests use the real ``LoggingManager``,
  ``WindowsStorageApi`` (constructed via ``__new__`` to skip the
  Windows-only ``__init__``), and real on-disk files in ``tmp_path``.
* "Platform compatible": tests run on Linux without PySide6/pywin32 by
  using AST-based source inspection for Qt-only classes
  (``ToastWindow``, ``TrayMenuController``) and by stubbing only the
  Win32 kernel32 entry points that cannot exist on Linux.
* "No test stubs": assertions check observable state (file contents,
  method calls, attribute presence, source structure) rather than
  verifying that mocks were invoked in a particular order.
"""

from __future__ import annotations

import ast
import inspect
import re
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# Make the package importable when running ``pytest`` from anywhere.
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from usb_monitor.app import (
    GUID,
    LoggingManager,
    LogConfig,
    LogMode,
    WindowsStorageApi,
    merge_cli_config,
    parse_args,
    usb_interface_guid,
)
from usb_monitor.core import AppConfig, normalize_recent_records

# Read the source once and parse the AST once — many tests inspect the
# source of classes that only exist inside ``if QT_AVAILABLE:`` blocks
# (so they cannot be imported on Linux without PySide6).
APP_SOURCE = (ROOT / "usb_monitor" / "app.py").read_text(encoding="utf-8")
APP_TREE = ast.parse(APP_SOURCE)
BUILD_BAT = (ROOT / "build" / "windows_nuitka.bat").read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# AST helpers — extract source of classes/methods defined inside
# ``if QT_AVAILABLE:`` blocks so we can inspect them without importing Qt.
# ---------------------------------------------------------------------------

def _find_class(name: str) -> ast.ClassDef | None:
    """Return the first top-level or ``if``-guarded ``ClassDef`` named ``name``."""
    for node in ast.walk(APP_TREE):
        if isinstance(node, ast.ClassDef) and node.name == name:
            return node
    return None


def _method_source(class_name: str, method_name: str) -> str:
    """Return the source text of ``Class.method``.

    ``ast.walk`` finds classes defined both at module top level and inside
    ``if QT_AVAILABLE:`` / ``if not QT_AVAILABLE:`` guards.  When several
    classes share a name (e.g. the stub ``ToastWindow`` and the real
    QWidget-derived one), we return the *first* class whose body contains
    the requested method.
    """
    for node in ast.walk(APP_TREE):
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for stmt in node.body:
                if isinstance(stmt, ast.FunctionDef) and stmt.name == method_name:
                    segment = ast.get_source_segment(APP_SOURCE, stmt)
                    if segment is not None:
                        return segment
    pytest.fail(f"method {class_name}.{method_name} not found in app.py source")


# ===========================================================================
# Bug 1: smart-pen / HID device misidentified as unallocated USB drive
# ===========================================================================

def test_usb_interface_guid_uses_volume_interface() -> None:
    """``usb_interface_guid()`` must return GUID_DEVINTERFACE_VOLUME (Data1=0x53F5630D).

    The previous GUID_DEVINTERFACE_USB_DEVICE (0xA5DCBF10) covers ALL USB
    devices including HID (smart pens, keyboards, mice).  Their plug/unplug
    events therefore triggered a drive scan that misclassified them as
    unallocated USB drives.  Switching to the volume-only GUID restricts
    DBT_DEVICEARRIVAL to actual file-system volumes.

    If reverted: ``guid.Data1 == 0xA5DCBF10`` and this assertion fails.
    """
    guid = usb_interface_guid()
    assert isinstance(guid, GUID)
    assert guid.Data1 == 0x53F5630D, (
        f"usb_interface_guid must use GUID_DEVINTERFACE_VOLUME "
        f"(Data1=0x53F5630D), got Data1=0x{guid.Data1:08X}"
    )


def test_volume_storage_is_external_method_exists() -> None:
    """``WindowsStorageApi`` must expose ``volume_storage_is_external``.

    This is the fallback path used by ``DriveScanner._classify`` when
    ``volume_disk_numbers`` returns empty (smart-pen emulated LFB / HID
    interfaces that expose a drive letter but have no disk extent).
    The previous code lacked this method and fell back to the weak
    heuristic ``drive_type_code == DRIVE_REMOVABLE and path != system_drive``.

    If reverted: ``hasattr(WindowsStorageApi, "volume_storage_is_external")``
    is False (AttributeError on the class).
    """
    assert hasattr(WindowsStorageApi, "volume_storage_is_external"), (
        "WindowsStorageApi must define volume_storage_is_external"
    )


def test_volume_storage_is_external_returns_false_when_handle_unavailable() -> None:
    """``volume_storage_is_external`` must return False (not raise) when the volume can't be opened.

    This is the safety net that prevents smart-pen paths from being
    misclassified as USB drives.  On Windows, ``CreateFileW`` returns
    ``INVALID_HANDLE_VALUE`` for paths that have no real storage extent;
    on non-Windows the same path is exercised via the test stub below.
    Returning False lets ``_classify`` correctly treat the path as
    "not external" instead of raising or falling back to drive-type guessing.

    If reverted: the method doesn't exist (AttributeError) or it raises
    ``AttributeError`` because ``kernel32`` is None on non-Windows.
    """
    # Bypass __init__ which raises RuntimeError on non-Windows.
    api = WindowsStorageApi.__new__(WindowsStorageApi)
    # Simulate "CreateFileW returned INVALID_HANDLE_VALUE" — what happens
    # for smart-pen emulated LFB paths that have no real storage extent.
    api._open = lambda path: None  # type: ignore[assignment]
    assert api.volume_storage_is_external("X:\\") is False


def test_classify_uses_volume_storage_when_disk_numbers_empty() -> None:
    """``DriveScanner._classify`` must call ``volume_storage_is_external`` when disk_numbers is empty.

    The previous weak heuristic decided "external" purely from
    ``DRIVE_REMOVABLE and path != system_drive``.  The fix delegates to
    the new method which opens the volume and queries its
    ``STORAGE_DEVICE_DESCRIPTOR`` for ``bus_type`` / ``removable_media``.

    If reverted: ``_classify``'s else branch contains the drive-type
    heuristic and never references ``volume_storage_is_external``.
    """
    src = _method_source("DriveScanner", "_classify")
    assert "volume_storage_is_external" in src, (
        "DriveScanner._classify must call self.api.volume_storage_is_external(path) "
        "when disk_numbers is empty (instead of the old drive-type heuristic)"
    )


# ===========================================================================
# Bug 2: toast animation stutter
# ===========================================================================

def test_restore_delays_reduced_to_two_passes() -> None:
    """``ToastWindow._RESTORE_DELAYS_MS`` must be ``(0, 200)`` — not ``(0, 50, 250)``.

    The old triple-delay fired three ``QTimer.singleShot`` reposition
    passes per refresh; combined with the row teardown/rebuild this caused
    visible stutter.  Compressing to a single immediate + 200ms settle
    pass cuts the reposition count by 1/3.

    The real ``ToastWindow(QWidget)`` lives inside ``if QT_AVAILABLE:``
    and cannot be imported on Linux without PySide6, so we inspect the
    source directly.

    If reverted: the regex captures ``0, 50, 250`` and the assertion fails.
    """
    match = re.search(r"_RESTORE_DELAYS_MS\s*=\s*\(([^)]+)\)", APP_SOURCE)
    assert match, "_RESTORE_DELAYS_MS not found in app.py source"
    values = tuple(int(v.strip()) for v in match.group(1).split(","))
    assert values == (0, 200), (
        f"_RESTORE_DELAYS_MS must be (0, 200), got {values}"
    )


def test_toast_window_has_row_cache_and_filter_id_set() -> None:
    """``ToastWindow.__init__`` must initialize ``_row_cache`` and ``_filter_installed_ids``.

    The row cache reuses existing ``VolumeRow`` instances across refreshes
    instead of tearing down and rebuilding every row — the primary cause
    of the animation stutter.  ``_filter_installed_ids`` tracks which rows
    already have the interaction ``eventFilter`` installed so we don't
    install it twice on a reused row.

    If reverted: neither attribute appears anywhere in the source, and
    the row-reuse optimisation doesn't exist.
    """
    assert re.search(r"self\._row_cache\b", APP_SOURCE), (
        "ToastWindow.__init__ must initialize self._row_cache for row reuse"
    )
    assert re.search(r"self\._filter_installed_ids\b", APP_SOURCE), (
        "ToastWindow.__init__ must initialize self._filter_installed_ids"
    )


# ===========================================================================
# Bug 3: PySide6 notification silently degraded to tray-only
# ===========================================================================

def test_startup_silent_does_not_override_qt_toast_backend(tmp_path: Path) -> None:
    """``merge_cli_config(['--startup', '--silent'])`` must NOT force ``gui_backend`` to tray-only.

    The previous ``elif getattr(args, "silent", False) or getattr(args,
    "startup", False): config.gui_backend = "tray-only"`` overrode the
    user's saved ``gui_backend="qt-toast"`` whenever the app was launched
    via the HKCU Run key (which passes ``["--startup", "--silent"]``).
    The user then lost the PySide6 ``ToastWindow`` and fell back to
    ``QSystemTrayIcon.showMessage``, which on Win10 1903+ silently
    returns success without showing anything.

    If reverted: ``config.gui_backend`` becomes ``"tray-only"`` and the
    assertion fails.
    """
    args = parse_args(["--startup", "--silent"])
    config = merge_cli_config(args, AppConfig(log_dir=tmp_path))
    assert config.gui_backend == "qt-toast", (
        f"--startup --silent must not override gui_backend "
        f"(got {config.gui_backend!r})"
    )


# ===========================================================================
# Bug 4: tray right-click menu overflow when many recent volumes
# ===========================================================================

def test_refresh_volume_menu_paginates_recent_records() -> None:
    """``refresh_volume_menu`` must paginate: first 5 recent records inline, rest in a "更多（N 项）" submenu.

    The previous code rendered all 10 records inline, each opening a
    submenu of 2-3 actions — totalling 30+ items and overflowing the
    screen on 1080p default DPI.  The fix slices into a 5-item preview
    plus an overflow submenu.

    If reverted: no ``RECENT_PREVIEW = 5`` constant and no ``更多（`` menu
    is created, both assertions fail.
    """
    src = _method_source("TrayMenuController", "refresh_volume_menu")
    assert "RECENT_PREVIEW = 5" in src, (
        "refresh_volume_menu must define RECENT_PREVIEW = 5 to cap inline items"
    )
    assert "更多（" in src, (
        "refresh_volume_menu must create a '更多（N 项）' overflow submenu"
    )


def test_normalize_recent_records_surfaces_more_than_five() -> None:
    """``normalize_recent_records`` must return more than 5 records when present.

    The paging logic in ``refresh_volume_menu`` relies on
    ``normalize_recent_records`` returning all available records; the
    slicing into preview/overflow happens in the menu builder, not here.
    """
    records = [
        {
            "path": f"{chr(ord('C') + i)}:\\",
            "title": f"Drive {i}",
            "last_seen_utc": f"2026-01-{i + 1:02d}",
        }
        for i in range(7)
    ]
    result = normalize_recent_records(records)
    assert len(result) == 7, (
        f"normalize_recent_records must return all 7 records (got {len(result)})"
    )


# ===========================================================================
# Bug 5: tray menu hard to click
# ===========================================================================

def test_tray_activated_suppress_threshold_below_100ms() -> None:
    """``_on_tray_activated`` suppress threshold must be < 0.10s (i.e. ≤ 0.05s).

    The old 0.20s (200ms) threshold rejected fast double-clicks, making
    the tray menu feel "hard to click" — the user clicked twice and the
    second click was swallowed.  Lowering to 50ms removes the
    double-click ghosting without eating legitimate rapid interactions.

    If reverted: the regex captures ``0.20`` and the ``< 0.10`` assertion
    fails.
    """
    src = _method_source("TrayMenuController", "_on_tray_activated")
    match = re.search(r"_last_device_menu_popup\s*<\s*(0\.\d+)", src)
    assert match, "could not find _last_device_menu_popup suppress threshold"
    threshold = float(match.group(1))
    assert threshold < 0.10, (
        f"tray-activated suppress threshold must be < 0.10s (got {threshold}s)"
    )


def test_tray_popup_position_uses_available_geometry_bottom() -> None:
    """``_tray_popup_position`` must use ``availableGeometry().bottom()`` as the fallback y.

    The previous fallback ``return QCursor.pos()`` placed the menu's
    top-left at the cursor, causing ``QMenu.popup()`` to expand down/right
    and overlap the taskbar (which is precisely what made the menu "hard
    to click" on Windows).  Using ``avail.bottom()`` (the top of the
    taskbar) leaves no space below, so Qt auto-flips the menu upward.

    If reverted: ``_tray_popup_position`` just returns ``QCursor.pos()``
    with no ``availableGeometry`` / ``bottom()`` call.
    """
    src = _method_source("TrayMenuController", "_tray_popup_position")
    assert "availableGeometry" in src, (
        "_tray_popup_position must consult screen.availableGeometry()"
    )
    assert "bottom()" in src, (
        "_tray_popup_position must use avail.bottom() so QMenu auto-flips upward"
    )


# ===========================================================================
# Bug 6: "clear logs" silently fails to clear
# ===========================================================================

def test_logging_stop_joins_listener_thread() -> None:
    """``LoggingManager.stop()`` must join the listener thread, not just enqueue a sentinel.

    ``stdlib`` ``QueueListener.stop()`` only puts a sentinel on the queue
    and returns; the listener thread may still be flushing records and
    holding the ``RotatingFileHandler`` file handle.  On Windows that
    prevents ``reset_files`` from unlinking the log files (the OS refuses
    to delete files with open handles).  The fix explicitly joins the
    thread (with a finite timeout to avoid deadlock).

    If reverted: ``thread.join`` is never called and the assertion fails.
    """
    manager = LoggingManager()
    fake_thread = MagicMock()
    fake_listener = MagicMock()
    fake_listener._thread = fake_thread
    manager._listener = fake_listener

    manager.stop()

    fake_listener.stop.assert_called_once()
    fake_thread.join.assert_called_once()
    # Verify the timeout is finite (None would block forever on a wedged thread).
    args, kwargs = fake_thread.join.call_args
    timeout = kwargs.get("timeout", args[0] if args else None)
    assert timeout is not None, "thread.join must be called with a finite timeout"
    assert 0 < float(timeout) <= 5.0, (
        f"thread.join timeout must be reasonable (got {timeout})"
    )


def test_reset_files_defaults_to_include_crash() -> None:
    """``reset_files(include_crash=...)`` must default to True.

    The previous default ``False`` left ``crash.log`` untouched by the
    "clear logs" tray menu action, allowing it to grow unbounded (see
    bug 7).  The fix flips the default so the user-visible "clear logs"
    actually clears every log file.

    If reverted: ``inspect.signature`` reports ``include_crash=False`` and
    the assertion fails.
    """
    sig = inspect.signature(LoggingManager.reset_files)
    param = sig.parameters["include_crash"]
    assert param.default is True, (
        f"reset_files(include_crash) must default to True (got {param.default!r})"
    )


def test_reset_files_truncates_when_unlink_fails(tmp_path: Path, monkeypatch) -> None:
    """``reset_files`` must fall back to truncating when ``unlink()`` raises OSError.

    On Windows, file handles held by other processes (or by the recently-
    stopped listener thread) cause ``unlink`` to fail.  The old code did
    ``except OSError: continue`` — silently leaving the file untouched,
    so users saw "logs cleared" but the bytes remained.  The fix calls
    ``path.write_bytes(b"")`` to truncate as a fallback.

    If reverted: ``write_bytes`` is never called and the assertion fails.
    """
    log_path = tmp_path / "events.log"
    log_path.write_bytes(b"old log content that should be wiped")

    # Save the real write_bytes before patching, so our tracker can delegate.
    real_write_bytes = Path.write_bytes
    write_calls: list[tuple[str, bytes]] = []

    # Force unlink to always fail (simulates Windows file-in-use).
    def failing_unlink(self: Path, *args: object, **kwargs: object) -> None:
        raise OSError("access denied (simulated)")

    monkeypatch.setattr(Path, "unlink", failing_unlink)

    # Track write_bytes calls without breaking the truncation fallback.
    def tracked_write_bytes(self: Path, data: bytes) -> int:
        write_calls.append((str(self), bytes(data)))
        return real_write_bytes(self, data)

    monkeypatch.setattr(Path, "write_bytes", tracked_write_bytes)

    # Don't call configure() — we don't want to install sys.excepthook
    # globally.  reset_files works without a configured listener.
    manager = LoggingManager()
    manager.reset_files(tmp_path)  # default include_crash=True

    events_truncated = any("events.log" in path for path, _ in write_calls)
    assert events_truncated, (
        "reset_files must call write_bytes(b'') on events.log when unlink fails"
    )
    assert log_path.read_bytes() == b"", (
        "events.log must be empty after the truncate fallback"
    )


# ===========================================================================
# Bug 7: crash.log grows without bound
# ===========================================================================

def test_crash_log_size_and_backup_constants() -> None:
    """``LoggingManager`` must define ``CRASH_LOG_MAX_BYTES = 512KB`` and ``CRASH_LOG_BACKUPS = 2``.

    The previous ``write_crash`` opened ``crash.log`` in append mode with
    no rotation, so repeated crashes (especially crash loops via
    ``sys.excepthook``) could fill the disk.  The fix adds explicit
    rotation constants modelled on ``RotatingFileHandler``.

    If reverted: the constants don't exist (``AttributeError``).
    """
    assert hasattr(LoggingManager, "CRASH_LOG_MAX_BYTES"), (
        "LoggingManager must define CRASH_LOG_MAX_BYTES"
    )
    assert hasattr(LoggingManager, "CRASH_LOG_BACKUPS"), (
        "LoggingManager must define CRASH_LOG_BACKUPS"
    )
    assert LoggingManager.CRASH_LOG_MAX_BYTES == 512 * 1024, (
        f"CRASH_LOG_MAX_BYTES must be 512KB "
        f"(got {LoggingManager.CRASH_LOG_MAX_BYTES})"
    )
    assert LoggingManager.CRASH_LOG_BACKUPS == 2, (
        f"CRASH_LOG_BACKUPS must be 2 (got {LoggingManager.CRASH_LOG_BACKUPS})"
    )


def test_rotate_crash_log_promotes_backups(tmp_path: Path) -> None:
    """``_rotate_crash_log`` must shift ``crash.log.1 -> .2`` and ``crash.log -> .1``.

    Mirrors ``RotatingFileHandler``'s behaviour: the oldest backup is
    deleted, remaining backups shift up by one index, and the current
    log is renamed to ``.1``.  This is invoked by ``write_crash`` before
    each append when the current ``crash.log`` exceeds the size limit.

    If reverted: ``_rotate_crash_log`` doesn't exist (``AttributeError``)
    or the rotation order is wrong.
    """
    crash_path = tmp_path / "crash.log"
    crash1 = tmp_path / "crash.log.1"
    crash2 = tmp_path / "crash.log.2"

    # Pre-state: crash.log and crash.log.1 both exist; .2 does not.
    crash_path.write_text("current", encoding="utf-8")
    crash1.write_text("previous backup", encoding="utf-8")
    assert not crash2.exists()

    manager = LoggingManager()
    manager._rotate_crash_log(crash_path)

    assert crash1.exists(), "crash.log.1 must exist after rotation"
    assert crash2.exists(), "crash.log.2 must exist after rotation (old .1 promoted)"
    assert crash1.read_text(encoding="utf-8") == "current", (
        "old crash.log must be promoted to crash.log.1"
    )
    assert crash2.read_text(encoding="utf-8") == "previous backup", (
        "old crash.log.1 must be promoted to crash.log.2"
    )


def test_write_crash_rotates_oversized_file(tmp_path: Path) -> None:
    """``write_crash`` must rotate ``crash.log`` when it exceeds ``CRASH_LOG_MAX_BYTES``.

    The previous ``write_crash`` unconditionally appended, allowing
    unbounded growth.  The fix checks ``stat().st_size`` before each
    write and rotates when ``>= MAX_BYTES``.

    If reverted: ``crash.log.1`` is never created and the assertion fails.
    """
    manager = LoggingManager()
    # Set _config directly to avoid configure() installing sys.excepthook
    # globally (which would pollute other tests).
    manager._config = LogConfig(tmp_path, LogMode.REDACTED, 10_000, 0, False)

    # Pre-write an oversized crash.log to trigger rotation on next write.
    crash_path = tmp_path / "crash.log"
    oversize_bytes = b"x" * (LoggingManager.CRASH_LOG_MAX_BYTES + 100)
    crash_path.write_bytes(oversize_bytes)

    # write_crash expects a real exception triplet.
    try:
        raise ValueError("synthetic crash for rotation test")
    except ValueError as exc:
        tb = exc.__traceback__
        manager.write_crash(ValueError, exc, tb)

    # The pre-existing oversized crash.log must have been rotated to .1.
    rotated = tmp_path / "crash.log.1"
    assert rotated.exists(), "write_crash must rotate oversized crash.log to .1"
    assert rotated.read_bytes() == oversize_bytes, (
        "rotated crash.log.1 must contain the original oversized content"
    )
    # And a fresh crash.log is created with the new crash record.
    assert crash_path.exists(), "fresh crash.log must be created after rotation"
    new_content = crash_path.read_text(encoding="utf-8")
    assert "synthetic crash for rotation test" in new_content, (
        "new crash.log must contain the just-written crash record"
    )


# ===========================================================================
# Bug 8: binary size — IOCTL eject path, lazy pywin32, Nuitka debloat
# ===========================================================================

def test_safe_eject_drive_does_not_harddepend_on_pywin32() -> None:
    """``safe_eject_drive`` must NOT import ``win32com`` at the top of its function body.

    The main eject path now uses ``WindowsStorageApi.eject_volume``
    (IOCTL via ctypes).  ``pywin32``'s ``win32com.client`` is imported
    only inside the fallback ``_eject_via_shell_application`` (a separate
    function defined elsewhere), so a successful IOCTL eject never loads
    pywin32 (~10MB) into the process or into the Nuitka bundle.

    If reverted: ``import win32com.client`` appears at the top of
    ``safe_eject_drive``'s body, and the AST walk finds it.
    """
    func = None
    for node in ast.walk(APP_TREE):
        if isinstance(node, ast.FunctionDef) and node.name == "safe_eject_drive":
            func = node
            break
    assert func is not None, "safe_eject_drive function not found"

    # Walk the function subtree looking for any win32com imports.
    # safe_eject_drive calls _eject_via_shell_application (defined as a
    # separate FunctionDef at module level); its body is NOT walked here.
    for node in ast.walk(func):
        if isinstance(node, ast.Import):
            for alias in node.names:
                assert not alias.name.startswith("win32com"), (
                    f"safe_eject_drive must not import win32com at top level "
                    f"(found 'import {alias.name}')"
                )
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            assert not module.startswith("win32com"), (
                f"safe_eject_drive must not import from win32com at top level "
                f"(found 'from {module} import ...')"
            )


def test_windows_storage_api_has_eject_volume_using_ioctl() -> None:
    """``WindowsStorageApi.eject_volume`` must exist and use ``IOCTL_STORAGE_EJECT_MEDIA``.

    The previous ``safe_eject_drive`` called ``Shell.Application.InvokeVerb("Eject")``
    which (a) drags pywin32 into the build, (b) depends on locale verb
    names ("弹出" / "Eject" / "Safely Remove").  The new ``eject_volume``
    uses ``IOCTL_STORAGE_EJECT_MEDIA`` via ``DeviceIoControl`` — pure
    ctypes, locale-independent.

    If reverted: ``WindowsStorageApi`` has no ``eject_volume`` method
    (``AttributeError``), or the method doesn't reference the IOCTL.
    """
    assert hasattr(WindowsStorageApi, "eject_volume"), (
        "WindowsStorageApi must define eject_volume"
    )

    cls = _find_class("WindowsStorageApi")
    assert cls is not None, "WindowsStorageApi class not found"

    method = None
    for stmt in cls.body:
        if isinstance(stmt, ast.FunctionDef) and stmt.name == "eject_volume":
            method = stmt
            break
    assert method is not None, "eject_volume method missing from WindowsStorageApi AST"

    method_src = ast.get_source_segment(APP_SOURCE, method)
    assert method_src is not None
    assert "IOCTL_STORAGE_EJECT_MEDIA" in method_src, (
        "eject_volume must use IOCTL_STORAGE_EJECT_MEDIA via DeviceIoControl"
    )


def test_nuitka_build_excludes_unused_pyside6_modules() -> None:
    """Nuitka build script must include ``--nofollow-import-to`` for unused Qt modules.

    The previous build script had only 3 nofollow flags (``tkinter``,
    ``pytest``, ``unittest``); ``PySide6.QtNetwork/QtQml/QtQuick/QtWebEngine``
    etc. were all pulled in (~30MB of unused binaries).  The fix adds ~32
    module-exclusion flags covering the bulk of the unused Qt surface.

    If reverted: the ``--nofollow-import-to=PySide6.QtNetwork`` flag (and
    the others) is absent from the build script.
    """
    required_exclusions = [
        "PySide6.QtNetwork",
        "PySide6.QtQml",
        "PySide6.QtQuick",
        "PySide6.QtMultimedia",
        "PySide6.QtWebEngine",
        "PySide6.QtSql",
        "PySide6.QtPrintSupport",
    ]
    for module in required_exclusions:
        flag = f"--nofollow-import-to={module}"
        assert flag in BUILD_BAT, (
            f"build/windows_nuitka.bat must include {flag} to exclude {module}"
        )
