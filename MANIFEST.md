# USB Monitor — v1.0.0 交付清单

> 当前交付状态：已加入自动测试、Windows 构建、Tag 与 GitHub Release 发布流程。

## 📦 仓库文件

```
USBMonitor/
├── README.md                              # 项目说明（含 🔒 Security 章节）
├── CHANGELOG.md                           # 变更历史
├── MANIFEST.md                            # 本文件
├── pyproject.toml                         # version: 1.0.0
├── requirements-build.txt                 # 构建依赖
├── .gitignore
├── .github/workflows/release.yml         # 自动测试、构建、Tag 与 Release
├── USBMonitor.pyw                         # GUI / Nuitka 稳定入口
├── USBMonitor_console.py                  # 保留控制台的诊断入口
├── run_usb_monitor.bat                    # 双击启动：venv + 依赖 + 启动
├── usb_monitor/
│   ├── __init__.py                        # 公共 API re-export
│   ├── __main__.py                        # python -m usb_monitor 入口
│   ├── app.py                             # Win32 + PySide6 实现
│   ├── core.py                            # 纯函数 / 数据模型
│   └── hooks.py                           # 显式 shell=False / 三流 DEVNULL
├── tests/
│   ├── test_core.py                       # core 纯函数 + stable_fingerprint 文档守门
│   ├── test_bus_cache.py                  # L1/L2 缓存
│   ├── test_ux_rewrites.py                # S2 UX 行为
│   ├── test_gui_bugfixes.py              # GUI 兼容性
│   ├── test_hook_security.py              # AST 防回归（5 个 case）
│   ├── test_entrypoint_and_nuitka.py      # 入口 + 动态版本一致性
│   ├── test_release_automation.py         # 发布元数据 + workflow 守门
│   ├── test_audit_report_fixes.py
│   ├── test_markdown_report_20260630_fixes.py
│   ├── test_md_followup_fixes.py
│   └── test_tray_split_and_button_size.py
├── scripts/
│   ├── __init__.py
│   └── release_meta.py                    # 版本校验与 Release Notes 提取
├── build/
│   └── windows_nuitka.bat                 # onefile / standalone（含 CI 解释器覆盖）
├── docs/
│   ├── BUILD.md                           # Nuitka 打包指南
│   └── RELEASE.md                         # 自动发布指南
└── upx/
    ├── upx.exe                            # UPX 4.2.4 win64
    ├── LICENSE
    └── README.txt
```

## 🛡 变更分类

| 优先级 | 变更 | 影响文件 |
|---|---|---|
| **P0** | Hooks 权限边界文档 | `README.md` |
| **P0** | 显式 `shell=False` + SECURITY 块注释 | `usb_monitor/hooks.py` |
| **P0** | AST 防回归测试（5 个 case） | `tests/test_hook_security.py` |
| **P1** | `hash_id` → `stable_fingerprint` 重命名 + 修正 docstring | `core.py` / `__init__.py` / `app.py` / `test_core.py` |
| **P1** | `stable_fingerprint` 文档守门测试 | `tests/test_core.py` |
| **P1** | 补齐缺失入口文件 | `USBMonitor.pyw` / `USBMonitor_console.py` / `run_usb_monitor.bat` |
| **版本号** | 全仓 1.2.7 → 1.0.0 | `pyproject.toml` / `__init__.py` / `app.py` / `windows_nuitka.bat` / `BUILD.md` / `test_entrypoint_and_nuitka.py` |
| **构建** | `windows_nuitka_upx.bat` → `windows_nuitka.bat`（含 UPX 自动检测、`py -3.11` 优先、onefile/standalone 模式） | `build/windows_nuitka.bat` |
| **发布** | 默认分支自动校验、双平台测试、Nuitka 构建、Tag 与 Release | `.github/workflows/release.yml` / `scripts/release_meta.py` |
| **文档** | 仓库结构 / 测试计数 / 构建与发布命令同步 | `README.md` / `docs/BUILD.md` / `docs/RELEASE.md` |
| **清理** | 删除 `windows_nuitka_upx.bat`、`optimized-prompt.md`、`MARKDOWN_REPORT_20260630_IMPLEMENTATION.md` | — |

## 🧪 验证

```
$ QT_QPA_PLATFORM=offscreen python -m pytest -q
154 passed, 1 skipped
```

**AST 防回归验证**（用 `python -O` 关闭 assert，临时把 `shell=False` 改成 `shell=True`）：
仍然被 `test_hooks_subprocess_calls_explicitly_disable_shell` 抓到 — 因为 pytest 对 test module 做了
bytecode assertion rewriting，`-O` 不会剥离。
