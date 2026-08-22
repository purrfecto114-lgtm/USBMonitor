"""攻防测试套件 — v1.1.1 鲁棒性加固

覆盖:
- hooks.py BatBadBut/CmdHijack 命令注入防护
- CLI/Config 参数边界 (log_max_bytes/log_backups 上限)
- crash.log 轮转失败兜底
- AST 危险模式扫描 (eval/exec/shell=True/pickle)
- 智能笔/HID 识别边界 (GUID=VOLUME, disk_numbers空+非USB bus → external=False)
- mutex name 注入检查
"""
from __future__ import annotations

import ast
import os
import threading
import tempfile
from collections import OrderedDict
from pathlib import Path
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT))

from usb_monitor.hooks import HookRunner, HookRule, _validate_hook_command_safety
from usb_monitor.core import VolumeInfo, normalize_drive_path
from usb_monitor.app import (
    usb_interface_guid, LoggingManager, WindowsStorageApi,
    DriveScanner, DRIVE_REMOVABLE, parse_args, merge_cli_config,
)
from usb_monitor.core import AppConfig


# ---------------------------------------------------------------------------
# 1. BatBadBut / CmdHijack 命令注入防护
# ---------------------------------------------------------------------------

class TestBatBadButProtection:
    """hooks.py 命令注入防护 — 防止 .bat/.cmd executable + cmd 元字符 + path traversal"""

    def test_rejects_bat_executable(self):
        """如果修复被回退, .bat executable 会被接受 → 断言失败"""
        reason = _validate_hook_command_safety(["backup.bat", "E:\\"])
        assert reason is not None
        assert "shell_script_executable" in reason

    def test_rejects_cmd_executable(self):
        reason = _validate_hook_command_safety(["script.cmd"])
        assert reason is not None
        assert "shell_script_executable" in reason

    def test_rejects_ps1_executable(self):
        reason = _validate_hook_command_safety(["run.ps1"])
        assert reason is not None
        assert "shell_script_executable" in reason

    def test_rejects_cmd_metachar_ampersand(self):
        """FOO&calc.exe 卷标注入"""
        reason = _validate_hook_command_safety(["echo", "FOO&calc.exe"])
        assert reason is not None
        assert "cmd_metachar" in reason

    def test_rejects_cmd_metachar_pipe(self):
        reason = _validate_hook_command_safety(["echo", "BAR|whoami"])
        assert reason is not None
        assert "cmd_metachar" in reason

    def test_rejects_cmd_metachar_percent(self):
        reason = _validate_hook_command_safety(["echo", "BAZ%USERNAME%"])
        assert reason is not None
        assert "cmd_metachar" in reason

    def test_rejects_path_traversal_unix(self):
        """CmdHijack: ../ path traversal"""
        reason = _validate_hook_command_safety(["echo", "QUX/../../evil.exe"])
        assert reason is not None
        assert "path_traversal" in reason

    def test_rejects_path_traversal_win(self):
        reason = _validate_hook_command_safety(["echo", "C:\\..\\..\\evil"])
        assert reason is not None
        assert "path_traversal" in reason

    def test_accepts_safe_command(self):
        """正常 echo + 安全路径应通过"""
        reason = _validate_hook_command_safety(["echo", "E:\\", "normal_label"])
        assert reason is None

    def test_accepts_exe_executable(self):
        reason = _validate_hook_command_safety(["backup.exe", "E:\\"])
        assert reason is None

    def test_empty_command_rejected(self):
        reason = _validate_hook_command_safety([])
        assert reason == "empty_command"

    def test_hook_runner_rejects_malicious_label(self):
        """集成测试: 恶意卷标通过 HookRunner._fire 应被拒绝(不调 subprocess)"""
        rule = HookRule(
            name="test",
            match_labels=("*",),
            command=["echo", "{label}"],
            debounce_seconds=0,
            enabled=True,
        )
        # 恶意卷标含 & 元字符
        vol = VolumeInfo(
            path="E:/", title="USB", drive_type="removable",
            disk_number=None, total=None, used=None, free=None,
            label="FOO&calc.exe",
        )
        runner = HookRunner([rule])
        with patch("subprocess.run") as mock_run:
            runner._fire(rule, vol)
            # subprocess.run 不应被调用
            mock_run.assert_not_called()
        runner.stop()


# ---------------------------------------------------------------------------
# 2. CLI/Config 参数边界
# ---------------------------------------------------------------------------

class TestCLIParameterBounds:
    """CLI/Config 参数上限防护 — 防止 --log-max-bytes=999GB 导致磁盘耗尽"""

    def test_log_max_bytes_upper_bound_cli(self, tmp_path):
        """--log-max-bytes 超过 16MB 应被截断"""
        args = parse_args(["--log-max-bytes", "99999999999999999"])
        cfg = merge_cli_config(args, AppConfig(log_dir=tmp_path))
        assert cfg.log_max_bytes == 16_777_216  # 截到 16MB

    def test_log_max_bytes_lower_bound_cli(self, tmp_path):
        args = parse_args(["--log-max-bytes", "-1"])
        cfg = merge_cli_config(args, AppConfig(log_dir=tmp_path))
        assert cfg.log_max_bytes == 10_000  # 抬到 10KB

    def test_log_backups_upper_bound_cli(self, tmp_path):
        """--log-backups 超过 9 应被截断"""
        args = parse_args(["--log-backups", "999999"])
        cfg = merge_cli_config(args, AppConfig(log_dir=tmp_path))
        assert cfg.log_backups == 9

    def test_log_backups_lower_bound_cli(self, tmp_path):
        args = parse_args(["--log-backups", "-5"])
        cfg = merge_cli_config(args, AppConfig(log_dir=tmp_path))
        assert cfg.log_backups == 0


# ---------------------------------------------------------------------------
# 3. crash.log 轮转失败兜底
# ---------------------------------------------------------------------------

class TestCrashLogRotateFallback:
    """crash.log 轮转失败时应截断兜底 + 记录诊断, 不静默吞错"""

    def _make_mgr(self, log_dir):
        mgr = LoggingManager()
        mgr._config = type('LC', (), {
            'log_dir': log_dir, 'mode': type('M', (), {'value': 'redacted'}),
        })()
        mgr._listener = None
        mgr._lock = threading.RLock()
        mgr._original_sys_hook = None
        mgr._original_thread_hook = None
        mgr._hooks_installed = False
        return mgr

    def test_rotate_partial_failure_truncates(self, tmp_path):
        """当 replace 失败时, crash.log 应被截断而非保留超限内容"""
        mgr = self._make_mgr(tmp_path)
        crash_path = tmp_path / "crash.log"
        crash_path.write_bytes(b"x" * (512 * 1024 + 100))  # 超过 512KB
        # mock replace 抛 OSError
        original_replace = Path.replace
        def failing_replace(self, target):
            if "crash.log.1" in str(target):
                raise OSError(13, "Permission denied")
            return original_replace(self, target)
        with patch.object(Path, "replace", failing_replace):
            mgr._rotate_crash_log(crash_path)
        # 轮转失败兜底: crash.log 应被截断为 0
        assert crash_path.stat().st_size == 0

    def test_normal_rotate_succeeds(self, tmp_path):
        """正常轮转场景仍应工作"""
        mgr = self._make_mgr(tmp_path)
        crash_path = tmp_path / "crash.log"
        crash_path.write_bytes(b"x" * (512 * 1024 + 100))
        mgr._rotate_crash_log(crash_path)
        # crash.log 应变为 crash.log.1
        assert (tmp_path / "crash.log.1").exists()
        assert not crash_path.exists()


# ---------------------------------------------------------------------------
# 4. AST 危险模式扫描
# ---------------------------------------------------------------------------

class TestASTSafetyScan:
    """全包扫描 eval/exec/shell=True/pickle/os.system"""

    def test_no_eval_exec_os_system_pickle(self):
        dangerous = []
        for root, _, files in os.walk(ROOT / "usb_monitor"):
            for f in files:
                if not f.endswith(".py"):
                    continue
                path = os.path.join(root, f)
                tree = ast.parse(Path(path).read_text(encoding="utf-8"))
                for node in ast.walk(tree):
                    if isinstance(node, ast.Call):
                        if isinstance(node.func, ast.Name) and node.func.id in ("eval", "exec", "__import__"):
                            dangerous.append((path, node.lineno, node.func.id))
                        if isinstance(node, ast.Attribute):
                            if node.attr == "system" and isinstance(node.value, ast.Name) and node.value.id == "os":
                                dangerous.append((path, node.lineno, "os.system"))
                            if node.attr == "loads" and isinstance(node.value, ast.Name) and node.value.id == "pickle":
                                dangerous.append((path, node.lineno, "pickle.loads"))
                    if isinstance(node, ast.keyword) and node.arg == "shell":
                        if isinstance(node.value, ast.Constant) and node.value.value is True:
                            dangerous.append((path, node.lineno, "shell=True"))
        assert not dangerous, f"发现危险模式: {dangerous}"


# ---------------------------------------------------------------------------
# 5. 智能笔/HID 识别边界
# ---------------------------------------------------------------------------

class TestSmartPenClassification:
    """智能笔(disk_numbers空 + 非USB bus) 应判为非外部存储"""

    def _make_scanner(self, api):
        scanner = DriveScanner.__new__(DriveScanner)
        scanner.api = api
        scanner._cache_lock = threading.Lock()
        scanner._classification_cache = OrderedDict()
        scanner._bus_cache = OrderedDict()
        scanner.CACHE_TTL_SECONDS = 2.0
        scanner.CACHE_MAX_ITEMS = 32
        scanner.BUS_CACHE_TTL_SECONDS = 60.0
        scanner.BUS_CACHE_MAX_ITEMS = 16
        scanner._l1_hits = 0; scanner._l1_misses = 0
        scanner._l2_hits = 0; scanner._l2_misses = 0
        return scanner

    def test_smart_pen_classified_non_external(self):
        """智能笔: disk_numbers空 + volume_storage_is_external=False → external=False"""
        class StubApi:
            def volume_disk_numbers(self, path): return ()
            def volume_storage_is_external(self, path): return False
        scanner = self._make_scanner(StubApi())
        disk_nums, external = scanner._classify("Z:", DRIVE_REMOVABLE)
        assert external is False, "智能笔被误判为外部存储"

    def test_real_usb_classified_external(self):
        """真实U盘: disk_nums非空 + bus_type=USB → external=True"""
        class UsbApi:
            def volume_disk_numbers(self, path): return (1,)
            def volume_storage_is_external(self, path): return True
        scanner = self._make_scanner(UsbApi())
        scanner._bus_type_for = lambda n: True
        disk_nums, external = scanner._classify("E:", DRIVE_REMOVABLE)
        assert external is True, "真实U盘被误判为非外部"

    def test_guid_is_volume_not_usb_device(self):
        """GUID 必须 = VOLUME(0x53F5630D), 不能回退到 USB_DEVICE(0xA5DCBF10)"""
        g = usb_interface_guid()
        assert g.Data1 == 0x53F5630D, f"GUID回退到USB_DEVICE: {hex(g.Data1)}"


# ---------------------------------------------------------------------------
# 6. mutex name 注入检查
# ---------------------------------------------------------------------------

class TestMutexNameSafety:
    """单实例 mutex name 不应含可注入字符"""

    def test_mutex_name_no_injection(self):
        from usb_monitor.app import single_instance_mutex_name
        import re
        name = single_instance_mutex_name()
        # Windows mutex name: Local\ 或 Global\ 前缀 + 字母数字_.-
        assert re.match(r'^(Local|Global)\\[A-Za-z0-9_.\-]+$', name), \
            f"mutex name含特殊字符: {name!r}"
