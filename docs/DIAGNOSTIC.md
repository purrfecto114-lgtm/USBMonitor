# USBMonitor 2.6.0 诊断与验证

本次以附件中的 2.5.0 为基线，基线提交标识为 `b35f3befe156e8cee8db7b7432d00d73db7f118c`。以新的 2.6.0 源码与双平台产物交付。

## 基线故障与取舍

| 维度 | 诊断 | 本次处理与代价 |
| --- | --- | --- |
| Linux 事件时效 | 基线监听 `/sys/block` 的 inotify。基线测试替换为普通目录，无法覆盖真实 sysfs 的限制。 | 正常扫描使用内核 block uevent，测试目录使用 inotify。保留间隔复核和短时间合并。容器限制 netlink、队列丢失或设备在两次快照间瞬时插拔时，事件仍可能延迟或缺失。 |
| 设备识别 | Windows 守护进程从盘符出发，面板和托盘各有一条扫描路径。目录挂载、无盘符磁盘和多盘卷容易产生分歧。 | 统一从 SetupAPI 采集，借助卷 GUID、磁盘号和挂载路径关联。只把明确归属单个磁盘的卷计入该设备。部分驱动或权限不足时，设备可能有盘号但缺少容量或卷详情。 |
| 状态一致性 | 基线 `--once` 与守护进程可并发改写，扫描失败后仍会保存旧值或空值，Windows CRT 的 `rename` 也有覆盖差异。 | 写入进程共用文件锁，成功扫描后原子替换快照，读取旧的单列格式。保留日志用的 `diskN`、`sdb` 键，以独立指纹识别已知的同名设备替换。缺少可靠身份时按原键匹配。 |
| 移除安全 | 原操作使用媒体弹出 IOCTL，且通过盘符定位。 | 按设备实例定位并重新核实 PnP 可移除能力，拒绝复合接口路径上的自动弹出。只有系统返回成功才显示移除成功。某些驱动、远程会话和需额外权限的环境只能由系统界面处理。 |
| UI 与无障碍 | 基线滚动可显示被裁切的卡片，焦点和显示范围会错位。Linux 插入通知缺少容量。 | 改用完整可见行、明确焦点、独立动作区和深浅主题。Windows 读取系统高对比度色。保留原生 GDI 和 Xft，依赖较少。自绘控件仍缺少 UI Automation 语义树。 |
| Hook 与隐私 | 配置中直接输入的 UTF-8 多字节字符被重复编码。命令名可受设备占位符影响，或在 Windows 上依赖搜索路径。 | 保留中文和空参数，拒绝嵌入 NUL 与超大配置。设备字段仅替换 argv 参数。Windows 命令改为绝对 `.exe`，相对命令需要迁移。默认指纹方便排查，原始序列字段由 `--log-raw` 控制。 |
| 构建与维护 | Windows 有两套磁盘探测逻辑，测试 shim 与生产走不同的枚举实现。演示脚本会清理固定临时路径和默认快照。 | 收敛到同一枚举调用序列，shim 只模拟系统 API。测试隔离状态、日志和配置，新增失败场景。保持现有 Makefile 和 Windows 原生冒烟入口。 |

## 依据

- Linux inotify 手册列出 sysfs 的监听限制。内核文档要求通过解析后的设备路径与 `subsystem` 链接判断设备归属。[inotify(7)](https://man7.org/linux/man-pages/man7/inotify.7.html) · [Linux 内核 sysfs 访问规则](https://docs.kernel.org/admin-guide/sysfs-rules.html)
- Windows 的 devnode 变化消息适合唤醒后刷新设备清单。卷设备句柄要使用去掉末尾反斜杠的卷 GUID 路径。[DBT_DEVNODES_CHANGED](https://learn.microsoft.com/windows/win32/devio/dbt-devnodes-changed) · [Basic and Dynamic Disks](https://learn.microsoft.com/windows/win32/fileio/basic-and-dynamic-disks)
- 访问容量可能因 ACL、挂载或配额失败。界面因此只把结果解释为当前用户可访问性。[GetDiskFreeSpaceExW](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexw)
- PnP 设备安全移除 API 会返回系统拒绝。媒体弹出 IOCTL 的对象是介质。[CM_Request_Device_EjectW](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_request_device_ejectw) · [IOCTL_STORAGE_EJECT_MEDIA](https://learn.microsoft.com/windows/win32/api/winioctl/ni-winioctl-ioctl_storage_eject_media)
- Windows 要在 DPI 变化后重新布置界面。XDG 的 `Exec` 字段对路径和引号有独立转义规则。[Microsoft 高 DPI 开发](https://learn.microsoft.com/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows) · [Desktop Entry Exec](https://specifications.freedesktop.org/desktop-entry/latest/exec-variables.html)

## 验收范围

基线的 Win32 模拟层通过 191 项断言。端到端演示通过 14 项，自启动相关 4 项失败。该运行环境缺少常规 `/proc` 和 `/tmp`，演示还使用固定临时目录。新版隔离测试，并增加从启动路径定位 Linux 可执行文件的回退。

真实设备验证需覆盖 Windows 普通用户的 UASP 移动 SSD、多分区 U 盘、目录挂载、离线或锁定卷、复合设备、占用时移除、Explorer 重启和混合 DPI。Linux 真机需验证多次快速插拔、休眠恢复、内核 uevent、发行版权限与自动挂载时序。模拟数据可验证分支、输入与输出，物理 USB 和 Windows 真机行为仍待验收。

| 验证 | 本次结果 |
| --- | --- |
| GCC 13 严格构建 | Linux 守护进程、X11 助手与测试均通过 `-Wall -Wextra -pedantic -Werror`。 |
| `make selftest` | UI 116 项、Windows 设备枚举与移除逻辑 82 项、核心逻辑 18 项，共 216 项通过。 |
| `tests/daemon_test.py` | glibc 动态版和 musl 静态版各通过 24 项，包括旧快照迁移、失败扫描、同名替换、并发、编码和自启动。 |
| `tools/demo.sh` | 动态版和静态版各通过 18 项，覆盖 Hook、事件日志和自启动回归。 |
| Xvfb | 13 项通过。真实 X11 窗口按预期显示、堆叠并随退出关闭。插入截图包含容量与分区数。 |
| ASan 与 UBSan | 216 项单元断言和 24 项端到端断言均通过。由于环境没有 `/proc`，本轮关闭 LeakSanitizer。 |
| 静态分析 | GCC `-fanalyzer` 严格通过，Cppcheck 2.13 的 warning、performance 和 portability 检查无报告。 |
| Windows x64 | MinGW-w64 GCC 13 交叉构建通过。PE GUI 子系统、资源节和系统 DLL 导入已验证。 |
| Linux 静态产物 | musl 1.2.4 构建通过，ELF 无解释器段。X11 助手的最高 glibc 符号版本为 2.34。 |

Ubuntu Noble 测试工具在隔离目录中解包。验证了 Ubuntu Archive 签名，并用签名索引中的 SHA-256 校验包文件。[Ubuntu 软件包完整性校验](https://documentation.ubuntu.com/security/software-integrity/archive-verification/)

当前环境的内核 uevent 订阅返回 `EPERM`。真实 Linux 热插拔只能在具备设备和权限的机器上验收。Windows 界面图来自模拟渲染，另有真实 X11 截图。[界面预览](USBMonitor-preview.png)
