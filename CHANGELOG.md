# Changelog

All notable changes to this project are documented in this file.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [1.1.0] — 2026-08-22

### Fixed — 8 项恶性bug修复

- **智能笔/HID设备误识别为未分配U盘**（bug1）。
  `usb_interface_guid()` 从 `GUID_DEVINTERFACE_USB_DEVICE`（覆盖所有USB含HID）
  改为 `GUID_DEVINTERFACE_VOLUME`（仅卷接口）。`DriveScanner._classify` 在
  `volume_disk_numbers` 返回空时，不再退化为"DRIVE_REMOVABLE 且非系统盘即外部"
  的弱启发，改为对卷句柄直接做 `IOCTL_STORAGE_QUERY_PROPERTY` 查询 `bus_type`
  / `removable_media`。新增 `WindowsStorageApi.volume_storage_is_external()`。
  新增诊断日志 `removable_drive_classified_non_external`（defense-in-depth）。
- **Toast动画卡顿**（bug2）。`ToastWindow.refresh()` 实现行复用缓存
  （按盘符key diff，仅增删变化行），不再每次全量销毁重建VolumeRow；
  移除冗余 `adjustSize()`；`_RESTORE_DELAYS_MS` 从 `(0,50,250)` 压缩为
  `(0,200)`，减少1/3重定位开销。
- **PySide6通知莫名降级为纯toast**（bug3）。`merge_cli_config` 移除
  `--startup/--silent` 强制 `gui_backend="tray-only"` 的覆盖。后端始终
  尊重用户 config / `--gui-backend`。`--silent` 取消 `SUPPRESS` 隐藏并
  文档化为"开机启动兼容标记（不再降级通知通道）"。
- **托盘右键菜单"最近操作"过长超出屏幕**（bug4）。`refresh_volume_menu`
  最近记录分页：前5条直接展示，其余收入"更多（N 项）"子菜单，
  避免主菜单30+项超出屏幕高度（Qt `menu-scrollable` 样式不可靠的业界共识）。
- **托盘菜单难以点击**（bug5）。`_on_tray_activated` 抑制从200ms降到50ms
  （不拒绝正常连击）；`_tray_popup_position` 改用 `availableGeometry().bottom()`
  让 Qt 自动向上展开菜单，避免与任务栏重叠。
- **日志清理功能失效**（bug6）。`LoggingManager.stop()` 显式 `join` listener
  线程（timeout=2s）释放Windows文件句柄；`reset_files` 的 `unlink` 失败时
  回退截断（`write_bytes(b"")`）而非静默 `continue`。
- **日志容易过大**（bug7）。`write_crash` 加手动轮转
  （`CRASH_LOG_MAX_BYTES=512KB`×`CRASH_LOG_BACKUPS=2`）；`reset_files`
  默认 `include_crash=True`（原False导致crash.log越界增长）；
  日志默认 `256KB×3`（原`1MB×5`，3类合计从15MB+降到≈2.3MB）。
- **程序体积过大**（bug8）。`safe_eject_drive` 主路径改用
  `IOCTL_STORAGE_EJECT_MEDIA`（纯ctypes，无需pywin32），Shell.Application
  降为惰性回退；Nuitka构建脚本新增 `--nofollow-import-to` 排除30+未用Qt模块
  （QtNetwork/QtSql/QtQml/QtQuick/QtWebEngine等）+ `--no-docstrings` + `--lto=yes`。

### Added — 合理拓展
- `tests/test_stage2_bugfixes.py` — 20个TDD回归测试，覆盖8个bug的
  红-绿循环验证（原版19失败/修复版20通过）。
- bug1 诊断日志层：DRIVE_REMOVABLE 被判为非外部存储时记录
  `removable_drive_classified_non_external`，帮助用户理解智能笔为何不显示。

### Architecture — 辩证结论
- **不引入Rust/C++**。三方交叉验证（根因分析/业界对比/YAGNI检查）一致：
  性能瓶颈在Qt widget重建（业务层Python）非IOCTL调用；同类项目
  Eric-Canas/USBMonitor纯Python跨三平台运行良好；Nuitka已自动C转译。
  走"纯Python + ctypes替代pywin32 + Nuitka显式排除未用Qt模块"路线。

## [1.0.0] — 2026-07-17

### Security
- **Hooks trust boundary documented.** README now ships a dedicated
  `🔒 Security` section describing that `config.json` is an implicit trust
  point, hooks run with the full current-user token, and the feature is
  opt-in with an empty default `hooks` array.
- **`shell=False` enforced at AST level.** Every `subprocess.*` call in
  `usb_monitor/hooks.py` now explicitly passes `shell=False` as a keyword
  literal, with `stdin/stdout/stderr=DEVNULL` and a 60s timeout. A new
  regression test (`tests/test_hook_security.py`) parses the module with
  `ast` and fails if any call site forgets the guard.
- **`hash_id` renamed to `stable_fingerprint`.** The old name implied
  cryptographic hashing; the new name matches what the function actually
  is (a plain truncated SHA-256 with no salt). The docstring now states
  explicitly that it is **not** encryption, **not** anonymization, and
  **not** safe for secrets.

### Added
- `USBMonitor.pyw` — GUI entry point for double-click and Nuitka builds.
- `USBMonitor_console.py` — diagnostic entry point that keeps the console.
- `run_usb_monitor.bat` — user-facing launcher that creates a `.venv`,
  installs runtime deps, and starts the app without a console window.
- `tests/test_hook_security.py` — 5 AST-based regression tests guarding
  the hook command-execution contract.
- `docs/BUILD.md` — Nuitka packaging guide covering onefile, standalone,
  debug-console, and common Qt/AV-false-positive pitfalls.
- `pyproject.toml` — editable-install metadata and `usb-monitor` console
  script.
- `MANIFEST.md` — per-file change manifest for this release.
- `.github/workflows/release.yml` — validates versions and changelog, runs Linux/Windows tests, builds the Windows x64 Nuitka executable, generates SHA-256 checksums, creates the `vX.Y.Z` tag, and publishes the GitHub Release.
- `scripts/release_meta.py` — standard-library-only release metadata validator and changelog-section extractor.
- `docs/RELEASE.md` — maintainer guide for the automated release flow.
- `tests/test_release_automation.py` — regression tests for release metadata and workflow safety guards.

### Changed
- Version bumped from 1.2.7 to 1.0.0 (project reset; semantic versioning
  from here on).
- `build/windows_nuitka.bat` replaces the legacy
  `build/windows_nuitka_upx.bat`.  The new script:
  - targets `USBMonitor.pyw` instead of `usb_monitor/__main__.py`;
  - accepts `onefile` / `standalone` as an explicit argument;
  - auto-detects the bundled `upx/upx.exe` and passes it to Nuitka via
    `--upx-binary` (override with `USBMONITOR_NO_UPX=1`);
  - prefers `py -3.11` and falls back to `python` on PATH;
  - honours `USBMONITOR_CONSOLE=1` to force a console for debugging;
  - writes `build/nuitka/nuitka-report.xml` for dependency audits.
- `CONFIG_VERSION` bumped from 2 to 3 (new `hooks` field).
- `usb_monitor/__init__.py` re-exports `stable_fingerprint` and the new
  hooks API surface.
- Version consistency tests now derive the expected version from `pyproject.toml` instead of hard-coding `1.0.0`, so future releases only need to update the actual version declarations.
- `build/windows_nuitka.bat` accepts `USBMONITOR_PYTHON` so CI can pin the interpreter supplied by `actions/setup-python`; automated releases disable UPX to reduce antivirus false positives.

### Removed
- `build/windows_nuitka_upx.bat` — superseded by `windows_nuitka.bat`.
- `docs/MARKDOWN_REPORT_20260630_IMPLEMENTATION.md` — internal diagnostic,
  no longer referenced.
- `optimized-prompt.md` — scratch file, not part of the shipped project.

### Tests
- `tests/test_core.py` — `hash_id` tests renamed to `stable_fingerprint`;
  added `test_stable_fingerprint_is_documented_as_not_cryptographic` to
  guard the security docstring against regression.
- `tests/test_entrypoint_and_nuitka.py` — version assertions updated to
  `1.0.0`; new test verifies `USBMonitor.pyw` is syntax-valid and calls
  `usb_monitor.app.main`.
- Local verification after adding release automation: `154 passed, 1 skipped`; the skipped case is the real-PySide6 subprocess smoke test because PySide6 is not installed in the Linux sandbox.

---

Older history predates the 1.0.0 reset and is not tracked here.
