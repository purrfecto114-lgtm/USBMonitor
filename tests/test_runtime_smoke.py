from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from usb_monitor.app import StartupManager


def test_source_startup_install_copies_complete_importable_package(tmp_path: Path) -> None:
    manager = StartupManager()
    manager.install_dir = tmp_path / "startup"

    launcher = manager._stable_source(install=True)

    assert launcher.exists()
    assert (manager.install_dir / "source" / "usb_monitor" / "__main__.py").exists()
    assert (manager.install_dir / "source" / "usb_monitor" / "core.py").exists()
    assert (manager.install_dir / "source" / "usb_monitor" / "hooks.py").exists()

    env = os.environ.copy()
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    completed = subprocess.run(
        [sys.executable, str(launcher), "--startup-status"],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0
    assert "ImportError" not in completed.stderr
    assert "attempted relative import" not in completed.stderr


def test_real_qt_gui_constructs_in_clean_subprocess(tmp_path: Path) -> None:
    pytest.importorskip("PySide6")
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    script = r"""
import tempfile
from pathlib import Path
import usb_monitor.app as app

class State:
    def snapshot(self): return ()
class Service:
    def __init__(self, sink): self.state = State(); self.sink = sink
    def start(self): pass
    def stop(self): pass
    def rescan(self): pass

app.UsbMonitorService = Service
tmp = Path(tempfile.mkdtemp())
runtime = app.GuiRuntime(app.parse_args([]), app.AppConfig(log_dir=tmp / 'logs'), app.ConfigStore(tmp / 'config.json'), app.StartupManager())
runtime.app.processEvents()
if runtime.menu is not None:
    runtime.menu.refresh_volume_menu()
    runtime.menu.update_dynamic_state()
runtime.shutdown()
print('QT_SMOKE_OK')
"""
    completed = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert "QT_SMOKE_OK" in completed.stdout
