# usbmon — 原生 USB 存储监控守护进程（C99）

> **命名消歧**：本项目与 Linux 内核自带的 `usbmon`（`CONFIG_USB_MON`，
> debugfs 下的 USB **数据包**抓包设施，docs.kernel.org/usb/usbmon.html）**同名但无关**。
> 本项目是用户态**存储设备**插拔监控；内核 usbmon 是总线级**报文**嗅探器，
> 无用户态可执行文件，安装路径不冲突。如需抓 USB 报文请用内核 usbmon + trace-cmd。

对原 `USBMonitor`（Python + PySide6，4,159 行单体 app.py）的**重置版**：
无托盘、无网页、守护进程本身零运行时依赖。默认**每 1 小时一轮**扫描（`1h 一轮`，
保底节奏）；插拔事件通过内核事件（inotify / WM_DEVICECHANGE）**毫秒级唤醒**，
插入 U 盘时右下角弹出设备信息窗（`usbmon-toast`），事件写入 JSONL 日志，
可选 hooks 在插入时以 argv 数组执行外部命令。

```
KEY      BUS       MODEL                       SIZE  MOUNT
sdb      usb       SanDisk Cruzer Glide 3.0   28.2 GB  /media/user/CRUZER
```

## 为什么重写（与 Python 版的实测对比）

沙箱实测（gcc -O2，Python 3.12）：

| 指标 | Python 原版（推算/引用） | usbmon (C) 实测 | 差距 |
|---|---|---|---|
| 进程启动 | 58 ms（仅解释器+非 GUI import）；PySide6 GUI 版 0.5–3 s（社区实测，Vorta 托盘常驻 215–220MB） | **0.56 ms** | ~100× |
| 常驻内存 | 17.9 MB（仅非 GUI import）；PySide6 托盘 41–220 MB | **3.1 MB**（1 线程） | 5.8–70× |
| 产物体积 | Nuitka onefile + UPX ≈ 10–30 MB，另带 venv/启动 bat/自愈复制 | **52 KB**（另有 23 KB 弹窗助手，可选） | ~200–600× |
| 运行时依赖 | Python ≥3.11、PySide6、pywin32 | **无** | — |
| 代码规模 | 4,959 行 + 18 个测试文件 + Nuitka/UPX 构建链 | ~3,800 行 C（含跨平台两套扫描器） | — |

诚实结论：原版"运行时吞吐性能"其实不是问题（USB 事件低频、IOCTL 是毫秒级 IO，
ctypes 的 µs 级 FFI 开销可忽略）。真正的问题是**内存驻留、启动延迟、部署复杂度**——
这恰是常驻小工具最要命的三个指标。原生语言（C/Rust/Go/Zig）是这类工具的正确选择。

## 构建

```bash
make            # 守护进程 + usbmon-toast 弹窗助手（需 X11/Xft 开发文件）
make strict     # -Wall -Wextra -pedantic -Werror 全绿
```

守护进程**永不链接 X11**：弹窗由独立助手二进制 `usbmon-toast` 渲染，
每事件 fork+exec（纯 argv，不经 shell）。无 X11 开发文件时助手不构建，
守护进程照常运行（纯无头模式）；无显示器的服务器上也完全不受影响。

Windows（MSVC 或 mingw-w64；本仓库无 Windows 编译产物，代码按 Win32 API 编写）：

```bash
x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra \
    src/main.c src/util.c src/logjson.c src/json.c src/hook.c src/lock.c \
    src/gui.c src/hotpath.c src/scan_win32.c src/gui_win32.c \
    -o usbmon.exe -luser32 -lgdi32 -lshell32
```

Windows 上弹窗与 WM_DEVICECHANGE 热路径由进程内 GUI 线程实现
（`gui_win32.c`，user32/gdi32 随系统存在，无需助手二进制）。

## 用法

```bash
usbmon                     # 守护进程：立即一轮，之后每 3600s 一轮（保底 1h），
                           # 插拔即时唤醒 + 弹窗（有显示器时自动启用 GUI）
usbmon --interval 300      # 自定义轮询间隔（秒）
usbmon --once              # 单轮退出（cron / 任务计划程序 模式，跨运行去重）
usbmon --list              # 列出当前外部存储设备（只读：不弹窗、不触发 hooks）
usbmon --gui / --no-gui    # 强制开/关弹窗（守护模式默认自动；--once 默认关）
usbmon --toast-secs 30     # 弹窗自动消失秒数（默认 12）
usbmon --no-hotpath        # 忽略插拔唤醒，严格按间隔轮询
usbmon --baseline          # 首轮把在位设备全部报为 add（默认状态文件去重）
usbmon --verbose           # 事件同时镜像到 stderr
usbmon --log FILE          # JSONL 日志（默认 ~/.local/state/usbmon/events.jsonl）
usbmon --log-raw           # 序列号明文入日志/弹窗（默认为 sha256 指纹）
```

守护进程持久化最近快照（`last-snapshot.txt`）并在每轮后更新，因此
**重启不会重发 add / 重触发 hooks**；`--once` 也复用同一状态文件，
每小时一次的 cron 同样能正确产生 add/remove 事件。首轮的在位设备
标记为 `"baseline":1` 且**不弹窗**（登录瞬间不该被一堆窗口轰炸）。

### 弹窗（保留原版"插入 U 盘即弹窗"体验）

```
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
┃ USB 设备已插入                     ┃
┃ 金士顿 DataTraveler 3.0 (sdc)     ┃
┃ 容量 62.7 GB · 2 个分区            ┃
┃ 挂载点 /media/usb-sdc1            ┃
┃ 序列 sha256:60a44c4c12ab           ┃
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
```

- 右下角堆叠（4 槽位），绿色边条=插入、灰色=拔出；点击或超时自动消失
- 中文设备名正常渲染：运行时逐字体探测 CJK 字形（fontconfig 在精简
  系统上会把 `sans:lang=zh` 解析到无中文字形的字体，不能信它）
- 序列号默认显示指纹，与日志一致（`--log-raw` 才显明文）

## 事件日志（JSONL）

```json
{"ts":"2026-09-01T12:00:00Z","ev":"add","key":"sdb","bus":"usb","model":"SanDisk Cruzer Glide 3.0","serial":"sha256:2239aa9f1386","size_bytes":30262169600,"partitions":["sdb1"],"mount":"/media/usb0","fs_total_bytes":30256263168,"fs_free_bytes":10758389760}
{"ts":"2026-09-01T12:00:01Z","ev":"add","baseline":1,"key":"sdc","bus":"usb","model":"金士顿 DataTraveler 3.0", ...}
{"ts":"2026-09-01T12:00:02Z","ev":"remove","key":"sdb"}
{"ts":"2026-09-01T13:00:00Z","ev":"round","n":12,"external":1,"interval_s":3600,"wake":"tick","scan_ms":0.3}
```

- `serial`：默认是截断 SHA-256（12 hex，用于跨日志关联同一设备）。
  **不是加密、不是匿名化**，与原版语义一致；`--log-raw` 才记录明文。
- `round.wake`：`start`（启动首轮）/ `hot`（插拔事件唤醒）/ `tick`（间隔到期）
  —— 热路径效果可直接从日志审计。
- `baseline:1`：首次见到该设备（无历史状态）而非插拔产生的 add。
- 日志 1MB 自动轮转，保留 3 份（`.1`/`.2`/`.3`）。

## Hooks（可选，默认无）

配置 `~/.config/usbmon/hooks.json`（Windows: `%APPDATA%\usbmon\hooks.json`）：

```json
{
  "hooks": [
    {
      "name": "auto-backup",
      "match_keys": ["sd*"],
      "match_models": ["SanDisk*"],
      "command": ["/usr/bin/rsync", "-a", "{path}/", "/backup/"],
      "debounce_seconds": 3,
      "enabled": true
    }
  ]
}
```

- 占位符：`{key}`（设备名）、`{model}`、`{path}`（挂载点，未挂载时为 `/dev/sdX`）
- **命令永远是 argv 数组，不经过任何 shell**（POSIX `execv` / Win32 `CreateProcessW`）
- Windows 上保留原版的 BatBadBut（CVE-2024-24576）与 CmdHijack 防护：
  拒绝 `.bat/.cmd/.ps1` 作为 executable、拒绝 cmd 元字符 `&|<>()%^` 与 `../`
- 子进程 stdio → `/dev/null`；超过 60s 由 reaper SIGKILL
- 守护进程 `SIGHUP` 重载 hooks 配置
- **Hooks 不是沙箱**：子进程拥有当前用户全部权限，勿用不可信配置文件

## 架构

```
┌ Linux: inotify 监视 /sys/block（插拔即醒，空闲零 CPU）          ┐
└ Win32: GUI 线程隐身窗口收 WM_DEVICECHANGE                        ┘
                        │ 醒后 0.7s 防抖（等 udev 挂载落定）
                        ▼
┌ Linux: /sys/block + /sys/block/*/device(usb?) + /proc/mounts ┐
└ Win32: GetLogicalDrives → IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS ┘
                        │ 每轮全量快照（µs~ms 级，无需 LRU 缓存）
                        ▼
              快照 diff → add / remove 事件
                        │
        ┌───────────┬──────────┬────────────────┐
        ▼           ▼          ▼                ▼
   JSONL 日志   hooks(argv exec)  usbmon-toast 弹窗  --list 表格
                                  (fork+exec, 独立进程)
```

刻意**不做**的事（原版过度工程的部分）：
- 无 L1/L2 LRU+TTL 缓存——每小时一轮的扫描本身只要亚毫秒
- 无三层 debounce——一个防抖窗口 + hooks 侧每 (规则,设备) 时间戳即可
- 无启动项自愈/源码包复制/清单签名——静态单文件没有"部署状态"可自愈
- 无托盘/常驻主窗口/SVG/动画——只在事件发生时弹一个轻量通知窗
- 守护进程本体不链接 X11/GUI 库——弹窗隔离在助手进程里，崩了也不影响监控

## 目录

```
usbmon/
├── Makefile
├── README.md
├── CHANGELOG.md
├── LICENSE              MIT（与 1.x Python 版同源授权）
├── src/
│   ├── usbmon.h       公共类型与契约
│   ├── main.c         CLI、快照 diff、轮循环、热路径接线、状态持久化
│   ├── scan_linux.c   sysfs 扫描器（支持 --sys-root 指向假 /sys 测试）
│   ├── scan_win32.c   Win32 扫描器（与 Python 原版同款 IOCTL 策略）
│   ├── hotpath.c      inotify 热路径（POSIX；Windows 用 WM_DEVICECHANGE）
│   ├── gui.c          GUI 管理器：助手发现、fork+exec、子进程回收
│   ├── gui_toast.c    弹窗助手：Xlib+Xft 渲染、CJK 字体探测、自动消失
│   ├── gui_win32.c    Windows GUI 线程：Win32 弹窗 + WM_DEVICECHANGE
│   ├── hook.c         hooks 解析/调度/安全过滤/子进程 reaper
│   ├── json.c         迷你递归下降 JSON 解析器（深度/节点数封顶）
│   ├── logjson.c      JSONL 日志 + 轮转
│   ├── lock.c         单实例（flock / 命名互斥体）
│   └── util.c         时间、glob、SHA-256 指纹、目录助手
└── tools/  demo.sh + demo-gui.sh   端到端演示与 GUI 回归测试
```

## 已验证行为（沙箱实测）

- `--list` / `--once` / 守护模式 / SIGTERM 干净退出
- 跨 `--once` 与跨守护重启的 add/remove 事件（状态文件；重启零重发）
- hooks：正常触发、glob 匹配、占位符替换、未知占位符拒绝、
  损坏 JSON 降级、exec 失败(127)与超时(60s)被记录/击杀
- SHA-256 指纹与 `python3 hashlib` 完全一致
- JSONL 每行均为合法 JSON；单实例锁（第二实例退出码 3）
- **GUI（Xvfb 下实测，`tools/demo-gui.sh` 11/11 通过）**：插入→约 1s 内
  弹窗（inotify 热路径 + 0.7s 防抖）、中文设备名正常渲染（VLM 图像
  识别验证）、拔出灰窗、4 槽堆叠、点击/超时消失、零僵尸（子进程回收
  表）、`--no-hotpath` 严格间隔、无 DISPLAY 自动降级无头、`--once --gui`
  cron 场景
- 编译：`-Wall -Wextra -pedantic -Werror` 零警告（含 Xft 助手）
- ASan/UBSan：守护进程与 `--once` 全场景零错误零泄漏；弹窗助手仅剩
  libfontconfig 内部 320B 进程级缓存（上游行为，非本代码）
- `gcc -fanalyzer`：唯一告警为 hook.c argv 循环的已知误报

Windows 路径（`scan_win32.c` / `gui_win32.c`）在无 mingw 的本沙箱中未编译，
上线前请在 Windows 上 `make windows` 或用 MSVC 构建并跑一次 `--list` 冒烟
+ 插拔弹窗检查（[WINDOWS-UNVERIFIED]）。
