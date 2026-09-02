# Changelog

All notable changes to this project are documented in this file.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [2.2.0] — 2026-09-02

### Added — Windows 成为首要目标平台（发布产物含 Windows 程序）

用户澄清**目标平台是 Windows**，而 2.x 的三版 Release 均只含 Linux
二进制——对 Windows 用户而言"release 中不包含程序运行时"的根源即此。
本版把 Windows 纳入一等公民：构建、验证、发布全链路。

- **发布产物新增 `usbmon-<ver>-windows-amd64.zip`**：mingw-w64 交叉编译的
  `usbmon.exe`，`-static` 完全静态自包含——PE import 表仅
  `KERNEL32.dll / USER32.dll / GDI32.dll / msvcrt.dll`（全部随 Windows
  系统自带，msvcrt 自 Win98/NT4 起即为系统组件），**目标机无需安装任何
  运行时**。strip 后约 89 KB（CI 实测 91,136 字节）。
- **Windows 真机验证链 `tools/demo.ps1`**（windows-latest runner，15 项
  断言）：`--version/--help/--list/--once`、未知选项拒绝（退出码 2）、
  JSONL 逐行合法（start/round/stop 齐全）、hooks 配置解析（start 事件
  hooks=N）、单实例锁（第二实例退出码 3）、GUI 线程与隐身**顶层**监听窗口
  创建（按窗口类名+PID 精确匹配）、**模拟系统级 WM_DEVICECHANGE 广播后
  守护进程即时唤醒、JSONL 出现 `"wake":"hot"`**——该广播与操作系统
  在卷到达时发送的消息完全一致，message-only 窗口的旧缺陷必挂此项。
- `ci.yml` 新增两个 job：`build-windows`（mingw-w64 交叉编译 +
  `-Werror` 零警告 + PE import 自包含断言 + strip）与
  `verify-windows`（windows-latest 真机跑 demo.ps1）——每次 push 都
  在真 Windows 上验证。
- `release.yml` 重构为多 job 门禁链：meta（版本一致性 + 已存在跳过）→
  build-linux（严格构建 + 双 demo 回归 + musl 静态 + bullseye toast +
  Xvfb GUI）∖ build-windows（mingw 静态 + import 断言）→
  verify-windows（真机 15 断言）→ release（双平台打包、tag 冲突
  拒绝、发布三资产：windows zip + linux tarball + SHA256SUMS）。
- `Makefile`：`make windows` 升级为 strict（`-Wall -Wextra -pedantic
  -Werror`）+ `-static`（去掉未使用的 `-lshell32`）；新增
  `make dist-windows`（strip + zip + 追加 SHA256SUMS，须在 `make dist`
  之后运行，同一份校验和覆盖双平台）。

### Fixed — Windows 代码地毯式审查发现的缺陷（此前从未编译过，全部实测复现）

- **热路径致命缺陷（监听窗口类型）**：`gui_win32.c` 用 `HWND_MESSAGE`
  父窗口创建监听窗口——message-only 窗口**收不到任何广播消息**（微软
  文档《Window Features》明文，Raymond Chen 多次撰文确认），而
  WM_DEVICECHANGE 设备事件正是广播给所有顶层窗口的。即热路径完全失效、
  守护进程退化为纯 1h 轮询。改为**不可见顶层窗口**（不 `ShowWindow`）。
- **第二个热路径致命缺陷（`gui.enabled` 从未置位）**：`gui.c` 的
  Windows 分支 `um_gui_init` 返回成功却不设置 `g->enabled = 1`——
  main() 的等待循环因此永远走 `sleep_ms()` 分支（**从不等待唤醒事件**），
  且 `um_gui_show_add/remove` 一律提前返回（**任何 toast 都弹不出来**）。
  POSIX 分支有此标志，Windows 分支遗漏。真机 CI 广播测试抓到。
- **编译错误**：`DBT_DEVTYP_DISK` 常量不存在（dbt.h 仅定义
  OEM/PORT/VOLUME/DEVICEINTERFACE/HANDLE 五种广播设备类型）；
  `DEV_BROADCAST_*`/`DBT_*` 实际位于 `<dbt.h>` 而非 `<shellapi.h>`；
  `main.c` 在不含 `<signal.h>` 的 Windows 分支使用 `sig_atomic_t`；
  `util.c` 缺 `<direct.h>`（`_mkdir` 声明）。修复后 mingw-w64 13.2
  `-std=c99 -Wall -Wextra -pedantic -Werror` 零警告（该代码首次被
  真正编译——此前"零警告"的说法只对 Linux 成立）。
- **hooks 在 Windows 上几乎不可用**：
  - 旧实现对 `&|<>()%^` 元字符的拒绝会误杀
    `C:\Program Files (x86)\...` 等合法路径（Python 原版的 argv 数组
    语义本就允许这些字符）；BatBadBut（CVE-2024-24576）的真实防线是
    **拒绝 `.bat/.cmd/.ps1` 可执行文件**（CreateProcess 会为其隐式拉起
    cmd.exe）+ 直启 `.exe` 不经 shell——两者保留，元字符拒绝移除。
  - `{path}` 占位符在 Windows 上展开为 `E:\`（尾部反斜杠），旧的
    trailing-backslash 拒绝使**所有带 `{path}` 的 hook 在 Windows 永不
    触发**。现按 CRT 参数引号规则实现尾部反斜杠 2n 加倍
    （`"E:\"` 正确往返为单个 token）。
- `gui_win32.c` 线程参数传递简化为直接传 `um_gui*`（原栈上 ctx 拷贝
  存在理论生存期竞态）。

### Removed — 撤回错误的发布

- 删除 v2.0.0 / v2.0.1 / v2.1.0 的 Release 与 tag：三版产物均不含
  Windows 程序，与"目标平台是 Windows"不符。v1.x（原 Python 版）历史
  原样保留。v2.2.0 起发布资产包含 Windows 静态自包含 exe。

## [2.1.0] — 2026-09-02

### Fixed — 发布产物自包含（release 中此前不含可移植运行时）

v2.0.1 及更早的 tarball 里两个二进制都是**动态链接**产物：守护进程
依赖构建机的 glibc 符号基线（`GLIBC_2.34`，仅 Ubuntu 22.04+/Debian 12+
可运行），toast 助手另有 17 个动态库依赖。用户正确指出"release 中不
包含程序运行时"。按业界标准做法修复：

- **守护进程 `usbmon` → musl 完全静态链接**：无 ELF interpreter、无
  glibc 符号基线，任意 x86-64 Linux 内核即跑——这才是名副其实的
  "零运行时依赖"。选 musl 而非 `-static` glibc 的原因：glibc 静态
  有 NSS/locale dlopen 的经典坑，本项目虽未调用 NSS，仍按社区共识
  走 musl（musl 本身就是为静态链接设计的）。
- **toast 助手 → glibc 2.31 基线**：在 `debian:bullseye` 容器中构建，
  覆盖 Ubuntu 20.04+/Debian 11+；运行时仅需桌面标配的
  `libX11/libXft/fontconfig`（README 已明确声明）。
- CI 在每次 push 与每次发布时都验证静态链接性
  （`ldd` 必须报 "not a dynamic executable"）并以静态二进制完整跑
  11 项 hooks 回归；发布时额外校验 toast 的 glibc 符号基线 < 2.32。

### Added

- `make static`：musl 静态守护进程构建目标（缺 musl-gcc 时显式报错
  而非静默降级）；`make dist` 优先打包静态版。
- `tools/demo.sh` 支持 `USBMON=` 覆盖被测二进制（同一套 11 断言既测
  动态版也测静态版）。

## [2.0.1] — 2026-09-02

### Added — 发布自动化

- `make dist`：一键产出发布产物（严格构建 → strip → tarball →
  SHA256SUMS），本地与 CI 产物同源同构，避免手滑漏文件。
- `.github/workflows/release.yml`：推送版本相关变更（`src/usbmon.h` /
  `CHANGELOG.md` / `Makefile` / workflow 自身）到默认分支时自动：
  校验版本一致性（usbmon.h ↔ CHANGELOG 章节）→ 严格构建 → 冒烟 +
  hooks 回归 → `make dist` → 从 CHANGELOG 提取 notes → 创建
  `vX.Y.Z` tag → 发布 Release 并上传资产。**Release 已存在则安全跳过；
  tag 指向不同提交则拒绝执行（不移动历史 tag）**。也支持
  workflow_dispatch 手动重跑。
- `tools/release_notes.py`：CHANGELOG 章节提取工具（发布 notes 与仓库
  记录单一来源）。

### Fixed

- v2.0.0 手动发布时踩过的坑自动化解决：发布产物由 CI 从同一提交构建，
  `SHA256SUMS.txt` 随资产一同生成上传，不再依赖本地环境。

## [2.0.0] — 2026-09-02

对 Python 版（PySide6 + pywin32，4,159 行单体 `app.py`）的**原生重置版**：
C99 重写，无网页、无托盘、守护进程零运行时依赖。版本号跳到 2.0.0
标记架构重置（1.x 为 Python 血统，历史见旧仓库）。

### Changed — 架构重置（why）

常驻小工具最要命的三个指标是**内存驻留、启动延迟、部署复杂度**，
而"运行时吞吐"从来不是问题（USB 事件低频、IOCTL 是毫秒级 I/O）。
同沙箱实测对比：

| 指标 | Python 原版 | usbmon 2.0 (C99) |
|---|---|---|
| 进程启动 | 58 ms（仅解释器+非 GUI import） | **0.56 ms** |
| 常驻内存 | 17.9 MB（非 GUI import）；PySide6 托盘业界 41–220 MB | **3.1 MB / 1 线程** |
| 产物体积 | Nuitka onefile + UPX ≈ 10–30 MB | **59 KB**（另有 26 KB 弹窗助手，可选） |
| 运行时依赖 | Python ≥3.11、PySide6、pywin32 | **无**（弹窗助手仅 X11+Xft） |

### Added — 新能力

- **1h 一轮契约**：`interval=3600s` 保底轮询；插拔通过内核事件
  （Linux inotify 监视 `/sys/block` / Windows `WM_DEVICECHANGE` 隐藏窗口）
  **毫秒级唤醒**，0.7 s 防抖等待挂载落定；`--no-hotpath` 强制严格间隔模式。
- **插入 U 盘弹窗（保留原版体验）**：Linux 由独立助手进程 `usbmon-toast`
  渲染（Xlib+Xft，右下角 4 槽堆叠、点击/超时消失、CJK 字形逐字体运行时探测），
  守护进程本体永不链接 X11；Windows 由进程内 GUI 线程实现。
  首轮 baseline 设备**不弹窗**（登录瞬间不该被窗口轰炸）。
- **状态持久化**：快照落盘 `last-snapshot.txt`，守护重启 / cron `--once`
  跨运行去重，**零重发 add / 零重触发 hooks**。
- **JSONL 事件日志**：add/remove/round/start/stop；`round.wake`
  字段区分 `start`/`hot`/`tick`，热路径效果可直接审计；1 MB 自动轮转 ×3。
- **hooks**：argv 数组执行（`execv` / `CreateProcessW`，不经任何 shell），
  glob 匹配、占位符替换、60 s reaper SIGKILL、SIGHUP 热重载；
  Windows 保留 BatBadBut（CVE-2024-24576）与 CmdHijack 防护。
- **安全默认**：序列号默认记 sha256 截断指纹（`--log-raw` 才记明文）；
  `--list` 严格只读（不弹窗、不触发 hooks）；单实例锁（重复启动退出码 3）。

### Removed — 原版的过度工程

- L1/L2 LRU+TTL 缓存（1h 一轮的扫描只要亚毫秒）
- 三层 debounce、托盘/常驻主窗口/SVG/动画
- 启动项自愈、源码包复制、venv 启动 bat、清单签名
  （静态单文件没有"部署状态"可自愈）

### Fixed — 相对原版的历史问题

- 原版 Python hooks 的 BatBadBut/CmdHijack 防护在 C 版同等保留并加强
  （元字符集合包含 `"`，占位符替换后逐 token 重新校验）。
- 守护进程重启重发 baseline add（原版已知行为）→ 状态持久化后零重发。
- `--list` 等只读命令不再触发用户自动化（原版会触发 hooks）。
- reaper 表满时僵尸子进程泄漏 → 有界回收 + 溢出兜底路径。
- Windows 路径的历史缺陷按静态评审修复：IOCTL
  `STORAGE_DESCRIPTOR_HEADER` 8 字节 size-probe、`disks[]` 初始化、
  `UM_MAX_DEV` 越界写、`_snwprintf_s` 截断回写、1.5 MB `um_hooks`
  移出栈、`CREATE_NO_WINDOW`、`QueryPerformanceCounter` 单调时钟、
  `Local\` 命名空间互斥体、宽字符日志 I/O。
  **[WINDOWS-UNVERIFIED]**：Windows 代码按 Win32 API 编写并经静态评审，
  但本仓库 CI 只构建 Linux；上线前请在 Windows 上构建并冒烟。

### Verified — 沙箱实测

- `-Wall -Wextra -pedantic -Werror`（gcc 14）零警告，含 Xft 助手；
- `gcc -fanalyzer` 唯一告警为 hook.c argv 循环已知误报；
- ASan/UBSan：守护进程与 `--once` 全场景零错误零泄漏；
- `tools/demo.sh` hooks 全链路回归通过；`tools/demo-gui.sh` 11/11 通过
  （Xvfb 下插拔→弹窗→中文渲染经 VLM 图像识别验证）。

---

1.x 的 Python 版历史记录见旧仓库 CHANGELOG（此处不再重复）。
