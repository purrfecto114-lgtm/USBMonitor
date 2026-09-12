# Changelog

All notable changes to this project are documented in this file.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [2.4.0] — 2026-09-10

### Added — v1.1.1 视觉/交互面板完整移植（设备通知浮层）

把 1.1.1（PySide6）ToastWindow/VolumeRow/Theme 里"看得见、点得着"的
部分移植为原生 C99，替换 2.3.0 的纯文本 toast——**设备插拔现在弹出
完整的设备面板**（不是一行文字），且新增了 Python 版都没有的设备分类
能力：

- **设备面板**（`um_toast_win32.c`，GDI 双缓冲 + 圆角 + 淡入）：
  头部图标/标题/设备计数、逐设备行（标题/副标题/容量条/剩余空间）、
  hover 暂停倒计时、展开/折叠（多分区视图）、行点击打开、底部主按钮
  打开U盘、右键行菜单（打开/在资源管理器中显示/复制路径/安全弹出）、
  键盘可达（Esc 关闭、Enter 打开、Tab/方向键焦点环）、深浅色主题跟随
  系统（AppsUseLightTheme）、DPI 缩放、右下角工作区锚定
- **平台无关 UI 内核**（`um_toast_ui.c/h`）：model→layout→drawlist
  分层 + 命中测试 + 交互状态机 + 主题表 + UTF-8→UTF-16 解码（含代理
  对与非法序列 U+FFFD 替换），全部纯 C99 可在 Linux 上单测——守护进程
  永不链接 X11 的底线不变
- **设备证据采集层**（`um_enum.c/h`）：SetupAPI 按接口类枚举
  （DISK/VOLUME/HID/NET/HUB）+ FindFirstVolume 枚举**所有**卷（含无盘
  符卷）+ IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 卷→物理磁盘归属 +
  CM_Locate_DevNodeW/CM_Get_Parent 父链判 USB。由此面板能看见 2.3.0
  看不见的东西：未分配盘符的卷、读卡器空槽、BitLocker 未解锁卷、
  USB 拓展坞、无线网卡、HID 设备——并对每类给出正确的能力判定
  （非存储设备不可打开不可弹出，按钮置灰）
- **设备分类决策树**（`um_ui_classify`）：位掩码标签而非互斥枚举
  （复合设备如"智能笔+配置盘"正确呈现），证据不足一律判 UNKNOWN 不
  硬猜（1.1.1 的"可能尚未分配盘符"兜底猜测由真实证据取代）
- **多分区聚合**（`um_ui_group_by_disk`）：同一物理磁盘的多个卷合并
  为一行（E:、F:、G: 计 3 个分区），与原版 VolumeRow 聚合语义一致
- **面板 ↔ 托盘共享动作**：面板的 打开/显示/弹出 按钮与托盘菜单调
  用**同一份** `um_tray_open/reveal/eject_letter` 实现（一处修复两处
  生效）；安全弹出结果仍以右下角反馈 toast 呈现
- **线程封送协议**（UMWM_PANEL）：守护线程构建堆 `um_toast_model`
  → PostThreadMessage → GUI 线程拷贝入单实例面板并释放——面板与
  WM_DEVICECHANGE 热路径共用一个消息泵，无锁无竞态

### Added — v1.1.1 功能对齐补全（异步弹出 + 启动项 CLI）

对照 v1.1.1 tag 逐项核查后发现的两处真实缺口，本版补齐（等价实现，
方向可变但语义对齐）：

- **异步安全弹出**（v1.1.1 的 QThread SafeEjectWorker 等价物）：
  弹出 IOCTL 从 GUI 线程移到 `_beginthreadex` 工作线程——此前同步
  路径会在 DeviceIoControl + 3s 盘符确认期间阻塞消息泵（WM_DEVICECHANGE
  /托盘菜单/toast 全部停摆，v1.1.1 明确异步化正是为此）。现在点击
  "安全弹出"立即看到 **"正在安全弹出，请稍候…"** 状态行，工作线程
  完成后**同一槽位原位升级**为最终结果（成功绿/失败灰）——toast 槽位
  追踪表（GUI 线程独占）实现原位替换，不叠窗。工作线程与 GUI 线程
  之间只传**按值捕获**的 `eject_job`（盘符/型号/gui_tid/ttl/slot），
  零共享状态零锁；结果经 `um_gui_win_post`（线程安全 toast 投递）封送
  回 GUI 线程。托盘菜单与设备面板按钮共用同一条异步路径
- **启动项管理 CLI**（v1.1.1 的 `--install-startup/--uninstall-startup/
  --startup-status` 等价物）：Windows 写 HKCU Run（与托盘"随系统启动"
  开关同一键值，互为等价操作）；Linux 写 XDG autostart
  （`~/.config/autostart/usbmon.desktop`，`XDG_CONFIG_HOME` 感知，
  Exec 为 `/proc/self/exe` 解析的绝对路径）。三命令即测即退，输出
  人类可读状态（含注册位置说明）
- **v1.1.1 "最近操作"托盘子菜单的等价物声明**：1.x 在右键菜单维护
  recent_volumes 列表（分页展示）；2.x 以 JSONL 事件日志（含指纹、
  可审计、跨重启）+ 面板"已拔出：xxx"状态行 + 每动作即时反馈 toast
  实现同一"我知道刚才发生了什么"诉求，且不占菜单空间——属有意
  的等价实现而非缺失
- **v1.1.1 L1/L2 扫描缓存的明确弃置**：1.x 的 path→disk / disk→bus
  两级 LRU 是为 Python ctypes 往返开销设计的；C 直调 IOCTL 单次
  µs 级，缓存收益归零——按"减去不必要的弯路"原则不移植并在此存档

### Changed — 测试与门禁全面升级

- **新增 `tests/`（164 项断言）**：`ui_test`（112 项：布局/命中/状态
  机/主题/UTF-8 边界/聚合/分类）与 `enum_test`（52 项：采集层逻辑），
  在 Linux 上经 `tests/win32_shim.c`（最小 Win32 API 仿真层）端到端
  运行；`make strict` 一并严格编译，`make selftest` 运行
- `tools/demo.ps1` 断言 15 → 24 项：新增**启动项 CLI 往返**（--startup-
  status / --install-startup / --uninstall-startup：HKCU Run 键值出现/
  消失 + 状态输出 未启用→已启用→未启用 翻转）、**异步安全弹出链**
  （UMWM_TRAY_EJECT_TEST 驱动：工作线程 spawn → IOCTL → 结果 toast
  封送回 GUI 线程 → 托盘日志记录，守护进程全程存活）、**面板窗口创建**（类名
  usbmonToast2 + PID 归属）、**面板内容转储**（标题/副标题/存储+拓展
  坞+手写笔三行）、**行点击与主按钮双路径打开动作**、**展开真实改变
  窗口几何**（SetWindowPos 而非重画）、**Esc 隐藏+动作日志**。CI 无
  USB 设备，经 `USBMON_PANEL_TEST` 固定 3 设备证据集走完整生产链路
  （守护线程建模 → 堆封送 → GUI 拷贝 → 窗口 → 命中 → 回调 → 托盘
  动作）——与托盘测试同一"注入真实窗口消息"方法论
- `tools/demo.sh` 断言 11 → 18 项：新增 Linux 启动项 CLI 往返（隔离
  HOME，XDG autostart 桌面文件出现/Exec 内容/移除 + 状态翻转）
- **toast 助手 glibc 2.31 基线构建从 `debian:bullseye` 迁至 `ubuntu:20.04
  focal`**（同为 glibc 2.31，基线断言不变）：bullseye 于 2026-08 退出
  LTS 后镜像持续腐化——过期且内部不一致的索引、被清空的 pool、
  archive 无 bullseye-security、镜像预装版本高于冻结归档导致降级战；
  focal 在 archive.ubuntu.com 上仍是活的一致套件，构建回归纯安装
- `ci.yml`/`release.yml`：Linux job 新增 selftest 步骤；build-windows
  新增 SetupAPI/cfgmgr32 导入存在性断言（防采集层被误排除出链接）
- `Makefile`：`windows` 目标纳入三个新源文件并链接 `-lsetupapi
  -lcfgmgr32`（均为系统自带 DLL，自包含底线不变）；新增 selftest/
  ui-test/enum-test 目标与对应 clean 项
- 外部评审修复全部并入：`um_enum_collect` 返回值装夹（评审 CRITICAL，
  接口数 > max 时越界读，UBSan 可复现）、mingw -Werror 四个编译阻断
  （NULL_BRUSH 假常量/SetupDiGetDeviceInterfaceDetail A-W 混用/GUID
  头文件依赖/未用变量）、展开重锚几何、hover 暂停、GDI 笔泄漏、菜单
  前台化、定时器重画节流、UTF-8 非法序列拒绝、实例 ID 改由
  SetupDiGetDeviceInstanceIdW 提供、滚轮 max_scroll 夹逼

## [2.3.0] — 2026-09-03

### Added — 恢复 Windows 托盘与左/右键菜单（用户实测 v2.2.0 反馈）

用户实测 v2.2.0：双击 `usbmon.exe` 出现**黑色控制台窗口**，且原 1.x
Python 版具备的**系统托盘图标、左键/右键菜单**全部缺失。本版恢复完整
托盘体验（`src/tray_win32.c`，原生 Win32，零新增运行时依赖）：

- **系统托盘图标**（Shell_NotifyIcon，内嵌 `res/usbmon.ico`，与 toast
  同视觉语言；explorer.exe 重启后监听 `TaskbarCreated` 自动重挂）
- **左键 = USB 设备菜单**（对标原版 volume menu，弹出时实时扫描）：
  每个卷一条目，子菜单 打开 / 在资源管理器中显示 / 安全弹出；空态诚实
  显示"当前没有检测到 USB 存储设备"
- **右键 = 应用菜单**（对标原版 compact app menu）：状态（N 台设备）/
  立即重新扫描 / 工具（打开日志目录、打开配置目录）/ 随系统启动 /
  退出。与原版同样的左右键分工，设备列表只归左键
- **安全弹出** = `IOCTL_STORAGE_EJECT_MEDIA`（Python 原版的主路径，
  无 pywin32/COM 依赖）：发出后确认盘符真正消失，结果以右下角 toast
  反馈（成功绿条/失败灰条，被占用时明确提示）
- **随系统启动**：菜单勾选即写 HKCU Run 键（无需管理员）
- **退出**：干净停机（退出码 0，JSONL stop 原因 `tray-quit`，移除图标）
- **GUI 子系统（`-mwindows`）**：双击运行不再出现黑色控制台窗口；
  交互式 cmd/PowerShell 中的 `--version/--help/--list` 通过
  AttachConsole 附加父控制台照常打印（管道/CI 捕获不受影响）
- **内嵌图标**：`tools/make_icon.py`（纯标准库生成 16/32/48px ICO）+
  `res/usbmon.rc`，`make windows` 走 windres 编译链接（.ico 已提交，
  构建不依赖 Python）
- **线程模型**：托盘与 WM_DEVICECHANGE 监听共用同一 GUI 线程消息泵，
  菜单打开期间设备事件照常送达，无死锁面；菜单数据来自弹出时的独立
  全量扫描（不共享主线程状态，无锁）

### Changed — CI / 发布门禁随托盘升级

- `tools/demo.ps1` 断言 9 → 15 项：新增 **PE 子系统 = GUI**（防黑窗
  回归）、**托盘图标安装**、**左/右键菜单内容**（注入与真实点击完全
  相同的窗口消息，经 `USBMON_TRAY_TEST` 转储断言；CI 无 explorer 时
  自动拉起一个）、**托盘触发重扫**（新的 `wake=hot` 轮）、**托盘退出**
  （退出码 0 + stop 原因 `tray-quit`）
- `ci.yml` / `release.yml` 的 build-windows：统一走 `make windows`
  （原 ci.yml 手写 gcc 行有漂移风险），新增断言 PE 为 GUI 子系统、
  `.rsrc` 图标段存在、windres 可用性预检
- `Makefile`：`windows` 目标新增 `res/usbmon.res.o`（windres）并链接
  `-mwindows -lshell32 -ladvapi32`（Shell_NotifyIcon / 注册表），全部
  仍为系统自带 DLL
- 停机事件记录原因（`signal` / `tray-quit`），README 新增托盘章节与
  原版对齐说明

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
