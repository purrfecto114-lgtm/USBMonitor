from __future__ import annotations

import ast
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_gui_entrypoint_is_syntax_valid_and_calls_application_main() -> None:
    source = (ROOT / "USBMonitor.pyw").read_text(encoding="utf-8")
    ast.parse(source)
    assert "from usb_monitor.app import main as application_main" in source
    assert 'if __name__ == "__main__"' in source


def test_console_entrypoint_help_runs() -> None:
    result = subprocess.run(
        [sys.executable, str(ROOT / "USBMonitor_console.py"), "--help"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    assert "usage:" in result.stdout.lower()


def test_module_entrypoint_help_runs() -> None:
    result = subprocess.run(
        [sys.executable, "-m", "usb_monitor", "--help"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr


def test_nuitka_script_targets_stable_root_entrypoint() -> None:
    source = (ROOT / "build" / "windows_nuitka.bat").read_text(encoding="utf-8")
    assert "set \"ENTRY=USBMonitor.pyw\"" in source
    assert "--enable-plugin=pyside6" in source
    assert "--report=build\\nuitka\\nuitka-report.xml" in source
    assert "usb_monitor\\__main__.py" not in source


def test_versions_are_consistent() -> None:
    with (ROOT / "pyproject.toml").open("rb") as handle:
        expected = tomllib.load(handle)["project"]["version"]

    declarations = {
        "package": (
            ROOT / "usb_monitor" / "__init__.py",
            r'^__version__\s*=\s*["\']([^"\']+)["\']',
        ),
        "application": (
            ROOT / "usb_monitor" / "app.py",
            r'^APP_VERSION\s*=\s*["\']([^"\']+)["\']',
        ),
        "Nuitka build": (
            ROOT / "build" / "windows_nuitka.bat",
            r'^set\s+"APP_VERSION=([^"]+)"',
        ),
    }
    for label, (path, pattern) in declarations.items():
        match = re.search(pattern, path.read_text(encoding="utf-8"), re.MULTILINE)
        assert match, f"missing version declaration for {label}"
        assert match.group(1) == expected
