# usbmon — 原生 USB 存储监控守护进程（C99，Windows 首要目标平台）

> **命名消歧**：本项目与 Linux 内核自带的 `usbmon`（`CONFIG_USB_MON`，
> debugfs 下的 USB **数据包**抓包设施，docs.kernel.org/usb/usbmon.html）**同名但无关**。
> 本项目是用户态**存储设备**插拔监控；内核 usbmon 是总线级**报文**嗅探器，
> 无用户态可执行文件，安装路径不冲突。如需抓 USB 报文请用内核 usbmon + trace-cmd。

对原 `USBMonitor`（Python + PySide6，4,159 行单体 app.py）的**重置版**。
**首要目标平台是 Windows**（发布产物以 Windows 为主，Linux 同源支持）：
无托盘、无网页、守护进程零运行时依赖。默认**每 1 小时一轮**扫描（`1h 一轮`，
保底节奏）；插拔事件通过内核事件（Windows: WM_DEVICECHANGE / Linux:
inotify）**毫秒级唤醒**，插入 U 盘时右下角弹出设备信息窗，事件写入 JSONL
日志，可选 hooks 在插入时以 argv 数组执行外部命令。

```
KEY      BUS       MODEL                       SIZE  MOUNT
E: F:    usb       SanDisk Cruzer Glide 3.0   28.2 GB  E:\
```

## 为什么重写（与 Python 版的实测对比）

沙箱实测（gcc -O2，Python 3.12）：

| 指标 | Python 原版（推算/引用） | usbmon (C) 实测 | 差距 |
|---|---|---|---|
| 进程启动 | 58 ms（仅解释器+非 GUI import）；PySide6 GUI 版 0.5–3 s（社区实测，Vorta 托盘常驻 215–220MB） | **0.56 ms** | ~100× |
| 常驻内存 | 17.9 MB（仅非 GUI import）；PySide6 托盘 41–220 MB | **3.1 MB**（1 线程） | 5.8–70× |
| 产物体积 | Nuitka onefile + UPX ≈ 10–30 MB，另带 venv/启动 bat/自愈复制 | **Windows: 89 KB**（mingw 静态 exe，CI 实测 91,136 B）；Linux: 116 KB（musl 静态守护进程） | ~100–340× |
| 运行时依赖 | Python ≥3.11、PySide6、pywin32 | **Windows：无**（静态 exe 仅 import 系统自带 DLL）；Linux 守护进程：无（musl 静态）；Linux toast 助手：libX11/libXft/fontconfig（桌面标配） | — |
| 代码规模 | 4,959 行 + 18 个测试文件 + Nuitka/UPX 构建链 | ~3,800 行 C（含跨平台两套扫描器） | — |

诚实结论：原版"运行时吞吐性能"其实不是问题（USB 事件低频、IOCTL 是毫秒级 IO，
ctypes 的 µs 级 FFI 开销可忽略）。真正的问题是**内存驻留、启动延迟、部署复杂度**——
这恰是常驻小工具最要命的三个指标。原生语言（C/Rust/Go/Zig）是这类工具的正确选择。

## 构建

```bash
# Linux（开发/沙箱）：
make                  # 守护进程 + usbmon-toast 弹窗助手（需 X11/Xft 开发文件）
make static           # musl 静态守护进程（需 musl-tools；发布产物同款）
make strict           # -Wall -Wextra -pedantic -Werror 全绿

# Windows（首要目标平台，mingw-w64 交叉编译，静态自包含）：
sudo apt-get install gcc-mingw-w64-x86-64        # Ubuntu/Debian
make CROSS=x86_64-w64-mingw32- windows           # → usbmon.exe（strict + -static）
make CROSS=x86_64-w64-mingw32- dist-windows      # → dist zip + 追加 SHA256SUMS

# 发布打包（双平台，产物与 CI 同构）：
make dist && make CROSS=x86_64-w64-mingw32- dist-windows
```

Windows 产物为 **mingw-w64 完全静态链接**：PE import 表仅
`KERNEL32.dll / USER32.dll / GDI32.dll / msvcrt.dll`——全部随 Windows 系统
自带（msvcrt 自 Win98/NT4 起就是系统组件），**目标机无需安装任何运行时**，
单文件即拷即用（CI 断言无 libgcc/libwinpthread 等运行时 DLL）。MSVC 亦可
直接编译（源码为标准 C99 + Win32 API）。

Linux 上守护进程**永不链接 X11**：弹窗由独立助手二进制 `usbmon-toast`
渲染（每事件 fork+exec，纯 argv，不经 shell）；无 X11 开发文件时助手不
构建、守护进程照常运行。Linux 发布产物中的守护进程为 **musl 完全静态
链接**（无 glibc 符号基线，任意 x86-64 Linux 可跑）；toast 为 glibc ≥ 2.31
基线 + 桌面标配库。Windows 上弹窗与 WM_DEVICECHANGE 热路径由进程内 GUI
线程实现（`gui_win32.c`，user32/gdi32 随系统存在，无需助手二进制）。

## 用法（Windows）

```
usbmon.exe                       # 守护进程：立即一轮，之后每 3600s 一轮（保底 1h），
                                 # 插拔即时唤醒 + 右下角弹窗
usbmon.exe --interval 300        # 自定义轮询间隔（秒）
usbmon.exe --once                # 单轮退出（任务计划程序模式，跨运行去重）
usbmon.exe --list                # 列出当前外部存储设备（只读：不弹窗、不触发 hooks）
usbmon.exe --gui / --no-gui      # 强制开/关弹窗（守护模式默认自动）
usbmon.exe --toast-secs 30       # 弹窗自动消失秒数（默认 12）
usbmon.exe --no-hotpath          # 忽略插拔唤醒，严格按间隔轮询
usbmon.exe --baseline            # 首轮把在位设备全部报为 add
usbmon.exe --log FILE            # JSONL 日志路径
usbmon.exe --log-raw             # 序列号明文入日志（默认为 sha256 指纹）
```

- **数据目录**：`%LOCALAPPDATA%\usbmon\`（`events.jsonl`、
  `last-snapshot.txt`、日志轮转 `.1/.2/.3`）
- **hooks 配置**：`%APPDATA%\usbmon\hooks.json`
- **开机自启（无需管理员）**：注册表
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 新增字符串值
  `usbmon = C:\path\to\usbmon.exe`（与原 1.x Python 版同一做法），或把
  快捷方式放进启动文件夹；也可用
  `schtasks /create /tn usbmon /tr C:\path\usbmon.exe /sc onlogon /rl LIMITED`
  （可加 `/delay 0000:30` 错峰登录）。守护进程自身不需要任何特权。

守护进程持久化最近快照并在每轮后更新，**重启不会重发 add / 重触发
hooks**；`--once` 复用同一状态文件，定时任务同样能正确产生 add/remove。
首轮在位设备标记 `"baseline":1` 且不弹窗（登录瞬间不该被窗口轰炸）。

Linux 用法完全相同（数据目录 `~/.local/state/usbmon`），详见上文 CLI。

### 弹窗（保留原版"插入 U 盘即弹窗"体验）

```
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
┃ USB 设备已插入                     ┃
┃ 金士顿 DataTraveler 3.0 (disk3)   ┃
┃ 容量 62.7 GB · 2 个分区            ┃
┃ 挂载点 E:\                         ┃
┃ 序列 sha256:60a44c4c12ab           ┃
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
```

- 右下角堆叠（4 槽位），绿色边条=插入、灰色=拔出；点击或超时自动消失
- 中文设备名正常渲染（Linux 助手运行时逐字体探测 CJK 字形；Windows 用
  系统 Unicode API，天然支持）
- 序列号默认显示指纹，与日志一致（`--log-raw` 才显明文）

## 事件日志（JSONL）

```json
{"ts":"2026-09-01T12:00:00Z","ev":"add","key":"disk3","bus":"usb","model":"SanDisk Cruzer Glide 3.0","serial":"sha256:2239aa9f1386","size_bytes":30262169600,"partitions":["E:","F:"],"mount":"E:\\","fs_total_bytes":30256263168,"fs_free_bytes":10758389760}
{"ts":"2026-09-01T12:00:01Z","ev":"add","baseline":1,"key":"disk4","bus":"usb","model":"金士顿 DataTraveler 3.0", ...}
{"ts":"2026-09-01T12:00:02Z","ev":"remove","key":"disk3"}
{"ts":"2026-09-01T13:00:00Z","ev":"round","n":12,"external":1,"interval_s":3600,"wake":"tick","scan_ms":0.3}
```

- `serial`：默认是截断 SHA-256（12 hex，用于跨日志关联同一设备）。
  **不是加密、不是匿名化**，与原版语义一致；`--log-raw` 才记录明文。
- `round.wake`：`start`（启动首轮）/ `hot`（插拔事件唤醒）/ `tick`（间隔
  到期）——热路径效果可直接从日志审计。
- `baseline:1`：首次见到该设备（无历史状态）而非插拔产生的 add。
- 日志 1MB 自动轮转，保留 3 份（`.1`/`.2`/`.3`）。

## Hooks（可选，默认无）

配置（Windows: `%APPDATA%\usbmon\hooks.json`；Linux:
`~/.config/usbmon/hooks.json`）：

```json
{
  "hooks": [
    {
      "name": "auto-backup",
      "match_keys": ["disk*", "?:*"],
      "match_models": ["SanDisk*"],
      "command": ["C:\\Tools\\backup.exe", "--src", "{path}", "--dst", "D:\\backup"],
      "debounce_seconds": 3,
      "enabled": true
    }
  ]
}
```

- 占位符：`{key}`（设备名）、`{model}`、`{path}`（挂载点，Windows 为
  `E:\` 形式，Linux 为 `/dev/sdX`）
- **命令永远是 argv 数组，不经过任何 shell**（POSIX `execv` / Win32
  `CreateProcessW`）
- Windows 上保留 BatBadBut（CVE-2024-24576）防线：**拒绝 `.bat/.cmd/.ps1`
  作为可执行文件**（CreateProcess 会为其隐式拉起 cmd.exe，这是该漏洞的
  唯一触发路径）；其余一律直启 `.exe`，命令行按 CRT 参数引号规则拼装
  （尾部反斜杠 2n 加倍），**任何 shell 都不参与参数解析**——因此含空格/
  括号的合法路径（`C:\Program Files (x86)\...`）与 `{path}` 展开的
  `E:\` 都能正确传递
- 子进程 stdio → `NUL` / `/dev/null`；超过 60s 由 reaper 终止
- 守护进程 `SIGHUP` 重载 hooks 配置（Windows 无对应信号，改配置后重启即可）
- **Hooks 不是沙箱**：子进程拥有当前用户全部权限，勿用不可信配置文件

## 架构

```
┌ Win32: GUI 线程隐身顶层窗口收 WM_DEVICECHANGE（message-only 收不到广播）┐
│ Linux:  inotify 监视 /sys/block（插拔即醒，空闲零 CPU）                  ┘
                        │ 醒后 0.7s 防抖（等挂载落定）
                        ▼
┌ Win32: GetLogicalDrives → IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS          ┐
│         → \\.\PhysicalDriveN + IOCTL_STORAGE_QUERY_PROPERTY              │
│ Linux: /sys/block + /sys/block/*/device(usb?) + /proc/mounts            ┘
                        │ 每轮全量快照（µs~ms 级，无需 LRU 缓存）
                        ▼
              快照 diff → add / remove 事件
                        │
        ┌───────────┬──────────┬────────────────┐
        ▼           ▼          ▼                ▼
   JSONL 日志   hooks(argv exec)  弹窗（Win32 线程内/Linux 助手进程）  --list 表格
```

刻意**不做**的事（原版过度工程的部分）：
- 无 L1/L2 LRU+TTL 缓存——每小时一轮的扫描本身只要亚毫秒
- 无三层 debounce——一个防抖窗口 + hooks 侧每 (规则,设备) 时间戳即可
- 无启动项自愈/源码包复制/清单签名——静态单文件没有"部署状态"可自愈
- 无托盘/常驻主窗口/SVG/动画——只在事件发生时弹一个轻量通知窗
- Windows 弹窗留在进程内（user32/gdi32 系统必带）；Linux 守护进程本体
  不链接 X11（弹窗隔离在助手进程里，崩了也不影响监控）

## 发布（自动化，双平台）

推送版本相关变更（`src/usbmon.h`、`CHANGELOG.md`、`Makefile`、
`.github/workflows/release.yml`）到默认分支时，`release.yml` 自动完成：

1. 版本一致性校验（usbmon.h ↔ CHANGELOG 章节）+ 已存在 release 安全跳过
2. Linux：严格构建 + musl 静态 + 11 断言 hooks 回归 ×2 + bullseye 基线
   toast + Xvfb GUI 回归 → 打包 tarball
3. Windows：mingw-w64 静态构建（`-Werror` 零警告）+ PE import 自包含
   断言（不得出现 libgcc/libwinpthread 等运行时 DLL）
4. **windows-latest 真机验证**：`tools/demo.ps1` 15 项断言门禁——任何
   一项失败都阻止发布
5. 从 CHANGELOG 提取 notes → tag 冲突检查（存在且指向不同提交则拒绝）→
   发布 Release，上传三份资产：
   `usbmon-<ver>-windows-amd64.zip`、`usbmon-<ver>-linux-amd64.tar.gz`、
   `SHA256SUMS.txt`（双平台统一校验和）

本地等价操作（产物同构）：

```bash
make dist && make CROSS=x86_64-w64-mingw32- dist-windows
```

发布新版本的流程：改 `src/usbmon.h` 的 `UM_VERSION` + 在 `CHANGELOG.md`
顶部加对应 `## [X.Y.Z]` 章节 → push —— 工作流完成其余全部。

## 目录

```
usbmon/
├── Makefile             linux/windows/静态/发布打包 一站式
├── README.md
├── CHANGELOG.md
├── LICENSE              MIT（与 1.x Python 版同源授权）
├── .github/workflows/   ci.yml（每次 push，含 Windows 真机验证）
│                        release.yml（版本变更时发布，双平台门禁）
├── src/
│   ├── usbmon.h       公共类型与契约
│   ├── main.c         CLI、快照 diff、轮循环、热路径接线、状态持久化
│   ├── scan_linux.c   sysfs 扫描器（支持 --sys-root 指向假 /sys 测试）
│   ├── scan_win32.c   Win32 扫描器（与 Python 原版同款 IOCTL 策略）
│   ├── hotpath.c      inotify 热路径（POSIX；Windows 用 WM_DEVICECHANGE）
│   ├── gui.c          GUI 管理器（Windows 线程接线 / Linux 助手发现+回收）
│   ├── gui_toast.c    Linux 弹窗助手：Xlib+Xft 渲染、CJK 字体探测
│   ├── gui_win32.c    Windows GUI 线程：Win32 弹窗 + 顶层监听窗口
│   ├── hook.c         hooks 解析/调度/BatBadBut 防线/子进程 reaper
│   ├── json.c         迷你递归下降 JSON 解析器（深度/节点数封顶）
│   ├── logjson.c      JSONL 日志 + 轮转
│   ├── lock.c         单实例（flock / Local\ 命名互斥体）
│   └── util.c         时间、glob、SHA-256 指纹、目录助手
└── tools/  demo.sh + demo-gui.sh + demo.ps1 + release_notes.py
           Linux hooks 回归 / Linux GUI 回归 / Windows 真机冒烟 / 发布 notes
```

## 已验证行为

**Windows（windows-latest 真机 CI，`tools/demo.ps1` 15 项断言全过）：**
- `--version`（含与发布 tag 一致性）/ `--help` / `--list` / `--once`
  退出码与输出全部正确；未知选项退出码 2
- `--once` 产出 JSONL 逐行合法（start/round/stop 齐全）；hooks.json
  解析（start 事件 `hooks=N`）
- 单实例锁：第二实例退出码 3；无头守护进程（`--no-gui`）稳定常驻
- GUI 线程启动 + **隐身顶层监听窗口存在**（EnumWindows 按类名
  `usbmonListen` + PID 精确匹配）
- **模拟系统级 WM_DEVICECHANGE 广播 → 守护进程即时唤醒 → 日志出现
  `"wake":"hot"`**（广播内容与操作系统卷到达通知逐字节一致；若监听
  窗口仍是 message-only，此项必挂——正是用来钉死该类缺陷）
- mingw-w64 13.2 `-std=c99 -Wall -Wextra -pedantic -Werror` 零警告；
  PE import 断言：仅 KERNEL32/USER32/GDI32/msvcrt，无任何运行时 DLL

**Linux（沙箱实测）：**
- `--list` / `--once` / 守护模式 / SIGTERM 干净退出；跨 `--once` 与跨
  重启的 add/remove（状态文件，重启零重发）；单实例锁退出码 3
- hooks：正常触发、glob 匹配、占位符替换、未知占位符拒绝、损坏 JSON
  降级、exec 失败(127)与超时(60s)被记录/击杀（mock sysfs 11 项断言）
- SHA-256 指纹与 `python3 hashlib` 逐字节一致；JSONL 每行合法 JSON
- GUI（Xvfb，`tools/demo-gui.sh` 11/11）：插拔约 1s 内弹窗（inotify +
  0.7s 防抖）、中文渲染（VLM 图像识别验证）、拔出灰窗、4 槽堆叠、
  点击/超时消失、零僵尸、`--no-hotpath` 严格间隔、无 DISPLAY 自动降级
- 编译零警告（含 Xft 助手）；ASan/UBSan 全场景零错误零泄漏（助手仅剩
  libfontconfig 内部 320B 上游缓存）；`gcc -fanalyzer` 唯一告警为
  hook.c 已知误报

**诚实的边界**：物理插拔（真 U 盘插入）无法在 CI 模拟——CI 用与操作系统
插拔时**完全相同的** WM_DEVICECHANGE 广播验证事件链路（监听窗口→唤醒→
防抖→扫描→日志），Windows 扫描器与弹窗渲染已分别验证；首次拿到 exe 后
建议实机插拔一次做最终体验确认。Linux 基线产物 toast 需桌面环境
（glibc ≥ 2.31）。
