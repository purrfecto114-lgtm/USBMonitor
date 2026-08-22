"""Regression coverage for the v1.3.0 closed-loop product iteration."""
from __future__ import annotations

import json
from pathlib import Path

from usb_monitor.app import ConfigStore, merge_cli_config, parse_args, wait_for_drive_removal
from usb_monitor.core import AppConfig, normalize_hook_rules

ROOT = Path(__file__).resolve().parents[1]
APP_SOURCE = ROOT / "usb_monitor" / "app.py"
HOOKS_SOURCE = ROOT / "usb_monitor" / "hooks.py"


def test_hook_rules_are_bounded_normalized_and_deduplicated() -> None:
    rules = normalize_hook_rules(
        [
            {
                "name": " Backup ",
                "match_labels": ["BACKUP*", ""],
                "command": ["powershell", "-File", "backup.ps1", "{path}"],
                "debounce_seconds": "0",
                "enabled": "yes",
                "ignored": "field",
            },
            {"name": "backup", "command": ["duplicate.exe"]},
            {"name": "missing-command"},
            {"name": "bad-command", "command": "cmd.exe /c unsafe"},
        ]
    )

    assert rules == [
        {
            "name": "Backup",
            "match_paths": [],
            "match_labels": ["BACKUP*"],
            "command": ["powershell", "-File", "backup.ps1", "{path}"],
            "debounce_seconds": 0.1,
            "enabled": True,
        }
    ]


def test_config_store_round_trips_hooks(tmp_path: Path) -> None:
    path = tmp_path / "config.json"
    store = ConfigStore(path)
    config = AppConfig(
        log_dir=tmp_path / "logs",
        hooks=[
            {
                "name": "auto-backup",
                "match_labels": ["BACKUP*"],
                "command": ["backup.exe", "{path}"],
                "debounce_seconds": 3,
                "enabled": True,
            }
        ],
    )

    store.save(config)
    raw = json.loads(path.read_text(encoding="utf-8"))
    loaded = store.load()

    assert raw["version"] == 3
    assert raw["hooks"] == loaded.hooks
    assert loaded.hooks[0]["name"] == "auto-backup"
    assert loaded.hooks[0]["command"] == ["backup.exe", "{path}"]


def test_cli_merge_copies_hook_collection(tmp_path: Path) -> None:
    stored = AppConfig(log_dir=tmp_path, hooks=[{"name": "x", "command": ["x.exe"]}])
    merged = merge_cli_config(parse_args([]), stored)

    merged.hooks[0]["name"] = "changed"
    assert stored.hooks[0]["name"] == "x"




def test_eject_success_requires_drive_to_become_inaccessible() -> None:
    assert wait_for_drive_removal("E:/", timeout=0, exists=lambda _path: False)
    assert not wait_for_drive_removal("E:/", timeout=0, exists=lambda _path: True)

def test_safe_eject_uses_canonical_key_duplicate_guard_and_finished_cleanup() -> None:
    source = APP_SOURCE.read_text(encoding="utf-8")

    assert "if drive in self._eject_in_flight:" in source
    assert "self._eject_threads[drive] = worker" in source
    assert "worker.finished.connect(partial(self._on_eject_finished, drive, worker))" in source
    assert "self._eject_threads.pop(path, None)" not in source


def test_tray_exposes_persistent_status_and_recent_records() -> None:
    source = APP_SOURCE.read_text(encoding="utf-8")

    assert 'self.task_status_action = self.menu.addAction("状态：就绪")' in source
    assert 'recent_menu = self.volume_menu.addMenu("最近使用")' in source
    assert 'eject.setText("正在安全弹出…")' in source
    assert 'item_menu.addAction("复制盘符"' in source


def test_hooks_use_explicit_shell_false_and_do_not_log_full_argv() -> None:
    source = HOOKS_SOURCE.read_text(encoding="utf-8")

    assert "shell=False" in source
    assert "stdout=subprocess.DEVNULL" in source
    assert 'extra={"rule": rule.name, "cmd":' not in source
    assert '.format(path=' not in source


def test_release_version_is_consistent() -> None:
    import usb_monitor
    from usb_monitor.app import APP_VERSION

    build = (ROOT / "build" / "windows_nuitka.bat").read_text(encoding="utf-8")
    assert usb_monitor.__version__ == APP_VERSION
    assert APP_VERSION.count(".") == 2
    assert f'set "APP_VERSION={APP_VERSION}"' in build


def test_hook_runner_executes_argv_without_shell(monkeypatch) -> None:
    from usb_monitor.core import VolumeInfo
    from usb_monitor.hooks import HookRule, HookRunner
    import usb_monitor.hooks as hooks_module

    submitted: list[object] = []
    calls: list[tuple[tuple[str, ...], dict[str, object]]] = []

    class ImmediateExecutor:
        def __init__(self, *args, **kwargs) -> None:
            pass

        def submit(self, fn):
            submitted.append(fn)
            fn()

        def shutdown(self, **kwargs) -> None:
            pass

    def fake_run(cmd, **kwargs):
        calls.append((tuple(cmd), kwargs))
        return object()

    monkeypatch.setattr(hooks_module, "ThreadPoolExecutor", ImmediateExecutor)
    monkeypatch.setattr(hooks_module.subprocess, "run", fake_run)
    runner = HookRunner([HookRule(name="backup", command=("backup.exe", "{path}", "{label}"))])
    volume = VolumeInfo(
        path="E:\\",
        title="Backup",
        drive_type="removable",
        disk_number=1,
        total=100,
        used=50,
        free=50,
        label="BACKUP",
    )

    runner._fire(runner._rules[0], volume)

    assert submitted
    assert calls[0][0] == ("backup.exe", "E:\\", "BACKUP")
    assert calls[0][1]["shell"] is False
    assert calls[0][1]["stdout"] is hooks_module.subprocess.DEVNULL
    runner.stop()


def test_hook_runner_rejects_unknown_placeholder(monkeypatch) -> None:
    from usb_monitor.core import VolumeInfo
    from usb_monitor.hooks import HookRule, HookRunner
    import usb_monitor.hooks as hooks_module

    calls: list[object] = []

    class ImmediateExecutor:
        def __init__(self, *args, **kwargs) -> None:
            pass

        def submit(self, fn):
            calls.append(fn)
            fn()

        def shutdown(self, **kwargs) -> None:
            pass

    monkeypatch.setattr(hooks_module, "ThreadPoolExecutor", ImmediateExecutor)
    runner = HookRunner([HookRule(name="bad", command=("tool.exe", "{path.__class__}"))])
    volume = VolumeInfo(
        path="E:\\",
        title="USB",
        drive_type="removable",
        disk_number=None,
        total=None,
        used=None,
        free=None,
        label=None,
    )

    runner._fire(runner._rules[0], volume)

    assert calls == []
    runner.stop()
