# 设计规格：usbmon UI —— 溢出修复、相对尺寸、高 DPI、信息框统一

- 日期：2026-10-02
- 状态：用户已逐节批准（含视觉伴侣两轮确认）
- 基线：`purrfecto114-lgtm/USBMonitor` @ main（f9bf404，UM_VERSION 2.4.0）
- 配套实现计划：`docs/superpowers/plans/2026-10-02-usbmon-ui-dpi.md`

## 1. 范围

只改 Windows UI 相关代码：

- `src/gui_win32.c`、`src/um_toast_win32.c`、`src/um_toast_win32.h`、`src/um_toast_ui.c`、`src/usbmon.h`（仅注释）
- `res/usbmon.rc`（保持基线；不内嵌 manifest）
- `Makefile`（`.res.o` 依赖行）
- `tests/win32_shim.h/.c`、`tests/ui_test.c`
- `tools/demo.ps1`（toast 窗口类名断言）、`.github/workflows/ci.yml`（断言数标注）
- `src/usbmon.h` 版本号 2.4.0 → 2.5.0、`CHANGELOG.md`

**不动**：守护进程逻辑（扫描/弹出/日志/hook）、POSIX/X11 路径、托盘菜单实现、
以及 `usbmon_复查_v2_superpowers.md` 所列功能缺陷（安全弹出序列、
`scan_win32.c:95` 零权限 CreateFile、logjson 轮转自愈、um_enum 截断告警、
托盘 TaskbarCreated 兜底、`--no-gui` 唤醒等）——它们属于独立修复轨，本规格
只列出不实现。

## 2. 信息框并入面板内核（决策 A）

文字 toast 成为「0 行面板」：`um_toast_model` 只填 headline/subtitle/summary
（n_rows=0），走现有 `um_toast_layout` / `um_toast_win32` 渲染。圆角、淡入、
主题跟随深浅色、YaHei UI 字体、左色条、底栏倒计时全部由内核免费获得。

文案映射：

| 场景 | headline | subtitle | summary | accent_kind |
|---|---|---|---|---|
| 设备插入（证据层无产出时的兜底） | USB 设备已插入 | 型号 (key) | 容量 … · 盘符 E: · 序列 …（按可用字段拼接，缺省省略） | 1 (ok 绿) |
| 设备拔出 | USB 设备已拔出 | 型号 (key) | （空） | 3 |
| 托盘反馈（弹出结果/自启开关/弹出进度） | title | （空） | body | accent_ok ? 1 : 3 |

交互保留：点窗任意处关闭（n_rows==0 时 hit miss 即 CLOSE）、Esc 关闭、
TTL 自动消失（`g->toast_ttl`，默认 12s；面板仍 10s）、hover 暂停倒计时。
4 个堆叠槽位不变，槽内原位替换语义不变（"正在安全弹出" → 结果），
槽 1..3 相对槽 0 向上偏移 `高度 + 12 逻辑px`（随 DPI/fit 缩放）。

`gui_win32.c` 删除：`toast_data`、旧 `toast_proc`（含整段 GDI WM_PAINT）、
`TW_*` 常数、`g_class_toast` 类注册。`UMWM_TOAST` 改携带堆上
`toast_carrier { um_toast_model model; int slot; int ttl_s; }`
（与 `UMWM_PANEL` 同构的封送协议；ttl_s=0 → 默认 10s）。

`um_toast_win_new` 签名扩展两个参数：`int width`（逻辑像素，0 = 默认
UM_UI_WIDTH=440）与 `int ttl_ms`（0 = 默认 10000）。面板调用传 0/0，行为不变。

## 3. 溢出修复 + 相对尺寸（决策 A：设计空间 × DPI × 工作区钳制）

- 内核保留 440/62/42 等 96DPI 设计空间常数，现有 230/334/430 测试基线不动。
- **DPI 层**：所有物理尺寸 = 设计值 × dpi/96（`ui_scale` 已存在；
  运行期声明 DPI 感知后 `GetDeviceCaps(LOGPIXELSY)` 返回真实值）。
- **工作区层（新增 `fit` 因子）**：每窗口计算
  `fit = min(1.0, 0.70×wa_w/宽_phys, 0.85×wa_h/高_phys)`
  （wa 为光标所在显示器工作区，物理像素；宽/高_phys 为未乘 fit 的物理尺寸）。
  `ui_scale` 乘以 fit，命中测试反除同一因子，字体同因子缩放。
  效果：任何屏幕/DPI 下窗口物理尺寸 ≤ 工作区 70% 宽、85% 高，
  布局等比缩放不重排；信息框的内容宽度（300~440 逻辑px）由
  `um_toast_suggest_width()` 按 headline/subtitle/summary 最长行
  （13px 96dpi YaHei 字体实测 + 2×PAD + 色条 8px）推导，
  其溢出防护并入同一 fit 钳制（不另设独立比例）。
- **内核唯一改动**：`um_toast_measure_height` 的 `min 205` 钳制对
  n_rows==0 不生效 → 0 行信息框高度 = 148（有 status 行时 166），
  不再留 57px 空行区。展开 430 上限等其余规则不变。
- 原文字 toast 缺陷全部随并入内核消失：行 y 非累计压字（内核布局天然累计）、
  长文本无省略号（`DT_END_ELLIPSIS` 已挂在所有文本图元）、
  `SM_CYSCREEN` 锚点贴任务栏（改为显示器工作区锚定）、宽度死写 400。

## 4. 多屏停靠（决策 A：跟随鼠标）

`um_toast_win32.c` 新增 `um_toast_monitor_workarea(RECT *wa)`（公开）与
静态 `monitor_workarea()` 实现：
`GetCursorPos → MonitorFromPoint(MONITOR_DEFAULTTONEAREST) →
MonitorGetInfoW(rcWork)`；任一失败回退 `SPI_GETWORKAREA`，再失败回退
`SM_CXSCREEN/SM_CYSCREEN`。`apply_geometry` 与 toast 创建一律用它取锚定区，
替换现状主屏 `SPI_GETWORKAREA` + `SM_*` 兜底。
顺带删除 `x<0/y<0` 的 0 钳制（多显示器虚拟桌面坐标可为负，
fit 保证窗口不越出所在显示器工作区）。

## 5. 高 DPI（决策 A：PerMonitorV2，运行期声明）

- **不内嵌 manifest**：`res/usbmon.rc` / `Makefile` 保持基线。（原因：mingw
  ld 2.47 对同时含 `.rsrc` 内嵌清单与 CRT 默认清单的 PE 报
  `multiple non-default manifests`；运行期声明与任意链接方式兼容。）
- `um_toast_win32.c` 新增 `um_toast_win_enable_dpi_awareness()`，在
  `um_toast_win_init()` 注册窗口类、创建任何 HWND **之前**调用：按
  `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` →
  `SetProcessDpiAwareness(2)` → `SetProcessDPIAware()` 顺序降级；三个入口
  动态 `GetProcAddress`（Win7/8 仍可启动），用 union 存函数指针规避
  `-Wcast-function-type`。
- 声明成功后 `GetDeviceCaps(LOGPIXELSY)` 返回真实系统 DPI；
  `dpi<96` 不再无脑钳到 96（75dpi 显示器合法缩小），仅保留 `dpi<16 → 96` 防御。
- 面板/信息框窗口过程新增 `WM_DPICHANGED`：
  `w->dpi = HIWORD(wp)`（<16 时取 96）→ 重算 fit → `apply_geometry`
  重锚定 → 全量重画。跨不同缩放屏拖动、运行时改缩放均正确。
- **创建期陷门（2.5.0 实测）**：`toast_proc` 一旦在 `WM_NCCREATE` 阶段因
  `GWLP_USERDATA == NULL` 提前 `return`，会打断 `CreateWindowExW` 的创建
  握手，表现为 `panel create fail`。修法：过程顶部
  `if (!w) return DefWindowProcW(...)`，把 `WM_NCCREATE`/`WM_CREATE` 交给
  默认过程（由其记录 `CREATESTRUCT` 标题），其余消息才走自定义逻辑。
- 托盘菜单为系统原生控件，自动缩放，无需改动。

## 6. 视觉刷新（决策 A：保持配色）

不改主题表。信息框观感提升来自 §2 的统一；面板本体几何不变、零回归风险。

## 7. 测试与验证

- `tests/ui_test.c`：
  - 保留全部既有断言（440/230/334/430、命中、滚动、SVG）；
  - 新增 0 行模型：measure_height==148（无 status）/166（有 status）、
    360 宽度下 关闭 按钮命中、shim 层点空区触发 UM_ACT_CLOSE；
  - 新增 fit 钳制：shim 假工作区缩小（如 500×400）后断言
    SetWindowPos 收到 ≤70%/85% 的尺寸；
  - 新增 DPI 缩放：shim dpi=144 时断言窗口物理宽 660（440×144/96）
    且标题字号 24px。
- `tests/win32_shim.h/.c`：新增 `GetCursorPos`、`MonitorFromPoint`、
  `MonitorGetInfoW`（假主屏 = 可配置工作区）、`HMONITOR`/`MONITORINFOW`
  类型、`shim_set_workarea(w,h)`、`shim_set_dpi(int)`。
- `tools/demo.ps1`：14b 的 toast 类名断言 `usbmonToast` → `usbmonToast2`；
  其余（菜单 dump、`toast create ok slot=` 日志行、panel 几何按
  clientRight/440 比例）不变。
- `ci.yml`：断言数标注按新 ui_test 实际数量更新。
- 本机（Windows，无系统 C 编译器）：安装 mingw-w64 后
  `make CROSS=x86_64-w64-mingw32- windows` 编译全量 exe；真实 exe 的 DPI
  行为按 §5 运行期复验（`%` 缩放变化时窗口物理尺寸 = 设计值 × dpi/96）。
  `ui_test`/`enum_test` 的 shim 在 Linux（LP64）由 CI 运行；在 mingw
  （LLP64）下运行需另加 `UM_USE_SHIM` guard 与 `intptr_t` 改造（本版未做），
  故本机不跑 shim 测试，交由 CI。
- 完成后由独立子代理（oracle）对完整 diff 做终审，修复后推送。

## 8. 风险与缓解

- `um_toast_win_new` 签名变化：调用点仅 gui_win32.c（面板 + toast）与
  ui_test.c，全部随本次改动更新；shim 不涉及。
- fit 取整误差：布局与命中测试共用同一 `ui_scale`/反除因子，
  ±1px 误差不叠加。
- headless CI 下 `GetCursorPos=(0,0)` → 主屏，行为可预期。
- 0 行 toast 创建比旧文本窗口重（完整面板窗口）：创建仍在 GUI 线程消息循环内，
  不阻塞热路径；TTL 后销毁，槽位释放。
- 运行期 DPI 声明必须在创建任何 HWND 之前调用；已落在
  `um_toast_win_init` 的窗口类注册前。重复调用只会失败
  （`SetProcessDpiAwarenessContext` 返回 `ERROR_ACCESS_DENIED`），
  不会降级已有感知，因此 fallback 链是安全的。
