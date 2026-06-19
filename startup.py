"""Windows user-login startup registration."""

from __future__ import annotations

import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Optional

from .config import APP_DISPLAY_NAME, APP_NAME, RUN_KEY, STARTUP_EXE_FILENAME, STARTUP_SCRIPT_FILENAME, app_data_dir
from .logging_setup import log_action, log_error


# ---------------------------------------------------------------------------
# Frozen / Nuitka runtime helpers
# ---------------------------------------------------------------------------

def _is_compiled_or_frozen() -> bool:
    """Return True for PyInstaller-like frozen apps and Nuitka builds.

    Nuitka deliberately does not set ``sys.frozen``. Instead it exposes
    ``__compiled__`` on compiled modules. The startup-registration code must
    treat Nuitka onefile builds as executable applications; otherwise it can
    incorrectly register the temporary unpacked ``.py`` file or ``pythonw``.
    """
    return bool(getattr(sys, "frozen", False) or globals().get("__compiled__") is not None)


def _runtime_executable_path() -> Path:
    """Return the externally launched executable for frozen/Nuitka builds.

    In Nuitka onefile mode, ``__file__`` points into the extraction directory,
    while ``sys.argv[0]`` is the original onefile executable. Prefer argv[0]
    for compiled builds so Windows Startup points at the stable EXE.
    """
    candidate = sys.argv[0] if sys.argv and sys.argv[0] else sys.executable
    return Path(candidate).resolve()


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

def _pythonw_path() -> Path:
    exe = Path(sys.executable).resolve()
    candidate = exe.with_name("pythonw.exe")
    return candidate if candidate.exists() else exe


def startup_install_dir() -> Path:
    return app_data_dir() / "startup"


def frozen_install_dir() -> Path:
    """Backward-compatible AppData folder used by older startup installs."""
    return startup_install_dir() / "bin"


def _script_source_path() -> Path:
    return Path(__file__).resolve().parent / "main.py"


def _script_install_package_dir() -> Path:
    return startup_install_dir() / "usb_monitor"


def _script_install_main_path() -> Path:
    return _script_install_package_dir() / "main.py"


def _is_unsafe_startup_location(directory: Path) -> bool:
    """Downloads/temp folders get cleared by the user or Windows; avoid them."""
    try:
        resolved = directory.resolve()
    except Exception:
        resolved = directory
    unsafe_roots = [Path.home() / "Downloads"]
    for env_name in ("TEMP", "TMP"):
        value = os.environ.get(env_name)
        if value:
            unsafe_roots.append(Path(value))
    unsafe_roots.append(Path(tempfile.gettempdir()))
    for root in unsafe_roots:
        try:
            resolved.relative_to(root.resolve())
            return True
        except Exception:
            continue
    return False


def installed_startup_source_path() -> Path:
    if _is_compiled_or_frozen():
        current_exe = _runtime_executable_path()
        if not _is_unsafe_startup_location(current_exe.parent):
            return current_exe
        # For --onefile builds, copying the whole parent directory can copy the
        # user's Downloads/project folder by accident. Keep only the executable.
        return startup_install_dir() / STARTUP_EXE_FILENAME

    source = _script_source_path()
    project_root = source.parent.parent
    if _is_unsafe_startup_location(project_root):
        return _script_install_main_path()
    return source


def _same_file_or_path(a: Path, b: Path) -> bool:
    try:
        return a.resolve().samefile(b.resolve())
    except Exception:
        return str(a.resolve()).casefold() == str(b.resolve()).casefold()


def _copy_for_startup_if_needed() -> Path:
    """Copy script/exe into AppData when the current location is volatile."""
    if _is_compiled_or_frozen():
        current_exe = _runtime_executable_path()
        if not _is_unsafe_startup_location(current_exe.parent):
            return current_exe

        target = startup_install_dir() / STARTUP_EXE_FILENAME
        target.parent.mkdir(parents=True, exist_ok=True)
        if _same_file_or_path(current_exe, target):
            return target

        try:
            if (
                target.exists()
                and current_exe.stat().st_size == target.stat().st_size
                and int(current_exe.stat().st_mtime) <= int(target.stat().st_mtime)
            ):
                return target
        except Exception:
            pass

        tmp = target.with_suffix(target.suffix + ".tmp")
        try:
            shutil.copy2(current_exe, tmp)
            tmp.replace(target)
            return target
        except PermissionError:
            try:
                tmp.unlink()
            except Exception:
                pass
            if target.exists():
                return target
            raise
        except Exception:
            try:
                tmp.unlink()
            except Exception:
                pass
            raise

    source_package = Path(__file__).resolve().parent
    source_main = source_package / "main.py"
    project_root = source_package.parent
    if not _is_unsafe_startup_location(project_root):
        return source_main

    target_package = _script_install_package_dir()
    target_main = target_package / "main.py"
    if _same_file_or_path(source_package, target_package):
        return target_main

    tmp_dir = target_package.with_name(target_package.name + ".tmp")
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo")
    try:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        shutil.copytree(source_package, tmp_dir, ignore=ignore)
        shutil.rmtree(target_package, ignore_errors=True)
        tmp_dir.replace(target_package)
        return target_main
    except PermissionError:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        if target_main.exists():
            return target_main
        raise
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise


def _startup_payload(install_copy: bool = False) -> tuple[str, list[str], str]:
    if install_copy:
        app_source = _copy_for_startup_if_needed()
    else:
        app_source = installed_startup_source_path()
        if not app_source.exists():
            app_source = Path(_runtime_executable_path() if _is_compiled_or_frozen() else __file__).resolve()
    if _is_compiled_or_frozen():
        return str(app_source), ["--startup"], str(app_source.parent)
    working_dir = app_source.parent.parent if app_source.parent.name == "usb_monitor" else app_source.parent
    return str(_pythonw_path()), [str(app_source), "--startup"], str(working_dir)


def _quote_startup_command(args: list[str]) -> str:
    return subprocess.list2cmdline(list(args))


def _startup_command(install_copy: bool = False) -> str:
    target, arguments, _working_dir = _startup_payload(install_copy=install_copy)
    return _quote_startup_command([target, *arguments])


def startup_folder_path() -> Path:
    appdata = os.environ.get("APPDATA")
    base = Path(appdata) if appdata else Path.home() / "AppData" / "Roaming"
    return base / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"


def startup_shortcut_path() -> Path:
    return startup_folder_path() / f"{APP_DISPLAY_NAME}.lnk"


def legacy_startup_cmd_path() -> Path:
    return startup_folder_path() / f"{APP_NAME}.cmd"


# ---------------------------------------------------------------------------
# Run-key
# ---------------------------------------------------------------------------

def _read_run_value() -> str:
    if platform.system() != "Windows":
        return ""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            try:
                value, _ = winreg.QueryValueEx(key, APP_NAME)
            except FileNotFoundError:
                return ""
            return str(value or "")
    except Exception:
        return ""


def _write_run_value(command: str) -> None:
    if platform.system() != "Windows":
        return
    import winreg
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
        winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, command)


def _delete_run_value() -> None:
    try:
        import winreg
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            try:
                winreg.DeleteValue(key, APP_NAME)
            except FileNotFoundError:
                pass
    except Exception:
        pass


def _delete_startup_shortcuts() -> None:
    for path in (startup_shortcut_path(), legacy_startup_cmd_path()):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except Exception:
            pass


def _startup_paths_from_command() -> dict[str, bool]:
    target, arguments, _working_dir = _startup_payload(install_copy=False)
    target_exists = Path(target).exists()
    script_or_exe = (
        Path(arguments[0])
        if arguments and str(arguments[0]).lower().endswith((".py", ".pyw", ".exe"))
        else Path(target)
    )
    return {
        "target_exists": target_exists,
        "script_or_exe_exists": script_or_exe.exists(),
        "installed_copy_exists": installed_startup_source_path().exists(),
    }


def startup_status_report() -> dict[str, Any]:
    command = startup_command_preview()
    run_value = _read_run_value()
    shortcut = startup_shortcut_path()
    legacy_cmd = legacy_startup_cmd_path()
    path_status = _startup_paths_from_command()
    shortcut_exists = shortcut.exists()
    legacy_cmd_exists = legacy_cmd.exists()
    run_registered = bool(run_value)
    enabled = shortcut_exists or legacy_cmd_exists or run_registered
    healthy = (
        enabled
        and path_status["target_exists"]
        and path_status["script_or_exe_exists"]
        and (shortcut_exists or legacy_cmd_exists)
        and run_registered
    )
    return {
        "enabled": enabled,
        "healthy": healthy,
        "shortcut_path": str(shortcut),
        "shortcut_exists": shortcut_exists,
        "legacy_cmd_path": str(legacy_cmd),
        "legacy_cmd_exists": legacy_cmd_exists,
        "run_key": RUN_KEY,
        "run_name": APP_NAME,
        "run_value": run_value,
        "run_registered": run_registered,
        "expected_command": command,
        **path_status,
    }


def is_startup_enabled() -> bool:
    return bool(startup_status_report().get("enabled"))


def startup_command_preview() -> str:
    return _startup_command(install_copy=False)


# ---------------------------------------------------------------------------
# Install / uninstall
# ---------------------------------------------------------------------------

def _create_startup_shortcut_and_run_key() -> str:
    target, arguments, working_dir = _startup_payload(install_copy=True)
    command = _quote_startup_command([target, *arguments])
    shortcut_path = startup_shortcut_path()
    shortcut_path.parent.mkdir(parents=True, exist_ok=True)
    method_parts: list[str] = []

    try:
        import win32com.client  # type: ignore[import-not-found]
        shell = win32com.client.Dispatch("WScript.Shell")
        shortcut = shell.CreateShortcut(str(shortcut_path))
        shortcut.TargetPath = target
        shortcut.Arguments = subprocess.list2cmdline(arguments)
        shortcut.WorkingDirectory = working_dir
        shortcut.Description = APP_DISPLAY_NAME
        shortcut.IconLocation = f"{target},0"
        shortcut.Save()
        try:
            legacy_startup_cmd_path().unlink()
        except FileNotFoundError:
            pass
        method_parts.append("startup_shortcut")
    except Exception as exc:
        cmd_path = legacy_startup_cmd_path()
        cmd_path.write_text(
            f"@echo off\r\nchcp 65001 >nul\r\ncd /d {subprocess.list2cmdline([working_dir])}\r\nstart \"{APP_DISPLAY_NAME}\" {command}\r\n",
            encoding="utf-8",
        )
        log_error(
            "startup_shortcut_failed_fallback_cmd",
            {"message": str(exc), "shortcut": str(shortcut_path), "cmd": str(cmd_path)},
            exc_info=True,
        )
        method_parts.append("startup_folder_cmd_fallback")

    try:
        _write_run_value(command)
        method_parts.append("run_key")
    except Exception as exc:
        log_error("startup_run_key_write_failed", {"message": str(exc), "command": command}, exc_info=True)
        if not method_parts:
            raise
    return "+".join(method_parts)


def set_startup_enabled(enabled: bool) -> str:
    if platform.system() != "Windows":
        raise RuntimeError("Only Windows startup is supported.")
    if not enabled:
        _delete_run_value()
        _delete_startup_shortcuts()
        return "disabled"
    return _create_startup_shortcut_and_run_key()


def repair_startup_registration_if_needed() -> Optional[str]:
    if platform.system() != "Windows":
        return None
    report = startup_status_report()
    if not report.get("enabled"):
        return None
    if report.get("healthy"):
        return None
    return set_startup_enabled(True)
