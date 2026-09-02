# Changelog

All notable changes to this project are documented in this file.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

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
