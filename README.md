# USBMonitor 2.6.0

原生 C99 外接存储监控。Windows 提供托盘与设备概览，Linux 提供命令行和可选的 X11 通知。守护进程合并短时间内的设备事件，并按间隔复核设备清单。

诊断记录包含基线结果、技术取舍、已测范围和真机验收项。[诊断与验证](docs/DIAGNOSTIC.md) · [界面预览](docs/USBMonitor-preview.png)

## 开始使用

| 平台 | 入口 | 操作 |
| --- | --- | --- |
| Windows x64 | 双击 `bin/windows-amd64/usbmon.exe` | 托盘左键查看卷和设备概览，右键重新扫描、勾选“显示其他设备”、打开日志与配置目录、管理自启动或退出。 |
| Linux x86-64 | `./bin/linux-amd64/usbmon` | 直接启动静态守护进程。有 X11 和 Xft 时，同目录下的 `usbmon-toast` 显示通知。 |

在 Windows 终端中将下列命令里的 `./usbmon` 换成 `./usbmon.exe`。Windows 可执行文件使用 GUI 子系统，自动化脚本应显式等待进程退出，例如 PowerShell 的 `Start-Process -Wait -PassThru`。

```sh
./usbmon --list
./usbmon --once --no-hooks
./usbmon --no-gui --interval 3600
./usbmon --help
```

- `--list` 仅读取当前存储清单。枚举失败时退出码为 1。
- `--once` 执行一轮并保留快照。与守护进程共用状态锁，占用时退出码为 3。
- `--no-gui` 关闭界面与通知，设备事件仍可触发扫描。`--no-hotpath` 关闭自动事件唤醒。
- `--baseline` 忽略历史状态，重新记录当前在场设备并执行匹配的 Hooks。
- `--install-startup`、`--startup-status`、`--uninstall-startup` 管理当前用户的登录自启动。Windows 使用 HKCU Run，Linux 使用 XDG Autostart。

## 设备概览

Windows 盘符、卷和磁盘分开计数。同一磁盘的多个盘符聚合成一行，容量条对应首个可访问的挂载点。无盘符、目录挂载、不可访问的卷和确认无介质的读卡器会显示各自状态。USB HID、网卡和集线器不进入通知，仅在概览中展示且默认隐藏。托盘右键菜单的“显示其他设备”可勾选显示并记住选择；面板底部不再重复放置该开关，只保留展开/收起与打开U盘。展开列表后可以滚动，方向键能滚动到所选设备，Tab 切换可用控件，Enter 执行动作，Esc 隐藏面板。

右键设备行可以打开卷、定位或复制路径。符合条件的 USB 存储还可以请求 Windows 安全移除物理设备。对于复合设备或缺少可移除能力证据的设备，请使用 Windows 系统托盘的移除界面。卷可访问性取决于当前用户和驱动状态。

Linux 通知适用于 X11 或提供 XWayland 的桌面。随包 X11 助手需要 glibc 2.34 或更新版本、libX11 和 libXft。容量、分区数和挂载点取自扫描时刻。挂载管理器在扫描之后才完成挂载时，首次事件可能还没有路径。

## 日志与 Hooks

| 内容 | Windows | Linux |
| --- | --- | --- |
| 日志、快照与锁 | `%LOCALAPPDATA%\usbmon` | `${XDG_STATE_HOME:-$HOME/.local/state}/usbmon` |
| Hooks | `%APPDATA%\usbmon\hooks.json` | `${XDG_CONFIG_HOME:-$HOME/.config}/usbmon/hooks.json` |

`--log FILE` 和 `--hooks FILE` 可以指定路径。JSONL 事件包括 `start`、`round`、`add`、`remove`、`error` 和 `stop`。每个 `round` 的 `wake` 标记为 `start`、`hot` 或 `tick`。`scan_ms` 记录枚举耗时。日志达到 1 MiB 后轮转，保留三份历史文件。默认序列字段为截断 SHA-256 指纹，可用于关联设备。`--log-raw` 输出原序列号。

Hooks 默认没有配置。首次运行时会在配置目录生成 `hooks.example.json`；编辑其中的命令后改名为 `hooks.json`，再把 `"enabled"` 设为 `true` 即可启用。下面是 Windows 示例。

```json
{
  "hooks": [
    {
      "name": "backup",
      "match_keys": ["disk*"],
      "match_models": ["SanDisk*"],
      "command": ["C:\\Tools\\backup.exe", "--source", "{path}"],
      "debounce_seconds": 3,
      "enabled": true
    }
  ]
}
```

- `command` 是 argv 数组。可执行文件必须是字面路径，Windows 还要求绝对 `.exe` 路径。参数可使用 `{key}`、`{model}`、`{path}`。
- `{path}` 优先取当前可访问的挂载点。未挂载时退回设备提示，Linux 通常为 `/dev/sdX`，Windows 可能为盘符或 `diskN`。
- Hooks 以当前用户权限执行，子进程超时预算为 60 秒。`--no-hooks` 禁用。Linux 可用 SIGHUP 重载，Windows 重启后读取。

## 从源码构建

```sh
make strict selftest integration-test
make analyze
make static
make CROSS=x86_64-w64-mingw32- windows
```

Linux GUI 构建需要 `pkg-config`、`libx11-dev` 和 `libxft-dev`。静态守护进程需要 `musl-tools`。Windows 交叉构建需要 MinGW-w64 的 GCC 和 windres。Linux 图形回归 `bash tools/demo-gui.sh` 需要 Xvfb 和 xwininfo。Windows 原生冒烟脚本是 `tools/demo.ps1`。

`src/main.c` 负责调度，`src/state.c` 负责快照持久化。Windows 的 `um_enum.c` 采集设备证据，`scan_win32.c` 将同一批证据投影到存储事件和 UI。Linux 的 `scan_linux.c` 读取 sysfs，`hotpath.c` 接收设备变化。原生窗口、托盘动作与 UI 布局分别位于 `gui_win32.c`、`tray_win32.c` 和 `um_toast_ui.c`。

发行包内 `bin/SHA256SUMS.txt` 对应已构建二进制。可在 `bin` 目录执行 `sha256sum -c SHA256SUMS.txt` 检查完整性。
