# USB Monitor

> Windows 端 USB 存储设备插拔通知工具 —— Toast 弹窗 + 系统托盘。

一个为 Windows 设计的轻量级 USB 存储设备监听器，UI 用 PySide6，后台是
单线程串行化的扫描协调器，事件从 Win32 `WM_DEVICECHANGE` 一路走到 Qt 信号，
中间不抖、不漏、不抢主线程。

## ✨ 特性

- **真实硬件事件**：监听 `WM_DEVICECHANGE` + IOCTL，不轮询、不猜。
- **两层缓存**：`DriveScanner` 用 L1(path→disk) + L2(disk_number→bus_type) 两级
  LRU，burst 事件下 IOCTL 调用降 50%+。
- **Toast 稳定版 + 倒计时**：Windows 下去掉分层透明、圆角 mask 和阴影动画，避免 `UpdateLayeredWindowIndirect` 控制台报错；"N 秒后自动关闭"实时倒计时，悬停暂停，Esc 关闭；折叠时只显示“打开U盘”，展开时按分区/盘符逐行显示并可单独打开，展开列表支持触屏滑动滚动；托盘重新扫描只轮询状态，不再改写 threading.Event；托盘左键直接打开“USB 设备”菜单，右键只保留精简设置/工具菜单。
- **异步安全弹出**：`QThread` worker 跑 `safe_eject_drive`，主线程不卡 1~10s，
  Toast 上有"正在安全弹出 E:\"实时状态。
- **Nuitka onefile + UPX**：单文件部署，UPX 压缩 50~70% 体积，HKCU Run 启动项自愈。
- **零外部依赖测试**：`core.py` 不依赖 Qt/Win32，Linux 上静态测试与真实 Qt 离屏启动测试均可执行。

## 📁 仓库结构

```
usb-monitor/
├── USBMonitor.pyw              # GUI/Nuitka 稳定入口
├── USBMonitor_console.py       # 保留控制台的诊断入口
├── pyproject.toml              # 可编辑安装与 usb-monitor 命令
├── usb_monitor/                # Python 包
│   ├── __init__.py             # 公共 API re-export
│   ├── __main__.py             # python -m usb_monitor 入口
│   ├── app.py                  # Win32 + PySide6 实现（约 3300 行）
│   └── core.py                 # 纯函数 / 数据模型（可独立测试）
├── tests/                      # pytest 测试集（139 cases）
│   ├── test_core.py            # core 纯函数
│   ├── test_bus_cache.py       # L1/L2 缓存
│   ├── test_ux_rewrites.py     # S2 UX 行为
│   └── test_gui_bugfixes.py    # 旧版 GUI 兼容性（2）
├── upx/                        # 内置 UPX 4.2.4 win64（可重放构建）
│   ├── upx.exe
│   ├── LICENSE
│   └── README.txt
├── build/                      # Windows 打包脚本
│   └── windows_nuitka_upx.bat
├── docs/                       # 审查与改进文档
│   ├── ANALYSIS.md             # 静态分析报告
│   └── IMPROVEMENTS.md         # 可落地 Patch
├── CHANGELOG.md                # 变更历史
├── pytest.ini
├── .gitignore
└── README.md
```

## 🚀 快速开始

Windows 源码包可直接双击 `run_usb_monitor.bat`。脚本会检查并安装运行依赖，然后启动程序。

### 开发方式

```bash
# 1. 准备环境（Windows）
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install PySide6 pywin32 pytest

# 2. 运行（GUI 模式）
python -m usb_monitor
# 或：python USBMonitor_console.py

# 3. 跑测试（Linux/Windows 都行）
python -m pytest -q
# → 141 passed

# 4. CLI 模式（不开 GUI，10s 后退出）
python -m usb_monitor --no-gui

# 5. 启动项管理
python -m usb_monitor --install-startup
python -m usb_monitor --uninstall-startup
python -m usb_monitor --startup-status
```

## ⚡ 自动化规则（可选）

规则保存在 `%LOCALAPPDATA%\USBMonitor\config.json` 的 `hooks` 数组中。每条规则
只在 USB 加入事件触发，命令必须使用参数数组，不经过 shell；支持 `{path}` 与
`{label}` 两个占位符。示例：

```json
{
  "hooks": [
    {
      "name": "auto-backup",
      "match_paths": [],
      "match_labels": ["BACKUP*"],
      "command": ["powershell", "-NoProfile", "-File", "C:/scripts/backup.ps1", "{path}"],
      "debounce_seconds": 3,
      "enabled": true
    }
  ]
}
```

无名称、无命令、重复名称或未知占位符的规则会被忽略，不会阻止主程序启动。
托盘右键菜单会显示当前启用的规则数量。

## 🏗 打包（Windows · Nuitka + UPX）

```cmd
:: 一行命令构建 single-file 可执行
py -3.11 -m pip install nuitka zstandard ordered-set
build\windows_nuitka.bat onefile
```

产物在 `dist\USBMonitor.exe`，用内置 `upx\upx.exe` 压缩。

详见 [`build/windows_nuitka.bat`](build/windows_nuitka.bat) 和
[`docs/BUILD.md`](docs/BUILD.md)（如果存在）。

## 🧱 架构

```
┌──────────────────────────────────────────────────────────────────┐
│ Win32 (kernel32/user32)                                          │
│   ├─ DeviceWindowThread    监听 WM_DEVICECHANGE                 │
│   └─ WindowsStorageApi     IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS  │
└─────────────────────┬────────────────────────────────────────────┘
                      │ RawDeviceChange
                      ▼
┌──────────────────────────────────────────────────────────────────┐
│ DriveReconciler (单线程串行)                                     │
│   ├─ DriveScanner (L1 path / L2 disk-number cache)               │
│   ├─ VolumeState (immutable snapshot)                            │
│   └─ debounce: immediate/short/settle 三档                       │
└─────────────────────┬────────────────────────────────────────────┘
                      │ UsbEvent (dataclass, frozen)
                      ▼
┌──────────────────────────────────────────────────────────────────┐
│ Qt Main Thread (PySide6)                                         │
│   ├─ EventBridge            Qt.QueuedConnection signal bridge    │
│   ├─ ToastWindow            直角稳定 Toast，倒计时，Esc / 打开U盘  │
│   ├─ VolumeRow              整行可点，进度条 hover 3 行 tooltip  │
│   ├─ TrayMenuController     左键设备菜单 / 右键精简设置菜单     │
│   └─ SafeEjectWorker (QThread)  异步安全弹出，Toast 状态行       │
└──────────────────────────────────────────────────────────────────┘
```

## 🛡 平台限制

- 主程序仅支持 Windows（Win32 + PySide6）
- 测试可在 Linux / macOS 上跑（`core.py` 是纯函数，`app.py` 在非 Windows 上
  走 fallback 路径，`ToastWindow` 走 stub 路径）

## 📜 许可证

仓库内嵌的 `upx/upx.exe` 是 UPX 4.2.4 win64，许可证为 GPL-2.0-or-later
带特殊例外条款（允许压缩任意二进制，包括商业软件）。详见
[`upx/LICENSE`](upx/LICENSE) 和 [`upx/README.txt`](upx/README.txt)。

主应用本身的许可证见仓库根目录（如未提供则默认私有）。
