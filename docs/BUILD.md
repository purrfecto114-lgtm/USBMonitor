# USB Monitor · Nuitka 打包指南

## 统一入口

源码、双击运行和 Nuitka 构建都进入同一业务函数：

- `USBMonitor.pyw`：GUI 与 Nuitka 的正式入口；
- `USBMonitor_console.py`：保留控制台，便于查看启动异常；
- `python -m usb_monitor`：模块入口；
- 安装项目后可使用 `usb-monitor` 命令。

入口只负责定位源码根目录并调用 `usb_monitor.app.main()`，不复制业务逻辑。

## 环境

在 Windows x64 上使用 Python 3.11：

```cmd
py -3.11 -m pip install -r requirements-build.txt
py -3.11 -m pytest -q
```

Qt 官方的 Nuitka 部署说明推荐直接编译应用入口；Nuitka 的 PySide6 插件负责收集 Qt 运行库和插件。项目因此使用 `--enable-plugin=pyside6`，不再强制包含整个 `PySide6` 包。

## 构建

### 单文件版

```cmd
build\windows_nuitka.bat onefile
```

产物：`dist\USBMonitor.exe`。

### 目录版

```cmd
build\windows_nuitka.bat standalone
```

目录版更适合定位 Qt 插件、DLL 或杀毒软件拦截问题；确认无误后再构建 onefile。

### 调试控制台

```cmd
set USBMONITOR_CONSOLE=1
set USBMONITOR_NO_UPX=1
build\windows_nuitka.bat standalone
```

### 追加 Nuitka 参数

```cmd
set NUITKA_EXTRA_ARGS=--show-progress --show-memory
build\windows_nuitka.bat onefile
```

## 已适配项目

- 构建目标固定为仓库根目录 `USBMonitor.pyw`；
- PySide6 Nuitka 插件；
- onefile 和 standalone 两种模式；
- GUI 默认禁用控制台，支持调试时强制控制台；
- 生成 `build\nuitka\nuitka-report.xml` 供依赖审计；
- PE 文件版本使用四段式 `1.0.0.0`；
- onefile 可选本地 UPX，目录版不执行 UPX；
- 可通过 `USBMONITOR_NO_UPX=1` 禁用 UPX；
- 可通过 `NUITKA_EXTRA_ARGS` 注入额外参数；
- CI 可通过 `USBMONITOR_PYTHON=python` 固定使用当前 Python 解释器；
- 构建前检查 Python 3.11、PySide6、pywin32 和 Nuitka；
- 构建后检查目标 EXE 是否实际存在。

## 建议验证顺序

```cmd
py -3.11 USBMonitor_console.py --help
py -3.11 USBMonitor_console.py --startup-status
build\windows_nuitka.bat standalone
dist\USBMonitor.dist\USBMonitor.exe --startup-status
build\windows_nuitka.bat onefile
dist\USBMonitor.exe --startup-status
```

`--startup-status` 不进入完整 GUI 事件循环，适合作为打包后的第一层启动烟测。随后再验证托盘、插拔、安全弹出和开机启动。

## 常见问题

### Qt platform plugin 无法初始化

先关闭 UPX并构建 standalone：

```cmd
set USBMONITOR_NO_UPX=1
set USBMONITOR_CONSOLE=1
build\windows_nuitka.bat standalone
```

检查 `USBMonitor.dist` 中是否存在 Qt `platforms` 插件，并查看控制台错误及 Nuitka report。

### 杀毒软件拦截 onefile

UPX 和 onefile 自解压都可能提高误报概率。优先发布未压缩版本、进行代码签名，并保留 standalone 构建作为诊断产物。

### 构建成功但开机启动失败

程序已针对 Nuitka 编译环境和 onefile 父进程路径做处理。请先运行：

```cmd
dist\USBMonitor.exe --install-startup
dist\USBMonitor.exe --startup-status
```

确认 `healthy`、`target_exists` 和 `source_current`。
