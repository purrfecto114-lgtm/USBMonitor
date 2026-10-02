# usbmon UI —— 溢出修复 / 相对尺寸 / 高 DPI / 信息框统一 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 把 usbmon 的纯文字通知并入设备面板内核（同一套圆角/淡入/主题/省略号渲染），并让面板与通知在任何屏幕与 DPI 下都相对合适：不写死像素、文本永不溢出、不越出所在显示器工作区、跨屏拖动与运行时改缩放都正确。

**架构：** 三层不动边界——`um_toast_ui.c`（平台无关内核，96DPI 设计空间常量）、`um_toast_win32.c`（Win32/GDI 后端，负责 DPI 与工作区）、`gui_win32.c`（宿主线程与封送协议）。本计划只动这三层中与 UI 相关的部分：内核新增"0 行文字模型"语义（信息框），后端新增 `fit` 工作区钳制因子 + `WM_DPICHANGED` + 显示器工作区锚定 + 按文本实测推荐宽度，宿主把旧的 `toast_data` 换成 `um_toast_model` 封送。

**技术栈：** C99（`-Wall -Wextra -pedantic -Werror`）、Win32/GDI、mingw-w64 交叉编译（CI + 本机可选）、`tests/win32_shim.c` 仿真层（Linux/mingw 上跑真·生产代码）、GitHub Actions。

**设计规格（唯一依据）：** `docs/superpowers/specs/2026-10-02-usbmon-ui-dpi-design.md`

---

> **实施修订（2.5.0 落地时的偏差，以此为准）**
>
> 1. **DPI 感知改用运行期声明，不内嵌 manifest。** 原计划的
>    `res/usbmon.manifest` + `res/usbmon.rc` + `Makefile` 三项作废：
>    mingw ld 2.47 对同时含 `.rsrc` 内嵌清单与 CRT 默认清单的 PE 报
>    `multiple non-default manifests`。改为 `um_toast_win32.c` 的
>    `um_toast_win_enable_dpi_awareness()`，在 `um_toast_win_init()` 注册
>    窗口类之前按 PerMonitorV2 → PerMonitor → system-DPI 动态降级声明。
>    下列 manifest 相关步骤（任务 1–3 及阶段二嵌入校验）请忽略，其余有效。
> 2. **新增创建期陷门修正。** `toast_proc` 顶部需
>    `if (!w) return DefWindowProcW(...)`，否则 `WM_NCCREATE` 阶段访存
>    `GWLP_USERDATA==NULL` 会打断创建握手，表现为 `panel create fail`。
> 3. **shim 测试仅在 Linux CI 运行。** mingw（LLP64）下运行 shim 还需
>    `UM_USE_SHIM` guard 与 `intptr_t` 改造，本版未做。
> 4. **槽位叠放补充。** 面板可见时通知整体叠到面板上方
>    （`um_toast_win_move_slot_ex` + `um_toast_win_pixel_height`）。

---

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `res/usbmon.manifest` | 创建 | PerMonitorV2 + `dpiAware=true/pm`：让 `GetDeviceCaps(LOGPIXELSY)` 返回真实 DPI，并收到 `WM_DPICHANGED` |
| `res/usbmon.rc` | 修改 | 追加 `1 24 "usbmon.manifest"`（RT_MANIFEST / CREATEPROCESS_MANIFEST_RESOURCE_ID） |
| `Makefile` | 修改 | `res/usbmon.res.o` 依赖行加上 manifest（改了 manifest 必须重新 windres） |
| `src/um_toast_ui.c` | 修改 | ① `um_toast_measure_height` 的 205 下限只对 `n_rows>0` 生效；② `um_toast_layout` 对 0 行只画倒计时（不画展开/关闭/打开按钮） |
| `tests/win32_shim.h/.c` | 修改 | 仿真 `GetCursorPos` / `MonitorFromPoint` / `MonitorGetInfoW` / `GetWindowRect` / `GetTextExtentPoint32W`，可注入工作区与 DPI，暴露字体创建日志 |
| `tests/ui_test.c` | 修改 | 新增 0 行高度、360 宽命中、点空区关闭、fit 钳制、DPI 缩放与 `WM_DPICHANGED`、`suggest_width` 夹取等断言；全部调用点跟签名 |
| `src/um_toast_win32.h` | 修改 | `um_toast_win_new` 增加 `width` / `ttl_ms`；新增 `um_toast_suggest_width` / `um_toast_win_move_slot` / `um_toast_monitor_workarea` |
| `src/um_toast_win32.c` | 修改 | `fit` 因子、`ui_factor`/`to_logical`、字号同因子缩放、`WM_DPICHANGED`、显示器工作区锚定（删 x/y<0 钳制）、0 行点空区关闭、0 行窗口标题 `usbmon-toast` |
| `src/gui_win32.c` | 修改 | 删除 `toast_data` / 旧 `toast_proc` / `TW_*` / `g_class_toast`（约 120 行）；`UMWM_TOAST` 改携带堆 `toast_carrier`；三个投递入口统一构造 0 行 `um_toast_model` |
| `tools/demo.ps1` | 修改 | 新增 `WindowsByClassTitleAndPid`；14b 的 toast 断言改查 `usbmonToast2` + 标题 `usbmon-toast` |
| `.github/workflows/ci.yml` | 修改 | 断言数标注按实测更新 |
| `src/usbmon.h` | 修改 | `UM_VERSION` 2.4.0 → 2.5.0；`UMWM_TOAST` 注释更新为 `toast_carrier` |
| `CHANGELOG.md` | 修改 | 顶部新增 `[2.5.0]` 条目 |

**不动**：`src/um_enum.c`、`src/scan_win32.c`、`src/tray_win32.c`、Linux 守护进程全部路径、`usbmon_复查_v2_superpowers.md`（审查文档，不入库）。

## 本机验证命令（Windows，无 make）

```powershell
$env:PATH = "$env:PATH;<mingw>\bin"          # winlibs 解压目录
gcc -std=c99 -O2 -Wall -Wextra -pedantic -Werror -Isrc -Itests -o tests\ui_test.exe `
    tests\ui_test.c src\um_toast_ui.c src\um_toast_win32.c tests\win32_shim.c   # 尝试 -U_WIN32
windres res\usbmon.rc -O coff -o res\usbmon.res.o
gcc -std=c99 -O2 -Wall -Wextra -pedantic -Werror -mwindows -static `
    src\main.c src\util.c src\logjson.c src\json.c src\hook.c src\lock.c src\gui.c `
    src\hotpath.c src\scan_win32.c src\gui_win32.c src\tray_win32.c src\um_enum.c `
    src\um_toast_ui.c src\um_toast_win32.c res\usbmon.res.o -o usbmon.exe `
    -luser32 -lgdi32 -lshell32 -ladvapi32 -lcfgmgr32 -lsetupapi
```

---

### 任务 1：PerMonitorV2 manifest

**文件：**
- 创建：`res/usbmon.manifest`
- 修改：`res/usbmon.rc:11`、`Makefile:181`

- [ ] **步骤 1：创建 `res/usbmon.manifest`**

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="usbmon" version="2.5.0.0" processorArchitecture="*"/>
  <description>usbmon - native USB storage monitor</description>
  <!-- PerMonitorV2 (Win10 1703+): GetDeviceCaps(LOGPIXELSY) then reports the real
       DPI of the monitor the window is on, and WM_DPICHANGED arrives when the
       window is dragged across monitors or the scale changes at runtime.
       "true/pm" keeps Win 8.1 on per-monitor DPI instead of system-DPI. -->
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

- [ ] **步骤 2：在 `res/usbmon.rc` 追加 manifest 资源**

在 `1 ICON "usbmon.ico"` 之后追加（并把文件头注释补一行）：

```
//   1 24 "usbmon.manifest" — PerMonitorV2（DPI 感知声明；windres 按
//                         RT_MANIFEST(24) + 资源 id 1
//                         = CREATEPROCESS_MANIFEST_RESOURCE_ID 嵌入 PE .rsrc）

1 ICON "usbmon.ico"
1 24 "usbmon.manifest"
```

- [ ] **步骤 3：`Makefile` 依赖行加 manifest**

把

```make
res/usbmon.res.o: res/usbmon.rc res/usbmon.ico
```

改成

```make
res/usbmon.res.o: res/usbmon.rc res/usbmon.ico res/usbmon.manifest
```

- [ ] **步骤 4：验证（需要任务 8 的 mingw；此步只做 XML 良构检查）**

```powershell
[xml](Get-Content res\usbmon.manifest -Raw) | Out-Null; "manifest xml ok"
```

预期：输出 `manifest xml ok`。

- [ ] **步骤 5：Commit**

```bash
git add res/usbmon.manifest res/usbmon.rc Makefile
git commit -m "feat(windows): PerMonitorV2 DPI manifest so LOGPIXELSY is the real DPI"
```

---

### 任务 2：win32 仿真层扩展（显示器 / DPI / 文本实测 / 字体日志）

**文件：**
- 修改：`tests/win32_shim.h:50-52`（RECT/POINT 之后加类型）、`tests/win32_shim.h:158-203`（原型）
- 修改：`tests/win32_shim.c:166-181`

- [ ] **步骤 1：头文件加类型与常量**

在 `typedef struct { long x, y; } POINT;` 之后追加：

```c
typedef void *HMONITOR;
typedef struct { long cx, cy; } SIZE;
#define MONITOR_DEFAULTTONEAREST 2
typedef struct {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} MONITORINFOW;
#define WM_DPICHANGED 0x02E0
```

- [ ] **步骤 2：头文件加仿真控制与 API 原型**

在 `void    shim_window_geometry(...)` 之后追加：

```c
/* 注入"当前显示器"：工作区尺寸（0,0,w,h）、屏幕尺寸与 DPI。
 * 默认 1920x1080 屏幕 / 1920x1040 工作区（40px 任务栏）/ 96dpi，
 * 与既有断言（1920-440-18 / 1040-430-18）保持一致。 */
void    shim_set_workarea(int w, int h);
void    shim_set_dpi(int dpi);
/* CreateFontW 的调用日志（顺序即 paint_toast 里的 UM_F_* 顺序：
 * 0=标题 16px, 1=正文 13px, 2=小字 12px, 3=按钮 13px, 4=行标题 13px）。 */
int     shim_font_log_count(void);
int     shim_font_log_px(int idx);
```

在 `int      GetDeviceCaps(HDC dc, int idx);` 之后追加：

```c
BOOL     GetCursorPos(POINT *pt);
HMONITOR MonitorFromPoint(POINT pt, DWORD flags);
BOOL     MonitorGetInfoW(HMONITOR mon, MONITORINFOW *mi, DWORD flags);
BOOL     GetWindowRect(HWND hw, RECT *rc);
BOOL     GetTextExtentPoint32W(HDC dc, const uint16_t *text, int len, SIZE *sz);
```

- [ ] **步骤 3：实现仿真状态与 API（`tests/win32_shim.c`）**

在 `static int          g_next_menu_cmd;` 之后追加：

```c
static int g_screen_w = 1920, g_screen_h = 1080;
static int g_work_left = 0, g_work_top = 0;
static int g_work_w = 1920, g_work_h = 1040;
static int g_dpi = 96;
static int g_font_px_log[16], g_font_log_n;
```

在 `void shim_reset(void)` 之后追加：

```c
void shim_set_workarea(int w, int h)
{
    if (w < 240) w = 240;
    if (h < 200) h = 200;
    g_screen_w = g_work_w = w;
    g_screen_h = g_work_h = h;
    g_work_left = g_work_top = 0;
}

void shim_set_dpi(int dpi) { g_dpi = (dpi >= 16) ? dpi : 96; }

int shim_font_log_count(void) { return g_font_log_n; }

int shim_font_log_px(int idx)
{
    return (idx >= 0 && idx < g_font_log_n) ? g_font_px_log[idx] : -1;
}
```

把 `SystemParametersInfoW` 的工作区分支与 `GetSystemMetrics` / `GetDeviceCaps` 换成注入值：

```c
BOOL SystemParametersInfoW(UINT a, UINT b, void *c, UINT d)
{
    (void)b; (void)d;
    if (a == SPI_GETWORKAREA && c) {
        RECT *r = (RECT *)c;
        r->left = g_work_left; r->top = g_work_top;
        r->right = g_work_left + g_work_w;
        r->bottom = g_work_top + g_work_h;
        return 1;
    }
    return 0;
}

int GetSystemMetrics(int i) { return i == SM_CXSCREEN ? g_screen_w : g_screen_h; }
HDC GetDC(HWND hw) { (void)hw; return (HDC)1; }
int ReleaseDC(HWND hw, HDC dc) { (void)hw; (void)dc; return 1; }
int GetDeviceCaps(HDC dc, int idx) { (void)dc; return (idx == LOGPIXELSY) ? g_dpi : 96; }

/* 显示器：只有一个假的"主屏"，工作区可注入。 */
BOOL GetCursorPos(POINT *pt)
{
    if (!pt) return 0;
    pt->x = g_work_left; pt->y = g_work_top;   /* headless: 等价 (0,0) */
    return 1;
}

HMONITOR MonitorFromPoint(POINT pt, DWORD flags)
{
    (void)flags;
    return ((pt.x >= g_work_left && pt.x < g_work_left + g_work_w &&
             pt.y >= g_work_top  && pt.y < g_work_top  + g_work_h)
            ? (HMONITOR)1 : NULL);
}

BOOL MonitorGetInfoW(HMONITOR mon, MONITORINFOW *mi, DWORD flags)
{
    (void)flags;
    if (!mon || !mi || mi->cbSize != (DWORD)sizeof *mi) return 0;
    mi->rcMonitor.left = g_work_left; mi->rcMonitor.top = g_work_top;
    mi->rcMonitor.right = g_work_left + g_screen_w;
    mi->rcMonitor.bottom = g_work_top + g_screen_h;
    mi->rcWork = mi->rcMonitor;
    mi->rcWork.right = g_work_left + g_work_w;
    mi->rcWork.bottom = g_work_top + g_work_h;
    mi->dwFlags = 0;
    return 1;
}

BOOL GetWindowRect(HWND hw, RECT *rc)
{
    (void)hw;
    if (!rc) return 0;
    rc->left = g_win_x; rc->top = g_win_y;
    rc->right = g_win_x + g_win_w; rc->bottom = g_win_y + g_win_h;
    return 1;
}
```

在 `CreateFontW` 实现里记录像素高度（形参 `h` 是负数，取绝对值）：

```c
    if (g_font_log_n < 16) g_font_px_log[g_font_log_n++] = h < 0 ? -h : h;
```

并在 `GetTextExtentPoint32W`（新增，紧跟 `DrawTextW` 之后）里做宽度估算：ASCII 约 0.55×px，CJK/全角约 1.0×px：

```c
int GetTextExtentPoint32W(HDC dc, const uint16_t *text, int len, SIZE *sz)
{
    int i, w = 0;
    (void)dc;
    if (!sz) return 0;
    sz->cy = g_font_px + g_font_px / 3;
    if (text) {
        for (i = 0; i < len && text[i]; i++)
            w += (text[i] >= 0x2E80) ? g_font_px : (g_font_px * 55 / 100);
    }
    sz->cx = w;
    return 1;
}
```

- [ ] **步骤 4：验证（仿真层自身可编译）**

```bash
make strict
```

预期：`tests/ui_test`、`tests/enum_test` 仍能编译（本步只加能力，不改行为）。本机若无 make，见任务 8。

---

### 任务 3：先写失败的测试（TDD 红）

**文件：**
- 修改：`tests/ui_test.c`

- [ ] **步骤 1：更新既有调用点为新签名（此时后端还没改 → 编译必失败）**

`tests/ui_test.c` 里 5 处 `um_toast_win_new(&m, &theme, 1, on_action, NULL)` 全部改为：

```c
    um_toast_win_new(&m, &theme, 1, 0, 0, on_action, NULL)
```

- [ ] **步骤 2：追加新断言区块（放在 `== 15` 之后、`um_toast_win_destroy(w)` 之前）**

```c
    printf("== 16. 0 行信息框：高度不再有 57px 空行区 ==\n");
    {
        um_toast_model m0;
        um_toast_state s;
        memset(&m0, 0, sizeof m0);
        snprintf(m0.headline, sizeof m0.headline, "%s", "USB 设备已拔出");
        snprintf(m0.subtitle, sizeof m0.subtitle, "%s", "Kingston DataTraveler (E:)");
        m0.accent_kind = 3;
        m0.n_rows = 0;
        um_toast_state_init(&s, 10000);
        CHECK(um_toast_measure_height(&m0, &s) == 148);
        snprintf(m0.status, sizeof m0.status, "%s", "正在安全弹出 E:…");
        CHECK(um_toast_measure_height(&m0, &s) == 166);
        m0.status[0] = 0;
        /* 内核几何：推荐宽度夹在 [300, 440]，且随文案变长 */
        {
            um_toast_model m1 = m0;
            int w_min = um_toast_suggest_width(&m0);
            char longline[UM_UI_MAX_TEXT];
            int w_max;
            memset(longline, 'W', sizeof longline - 1);
            longline[sizeof longline - 1] = 0;
            snprintf(m1.summary, sizeof m1.summary, "%s", longline);
            w_max = um_toast_suggest_width(&m1);
            CHECK(w_min >= 300 && w_min <= UM_UI_WIDTH);
            CHECK(w_max == UM_UI_WIDTH);
            CHECK(w_max > w_min);
        }
    }

    printf("== 17. 0 行信息框：窄宽可点、点空区即关闭 ==\n");
    {
        um_toast_model m0;
        um_toast_win *w2;
        int gx, gy, gw, gh;
        memset(&m0, 0, sizeof m0);
        snprintf(m0.headline, sizeof m0.headline, "%s", "USB 设备已拔出");
        snprintf(m0.subtitle, sizeof m0.subtitle, "%s", "SanDisk Ultra (Q:\\)");
        m0.accent_kind = 3;
        w2 = um_toast_win_new(&m0, &theme, 1, 360, 0, on_action, NULL);
        CHECK(w2 != NULL);
        um_toast_win_show(w2);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 360 && gh == 148);
        /* 0 行模型没有"关闭"按钮：命中测试任何位置都是 NONE → 整窗即关闭区 */
        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(40 | (110 << 16)));
        CHECK(g_last_act == UM_ACT_CLOSE);
        /* 有行的面板：点空区仍然什么都不做（不回归） */
        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)((5) | (5 << 16)));
        CHECK(g_last_act == UM_ACT_CLOSE);   /* 此时窗口已被上一条销毁，丢弃即可 */
        um_toast_win_destroy(w2);
    }

    printf("== 18. 工作区钳制：物理尺寸 ≤ 70% 宽 / 85% 高（不写死像素）==\n");
    {
        um_toast_win *w2 = um_toast_win_new(&m, &theme, 1, 0, 0,
                                            on_action, NULL);
        int gx, gy, gw, gh;
        CHECK(w2 != NULL);
        um_toast_win_show(w2);
        shim_set_workarea(500, 400);              /* 小屏：440x230 装不下 */
        um_toast_win_show(w2);                    /* 每次展示都重算 fit + 重锚 */
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw <= 350 && gh <= 340);            /* 0.70*500 / 0.85*400 */
        CHECK(gw < 440 && gh < 230);
        CHECK(gx >= 0 && gy >= 0);
        /* 命中测试跟着 fit 走：放大坐标命中同一个"打开U盘"按钮 */
        CHECK(um_toast_hit_test(&m, um_toast_win_state(w2), 440, 230,
                                (440 - UM_UI_PAD - 59), (230 - UM_UI_PAD - 21))
              == UM_HIT_OPEN);
        shim_set_workarea(1920, 1040);
        um_toast_win_show(w2);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 440 && gh == 230);            /* 大屏：回到设计尺寸 */
        CHECK(gx == 1920 - 440 - 18 && gy == 1040 - 230 - 18);
        um_toast_win_destroy(w2);
    }

    printf("== 19. 高 DPI：144dpi 物理尺寸与字号整体放大，WM_DPICHANGED 实时跟随 ==\n");
    {
        um_toast_win *w2 = um_toast_win_new(&m, &theme, 1, 0, 0,
                                            on_action, NULL);
        int gx, gy, gw, gh;
        CHECK(w2 != NULL);
        shim_set_dpi(144);
        um_toast_win_show(w2);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 660 && gh == 345);            /* 440x230 × 144/96 */
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(shim_font_log_px(0) == 24);         /* 标题 16px × 1.5 */
        /* 拖到另一块 100% 缩放的屏：系统发 WM_DPICHANGED */
        shim_send(TEST_HWND, WM_DPICHANGED, (WPARAM)((96 << 16) | 96), 0);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 440 && gh == 230);
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(shim_font_log_px(0) == 16);
        /* 低 dpi（75% 缩放）合法缩小，不再被钳到 96 */
        shim_send(TEST_HWND, WM_DPICHANGED, (WPARAM)((72 << 16) | 72), 0);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 330 && gh == 172);            /* 440x230 × 72/96 */
        shim_set_dpi(96);
        um_toast_win_destroy(w2);
    }

    printf("== 20. 槽位偏移：move_slot 按 fit 后的高度叠放 ==\n");
    {
        um_toast_win *w2 = um_toast_win_new(&m, &theme, 1, 0, 0,
                                            on_action, NULL);
        int gx0, gy0, gx1, gy1, gw, gh;
        um_toast_win_show(w2);
        shim_window_geometry(&gx0, &gy0, &gw, &gh);
        um_toast_win_move_slot(w2, 1);
        shim_window_geometry(&gx1, &gy1, &gw, &gh);
        CHECK(gx1 == gx0);
        CHECK(gy1 == gy0 - (230 + 12));
        um_toast_win_move_slot(w2, 0);            /* 槽 0 = 不偏移 */
        shim_window_geometry(&gx1, &gy1, &gw, &gh);
        CHECK(gx1 == gx0 && gy1 == gy0);
        um_toast_win_destroy(w2);
    }
```

- [ ] **步骤 3：运行测试验证失败**

```bash
make strict && make selftest
```

预期：编译失败，报 `implicit declaration of function 'um_toast_suggest_width'` / `'um_toast_win_move_slot'`，以及 `too few arguments to function 'um_toast_win_new'`。这就是"红"。

---

### 任务 4：内核 —— 0 行高度门控 + 0 行底栏

**文件：**
- 修改：`src/um_toast_ui.c:203-210`（measure_height）、`src/um_toast_ui.c:323-360`（底栏）

- [ ] **步骤 1：`um_toast_measure_height` 的 205 下限只对有行的模型生效**

把

```c
    h += rows_h;
    /* 与 1.1.1 一致：min(max(自然高度, 205), 目标高度) */
    {
        int target = s->expanded ? UM_UI_EXPANDED_H : UM_UI_COLLAPSED_H;
        if (h < 205) h = 205;
        if (h > target) h = target;
    }
    return h;
```

改成

```c
    h += rows_h;
    /* 与 1.1.1 一致：min(max(自然高度, 205), 目标高度)。
     * 0 行（纯文字通知/信息框）例外：205 的下限是"至少放得下一行设备"的
     * 经验值，对文字通知只会留下 57px 空行区 —— 按自然高度收缩。 */
    {
        int target = s->expanded ? UM_UI_EXPANDED_H : UM_UI_COLLAPSED_H;
        if (m->n_rows > 0 && h < 205) h = 205;
        if (h > target) h = target;
    }
    return h;
```

- [ ] **步骤 2：`um_toast_layout` 对 0 行只画倒计时**

在 `/* ---- 底栏按钮 ---- */` 之前插入：

```c
    /* ---- 0 行 = 纯文字信息框 ---- */
    if (m->n_rows == 0) {
        /* 不画 展开/关闭/打开：整窗点击即关闭（后端在 hit miss 时发 CLOSE），
         * Esc 亦可。底部只留倒计时，于是宽度可以按文案实测收窄到 300，
         * 不会出现倒计时压住"关闭"按钮的排版。 */
        char cd[64];
        if (s->paused) snprintf(cd, sizeof cd, "%s", "已暂停");
        else if (m->status[0]) snprintf(cd, sizeof cd, "%s", "");
        else um_ui_countdown(s->remaining_ms, cd, sizeof cd);
        push_text(out, p, btn_y + 13, width - p * 2, UM_F_SMALL, t->muted,
                  UM_A_LEFT, cd);
        return;
    }
```

（原来的底栏代码保持不变，被 `if (m->n_rows == 0) return;` 提前短路。）

- [ ] **步骤 3：验证内核单测（此时仍编译失败，预期：只剩后端 API 缺失）**

```bash
make strict 2>&1 | grep -E 'error' | head
```

预期：只报 `um_toast_suggest_width` / `um_toast_win_move_slot` / `um_toast_win_new` 参数个数错误；不再报 `um_toast_layout` 相关错误。

---

### 任务 5：后端 —— fit 钳制 / DPI / 多屏 / 槽位 / 推荐宽度

**文件：**
- 修改：`src/um_toast_win32.h:27-60`
- 修改：`src/um_toast_win32.c:48-61`（结构体）、`76-79`（ui_scale）、`180-201`（paint 字体）、`284-311`（apply_geometry）、`347-351`（on_click）、`428-503`（鼠标坐标）、`597-641`（um_toast_win_new）、`551+`（新增公开函数）

- [ ] **步骤 1：结构体加 `fit` 字段**

```c
struct um_toast_win {
    HWND             hwnd;
    um_toast_model   model;
    um_toast_state   state;
    um_theme         theme;
    int              width, height;     /* 逻辑像素（96DPI 设计空间） */
    int              dpi;
    double           fit;               /* 工作区钳制因子（0.05..1.0） */
    ...
};
```

- [ ] **步骤 2：统一的缩放因子 + 物理→逻辑换算**

把 `ui_scale` 替换为：

```c
/* 物理像素 = 设计空间值 × (dpi/96) × fit。布局、字体、圆角、命中测试
 * 共用同一个因子，因此缩放不会让布局错位。 */
static double ui_factor(const struct um_toast_win *w)
{
    double f = w->dpi / 96.0;
    if (w->fit > 0.01 && w->fit < 1.0) f *= w->fit;
    return f;
}

static int ui_scale(const struct um_toast_win *w, int v)
{
    return (int)(v * ui_factor(w) + 0.5);
}

/* 物理 → 逻辑（鼠标坐标）：与 ui_scale 同一个因子，取整误差 ±1px 不会叠加。 */
static int to_logical(const struct um_toast_win *w, int phys)
{
    double f = ui_factor(w);
    return (f < 0.01) ? phys : (int)(phys / f);
}

/* 字号用同一因子：等价于 make_font(px, dpi*fit) */
static int ui_font_dpi(const struct um_toast_win *w)
{
    int v = (int)(ui_factor(w) * 96.0 + 0.5);
    return (v < 16) ? 96 : v;
}
```

- [ ] **步骤 3：`paint_toast` 的 5 处字号改用 `ui_font_dpi(w)`**

```c
    fonts[UM_F_TITLE]    = make_font(16, ui_font_dpi(w), 1, font_face());
    fonts[UM_F_BODY]     = make_font(13, ui_font_dpi(w), 0, font_face());
    fonts[UM_F_SMALL]    = make_font(12, ui_font_dpi(w), 0, font_face());
    fonts[UM_F_BTN]      = make_font(13, ui_font_dpi(w), 1, font_face());
    fonts[UM_F_ROWTITLE] = make_font(13, ui_font_dpi(w), 1, font_face());
```

- [ ] **步骤 4：显示器工作区 + `fit` 钳制的新 `apply_geometry`**

```c
/* 锚定区 = 光标所在显示器的工作区：多屏 + 负坐标虚拟桌面都正确。
 * 依次回退 光标所在屏 rcWork → SPI_GETWORKAREA（主屏）→ SM_CX/CYSCREEN。 */
static void monitor_workarea(RECT *wa)
{
    POINT pt;

    memset(wa, 0, sizeof *wa);
    if (GetCursorPos(&pt)) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (mon) {
            MONITORINFOW mi;
            mi.cbSize = sizeof mi;
            if (MonitorGetInfoW(mon, &mi)) { *wa = mi.rcWork; return; }
        }
    }
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, wa, 0)) return;
    wa->left = 0; wa->top = 0;
    wa->right  = GetSystemMetrics(SM_CXSCREEN);
    wa->bottom = GetSystemMetrics(SM_CYSCREEN);
}

static void apply_geometry(struct um_toast_win *w)
{
    RECT wa;
    int  pw, ph, x, y;

    w->height = um_toast_measure_height(&w->model, &w->state);
    monitor_workarea(&wa);

    /* fit：把窗口等比钳进工作区（≤70% 宽、≤85% 高）。设计空间布局不变，
     * 只是整体缩小 —— 任何分辨率/缩放下都不会超出所在显示器，也不需要
     * 为"大屏/小屏"写两套常量。 */
    pw = (int)(w->width  * w->dpi / 96.0 + 0.5);
    ph = (int)(w->height * w->dpi / 96.0 + 0.5);
    {
        double aw = (double)(wa.right - wa.left);
        double ah = (double)(wa.bottom - wa.top);
        double fit = 1.0;
        if (aw > 0.0 && pw > 0) { double f = 0.70 * aw / pw; if (f < fit) fit = f; }
        if (ah > 0.0 && ph > 0) { double f = 0.85 * ah / ph; if (f < fit) fit = f; }
        if (fit < 0.05) fit = 0.05;      /* 极小工作区兜底，别把窗口缩没了 */
        w->fit = fit;
    }

    pw = ui_scale(w, w->width);
    ph = ui_scale(w, w->height);
    x = wa.right  - pw - ui_scale(w, UM_UI_MARGIN);
    y = wa.bottom - ph - ui_scale(w, UM_UI_MARGIN);
    /* 不再钳 x/y 到 0：多屏虚拟桌面坐标可以为负（副屏在左侧/上方），
     * fit 已保证窗口落在所在显示器的工作区内。 */
    SetWindowPos(w->hwnd, NULL, x, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, pw + 1, ph + 1,
                                      ui_scale(w, 20), ui_scale(w, 20));
        SetWindowRgn(w->hwnd, rgn, FALSE);
    }
}
```

（原 `apply_geometry` 里 `if (x < 0) x = 0; if (y < 0) y = 0;` 两行删除。）

- [ ] **步骤 5：0 行信息框点空区即关闭**

`on_click` 的开头改成：

```c
static void on_click(struct um_toast_win *w, int x, int y)
{
    um_ui_hit hit = um_toast_hit_test(&w->model, &w->state,
                                      w->width, w->height, x, y);
    if (hit == UM_HIT_NONE) {
        /* 0 行 = 纯文字信息框：整窗即关闭区（保持 2.4.0 文本 toast
         * "点任意处消失"的语义）。有行的面板点空区仍然什么都不做。 */
        if (w->model.n_rows == 0) fire(w, UM_ACT_CLOSE, -1);
        return;
    }
```

- [ ] **步骤 6：鼠标坐标统一走 `to_logical`**

- `WM_MOUSEMOVE`：
  ```c
        hit = um_toast_hit_test(&w->model, &w->state, w->width, w->height,
                                to_logical(w, x), to_logical(w, y));
  ```
- `WM_LBUTTONUP`：
  ```c
        on_click(w, to_logical(w, (int)(short)LOWORD(lp)),
                 to_logical(w, (int)(short)HIWORD(lp)));
  ```
- `WM_RBUTTONUP`：
  ```c
        int x = to_logical(w, (int)(short)LOWORD(lp));
        int y = to_logical(w, (int)(short)HIWORD(lp));
  ```

- [ ] **步骤 7：新增 `WM_DPICHANGED` 处理（紧跟 `WM_ERASEBKGND` 之后）**

```c
    case WM_DPICHANGED: {
        /* manifest 声明 PerMonitorV2 后，跨屏拖动 / 运行时改缩放都会走到这里。
         * 采纳新 DPI（而不是 wParam 里建议的矩形 —— 我们自己锚定）。 */
        int dpi = (int)HIWORD(wp);
        if (w) {
            if (dpi < 16) dpi = 96;       /* 防御；<96 合法（75% 缩放） */
            w->dpi = dpi;
            apply_geometry(w);             /* 重算 fit + 重锚 + 改真实窗口尺寸 */
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        break;
    }
```

- [ ] **步骤 8：`um_toast_win_new` 增加 `width` / `ttl_ms` 参数**

头文件：

```c
/* 创建（不显示）：model 拷贝进窗口，theme 同理。
 * width 为逻辑像素宽（0 = 默认 UM_UI_WIDTH）；ttl_ms 为倒计时（0 = 默认 10s）。 */
um_toast_win *um_toast_win_new(const um_toast_model *m, const um_theme *t,
                               int topmost, int width, int ttl_ms,
                               um_toast_cb cb, void *user);
```

实现里：

```c
um_toast_win *um_toast_win_new(const um_toast_model *m, const um_theme *t,
                               int topmost, int width, int ttl_ms,
                               um_toast_cb cb, void *user)
{
    struct um_toast_win *w = (struct um_toast_win *)calloc(1, sizeof *w);
    ...
    if (!w) return NULL;
    w->model = *m;
    w->theme = *t;
    w->topmost = topmost;
    w->cb = cb;
    w->user = user;
    w->fit = 1.0;                                  /* calloc 给的是 0，必须显式初始化 */
    um_toast_state_init(&w->state, ttl_ms > 0 ? ttl_ms : 10000);
    w->width  = (width > 0) ? width : UM_UI_WIDTH;
    w->height = um_toast_measure_height(&w->model, &w->state);

    hdc = GetDC(NULL);
    w->dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (w->dpi < 16) w->dpi = 96;                  /* 不再钳到 96：低 dpi 合法 */
    ReleaseDC(NULL, hdc);
    ...
    w->hwnd = CreateWindowExW(ex, g_class,
                              (m->n_rows > 0) ? L"usbmon" : L"usbmon-toast",
                              WS_POPUP, 0, 0, pw, ph, NULL, NULL,
                              g_hinst, NULL);
```

- [ ] **步骤 9：新增三个公开函数（放在 `um_toast_win_toggle_expand` 之后）**

```c
/* 按文本实测推导信息框宽度：headline / subtitle / summary 里最长的一行
 * （13px 96dpi 字体）+ 2×内边距 + 左色条，夹在 [300, UM_UI_WIDTH]。
 * 溢出防护交给 apply_geometry 的 fit 钳制统一处理。 */
int um_toast_suggest_width(const um_toast_model *m)
{
    const char *lines[3];
    uint16_t    buf[UM_UI_MAX_TEXT];
    HDC  dc;
    HFONT f, old;
    SIZE sz;
    int  maxw = 0, w, i;

    if (!m) return UM_UI_WIDTH;
    lines[0] = m->headline;
    lines[1] = m->subtitle;
    lines[2] = m->summary;
    dc = GetDC(NULL);
    if (!dc) return 300;
    f = make_font(13, 96, 0, font_face());     /* 96dpi 基准，与 fit 无关 */
    old = (HFONT)SelectObject(dc, f);
    for (i = 0; i < 3; i++) {
        int n;
        if (!lines[i] || !lines[i][0]) continue;
        n = um_ui_utf8_to_utf16(lines[i], buf, UM_UI_MAX_TEXT - 1);
        buf[n] = 0;
        if (GetTextExtentPoint32W(dc, buf, n, &sz) && (int)sz.cx > maxw)
            maxw = (int)sz.cx;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(NULL, dc);

    w = maxw + 2 * UM_UI_PAD + 8;               /* 内边距 + 左色条 */
    if (w < 300) w = 300;
    if (w > UM_UI_WIDTH) w = UM_UI_WIDTH;
    return w;
}

/* 槽位偏移：槽 n（1..N-1）相对槽 0 向上叠放（高度 + 12 逻辑 px，
 * 都按 fit 缩放）。槽 0 = 不偏移。 */
void um_toast_win_move_slot(um_toast_win *w, int slot)
{
    RECT rc;
    int  off;

    if (!w || !w->hwnd || slot <= 0) return;
    if (!GetWindowRect(w->hwnd, &rc)) return;
    off = slot * (ui_scale(w, w->height) + ui_scale(w, 12));
    SetWindowPos(w->hwnd, NULL, rc.left, rc.bottom - off, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 导出当前锚定显示器的工作区（RECT 由调用方提供；参数用 void* 以免本头
 * 文件引入 windows.h）。返回 1 表示成功。 */
int um_toast_monitor_workarea(void *rc_work)
{
    RECT wa;
    if (!rc_work) return 0;
    monitor_workarea(&wa);
    *(RECT *)rc_work = wa;
    return 1;
}
```

头文件对应声明：

```c
/* 信息框推荐宽度（逻辑像素，夹在 [300, UM_UI_WIDTH]）。 */
int  um_toast_suggest_width(const um_toast_model *m);
/* 槽位叠放：slot<=0 不偏移。 */
void um_toast_win_move_slot(um_toast_win *w, int slot);
/* 当前锚定显示器的工作区（RECT*，避免在头文件里引入 windows.h）。 */
int  um_toast_monitor_workarea(void *rc_work);
```

- [ ] **步骤 10：运行测试验证通过**

```bash
make strict && make selftest
```

预期：全部 `ok`，结尾 `N checks, 0 failures`（N ≥ 180）。

---

### 任务 6：宿主 —— 文字通知并入面板内核

**文件：**
- 修改：`src/gui_win32.c:22-25`（文件头注释）、`53-93`（调色板/类型/槽位）、`283-431`（helpers + 旧 toast_proc）、`458-549`（gui_thread_main 里的类注册与 UMWM_TOAST）、`617-732`（三个投递入口）

- [ ] **步骤 1：删掉旧的文字 toast 实现**

整段删除：`TW_*` 宏（`#define TW_BG` … `#define TW_SLOT_GAP`）、`toast_data` 结构、
`g_class_toast`、`toast_data_make`、`toast_proc`、`wcopy`，以及
`gui_thread_main` 里注册旧类的：

```c
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = toast_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = g_class_toast;
    at = RegisterClassW(&wc);
    (void)at;
```

同时把 `ATOM at, al;` 改成 `ATOM al;`，并删掉 `#include <wchar.h>`。

- [ ] **步骤 2：槽位改存内核窗口 + 封送结构**

把 `static HWND g_slot_win[UM_GUI_SLOTS];` 换成：

```c
/* 槽位 → 内核信息框（0 行 um_toast_model）。GUI 线程独占：新通知进已占用的
 * 槽位就原位替换（异步弹出的"正在安全弹出"升级为最终结果不叠窗）。 */
static um_toast_win *g_slot_win[UM_GUI_SLOTS];

/* UMWM_TOAST 的封送信封（与 UMWM_PANEL 同构：守护/工作线程建堆对象，
 * GUI 线程消费后释放）。ttl_s=0 → 后端默认 10s。 */
typedef struct {
    um_toast_model model;
    int            slot;
    int            ttl_s;
} toast_carrier;
```

- [ ] **步骤 3：新增模型构造 + 回调（替换 `toast_data_make` 所在位置）**

```c
/* 0 行 um_toast_model = 纯文字信息框：headline/subtitle/summary 三个字段
 * 就是全部内容，走面板内核的圆角/淡入/主题/省略号渲染。 */
static void toast_model_init(um_toast_model *m, const char *headline,
                             const char *subtitle, const char *summary,
                             int accent_kind)
{
    memset(m, 0, sizeof *m);
    snprintf(m->headline, sizeof m->headline, "%s", headline ? headline : "");
    snprintf(m->subtitle, sizeof m->subtitle, "%s", subtitle ? subtitle : "");
    snprintf(m->summary,  sizeof m->summary,  "%s", summary  ? summary  : "");
    m->accent_kind = accent_kind;
    m->n_rows = 0;
}

/* 设备插拔的兜底信息框（证据层没有产出时的 fallback，语义与 2.4.0 的
 * 文本 toast 一致：标题 + 型号(key) + 容量/挂载点/序列，按可用字段拼接）。 */
static void toast_model_device(um_toast_model *m, const um_device *dev,
                               int is_add, um_gui *g)
{
    char buf[512] = "";
    char what[192];
    const char *serial;

    snprintf(what, sizeof what, "%s",
             dev->model[0] ? dev->model
                            : (dev->key[0] ? dev->key : "USB 存储设备"));
    toast_model_init(m, is_add ? "USB 设备已插入" : "USB 设备已拔出",
                     what, "", is_add ? 1 : 3);
    if (!is_add) return;
    if (dev->model[0] && dev->key[0])
        snprintf(m->subtitle, sizeof m->subtitle, "%s (%s)", dev->model, dev->key);
    if (dev->size_bytes > 0) {
        char sz[32], part[96];
        um_human_size(dev->size_bytes, sz, sizeof sz);
        if (dev->partition_count > 0)
            snprintf(part, sizeof part, "容量 %s · %d 个分区", sz,
                     dev->partition_count);
        else
            snprintf(part, sizeof part, "容量 %s", sz);
        strncat(buf, part, sizeof buf - strlen(buf) - 1);
    }
    {
        char part[192];
        snprintf(part, sizeof part, "挂载点 %s",
                 dev->mount[0] ? dev->mount : "未挂载");
        if (buf[0]) strncat(buf, " · ", sizeof buf - strlen(buf) - 1);
        strncat(buf, part, sizeof buf - strlen(buf) - 1);
    }
    serial = (g->raw_serial && dev->serial[0]) ? dev->serial : dev->serial_fp;
    if (serial[0]) {
        char part[192];
        snprintf(part, sizeof part, "序列 %s", serial);
        if (buf[0]) strncat(buf, " · ", sizeof buf - strlen(buf) - 1);
        strncat(buf, part, sizeof buf - strlen(buf) - 1);
    }
    snprintf(m->summary, sizeof m->summary, "%s", buf);
}

/* 信息框动作：只有"关闭"与"到点自动消失"会回传，两者都等于销毁 + 释放信封。 */
static void toast_action_cb(um_toast_action act, int row, void *user)
{
    toast_carrier *c = (toast_carrier *)user;
    (void)row;
    if (!c) return;
    if (act == UM_ACT_CLOSE || act == UM_ACT_AUTOHIDE) {
        if (c->slot >= 0 && c->slot < UM_GUI_SLOTS && g_slot_win[c->slot]) {
            um_toast_win_destroy(g_slot_win[c->slot]);
            g_slot_win[c->slot] = NULL;
        }
        free(c);
    }
}
```

- [ ] **步骤 4：`UMWM_TOAST` 分支改用内核窗口**

把整个 `if (msg.message == UMWM_TOAST) { ... }` 块替换为：

```c
        if (msg.message == UMWM_TOAST) {
            /* 堆上的 toast_carrier（0 行 um_toast_model）：槽内原位替换 →
             * 内核窗口 → show → 按槽位向上叠放。 */
            toast_carrier *c = (toast_carrier *)msg.lParam;
            if (c) {
                um_theme th;
                int slot = c->slot;
                if (slot >= 0 && slot < UM_GUI_SLOTS && g_slot_win[slot]) {
                    um_toast_win_destroy(g_slot_win[slot]);
                    g_slot_win[slot] = NULL;
                }
                um_theme_resolve(&th, "auto", um_toast_system_dark());
                {
                    um_toast_win *tw = um_toast_win_new(
                        &c->model, &th, 1,
                        um_toast_suggest_width(&c->model),
                        c->ttl_s > 0 ? c->ttl_s * 1000 : 0,
                        toast_action_cb, c);
                    if (tw) {
                        if (slot >= 0 && slot < UM_GUI_SLOTS)
                            g_slot_win[slot] = tw;
                        um_toast_win_show(tw);
                        um_toast_win_move_slot(tw, slot);
                        {
                            char dbg[48];
                            snprintf(dbg, sizeof dbg, "create ok slot=%d", slot);
                            um_tray_test_log("toast", dbg);
                        }
                    } else {
                        char dbg[64];
                        snprintf(dbg, sizeof dbg, "create fail gle=%lu slot=%d",
                                 (unsigned long)GetLastError(), slot);
                        um_tray_test_log("toast", dbg);
                        free(c);
                    }
                }
            }
            continue;
        }
```

- [ ] **步骤 5：三个投递入口改构造 `toast_carrier`**

```c
/* Fallback/feedback text toast：证据层没有产出时（以及托盘动作反馈），
 * 现在也是 0 行面板 —— 同一套渲染内核。 */
static void text_toast_post(um_gui *g, const um_device *dev, int is_add,
                            int slot, int ttl_s)
{
    toast_carrier *c = (toast_carrier *)malloc(sizeof *c);
    if (!c) return;
    toast_model_device(&c->model, dev, is_add, g);
    c->slot = (slot >= 0 && slot < UM_GUI_SLOTS) ? slot
                                                 : (g->slot_seq % UM_GUI_SLOTS);
    g->slot_seq++;
    c->ttl_s = (ttl_s > 0) ? ttl_s : g->toast_ttl;
    if (!PostThreadMessageW(g->gui_tid, UMWM_TOAST, (WPARAM)is_add,
                            (LPARAM)c))
        free(c);                     /* GUI 线程已退出：安静丢弃 */
}
```

`um_gui_win_show` 的兜底调用改为
`text_toast_post(g, dev, is_add, -1, 0);`，
并删除原来对 `um_gui_win_notify` 的旧实现体，替换为：

```c
/* Arbitrary-text toast (tray action feedback: eject result, startup
 * toggle).  0 行面板：headline=title, summary=body。 */
void um_gui_win_notify(um_gui *g, const char *title, const char *body,
                       int accent_ok)
{
    toast_carrier *c = (toast_carrier *)malloc(sizeof *c);
    if (!c) return;
    toast_model_init(&c->model, title, "", body, accent_ok ? 1 : 3);
    c->slot = g->slot_seq % UM_GUI_SLOTS;
    g->slot_seq++;
    c->ttl_s = g->toast_ttl;
    if (!PostThreadMessageW(g->gui_tid, UMWM_TOAST, 1, (LPARAM)c))
        free(c);
}

/* Thread-safe variant with EXPLICIT slot and ttl（异步弹出的工作线程用）：
 * 只跨线程传不可变的 gui_tid，不读任何 um_gui 状态。 */
void um_gui_win_post(unsigned long gui_tid, const char *title,
                     const char *body, int accent_ok, int slot, int ttl)
{
    toast_carrier *c = (toast_carrier *)malloc(sizeof *c);
    if (!c) return;
    if (slot < 0 || slot >= UM_GUI_SLOTS || ttl < 1) { free(c); return; }
    toast_model_init(&c->model, title, "", body, accent_ok ? 1 : 3);
    c->slot = slot;
    c->ttl_s = ttl;
    if (!PostThreadMessageW((DWORD)gui_tid, UMWM_TOAST, 1, (LPARAM)c)) {
        um_tray_test_log("toast", "post fail");
        free(c);
    }
}
```

- [ ] **步骤 6：更新文件头注释里的"文字 toast"描述**

把

```
 *   - top-level TEXT toast windows (WS_POPUP | WS_EX_TOPMOST | tool
 *     window, class "usbmonToast"): tray action feedback (eject result,
 *     startup toggle) and the fallback when the evidence layer yields
 *     nothing, so an event is never silently dropped.
```

改为

```
 *   - 0-row NOTIFICATION panels (same kernel, title "usbmon-toast"):
 *     tray action feedback (eject result, startup toggle) and the fallback
 *     when the evidence layer yields nothing, so an event is never
 *     silently dropped.  Since 2.5.0 these are the very same window class
 *     and draw path as the device panel ("usbmonToast2") — one renderer,
 *     so text can no longer overflow, overlap or ignore DPI.
```

- [ ] **步骤 7：验证（Windows 全量编译，本机 mingw 见任务 8）**

```bash
make CROSS=x86_64-w64-mingw32- windows
```

预期：`-Werror` 下零告警，产出 `usbmon.exe`。

---

### 任务 7：demo.ps1 / CI 标注 / 版本 / CHANGELOG

**文件：**
- 修改：`tools/demo.ps1:298-311`（新增 helper）、`629-637`（14b 断言）
- 修改：`.github/workflows/ci.yml:35`
- 修改：`src/usbmon.h:26`
- 修改：`CHANGELOG.md`

- [ ] **步骤 1：demo.ps1 新增"类名 + 标题 + pid"三条件查询**

在 `WindowsByClassAndPid` 方法之后（`[DllImport("user32.dll", SetLastError = true)] PostMessageW` 之前）插入：

```csharp
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
        // 2.5.0: text toasts are 0-row panels of the SAME window class as the
        // device panel ("usbmonToast2"), distinguished by window title
        // ("usbmon" = panel, "usbmon-toast" = notification).
        public static List<long> WindowsByClassTitleAndPid(string className, string title, uint pid) {
            var hwnds = new List<long>();
            EnumWindows(delegate(IntPtr h, IntPtr l) {
                var cb = new StringBuilder(64);
                var tb = new StringBuilder(128);
                GetClassName(h, cb, 64);
                GetWindowTextW(h, tb, 128);
                if (cb.ToString() == className && tb.ToString() == title) {
                    uint wpid;
                    GetWindowThreadProcessId(h, out wpid);
                    if (wpid == pid) hwnds.Add(h.ToInt64());
                }
                return true;
            }, IntPtr.Zero);
            return hwnds;
        }
```

- [ ] **步骤 2：14b 的 toast 断言改查新类名 + 标题**

```powershell
    $toastWnds = [UsbmonDemo.Win32]::WindowsByClassTitleAndPid("usbmonToast2", "usbmon-toast", [uint32]$proc2.Id)
    if ($toastWnds.Count -gt 0) {
        Ok "eject progress/result toast window exists (class usbmonToast2 + title usbmon-toast, pid $($proc2.Id))"
    } else {
        # surface the C-side toast diagnostics (um_tray_test_log lines) so
        # CI failures are diagnosable from the run log alone
        $toastDiag = (Read-TrayLog) -split "`n" | Where-Object { $_ -match '^toast ' } | Select-Object -Last 4
        Bad "no usbmon-toast window (pid $($proc2.Id)) after the eject test; C diagnostics: $($toastDiag -join ' | ')"
    }
```

- [ ] **步骤 3：CI 标注按实测断言数更新**

`make selftest` 会打印两个测试的断言数。把 `.github/workflows/ci.yml:35` 的
`Panel UI kernel + evidence layer tests (164 assertions, win32_shim)` 改成实测值
（例如 `(190 assertions, win32_shim)`）。

- [ ] **步骤 4：版本号 2.4.0 → 2.5.0 + `UMWM_TOAST` 注释**

```c
#define UM_VERSION "2.5.0"
```

并把 `UMWM_TOAST` 的说明从 "arbitrary-text toast" 改为携带堆 `toast_carrier`
（0 行 `um_toast_model` + slot + ttl）。

- [ ] **步骤 5：CHANGELOG 顶部新增 `[2.5.0]` 条目**

在 `## [2.4.0]` 之前插入 `## [2.5.0] — 2026-10-02` 小节，四条要点：
信息框并入面板内核（0 行模型）、PerMonitorV2 + `WM_DPICHANGED`、
设计空间 × DPI × 工作区钳制的相对尺寸、多屏跟随光标的工作区锚定。

- [ ] **步骤 6：Commit**

```bash
git add tools/demo.ps1 .github/workflows/ci.yml src/usbmon.h CHANGELOG.md src/gui_win32.c
git commit -m "feat(ui): text toasts reuse the panel kernel; PerMonitorV2 + work-area clamped relative sizing"
```

---

### 任务 8：验证、审查、推送与发布

- [ ] **步骤 1：本机编译 Windows exe（mingw-w64）**

```powershell
$env:PATH = "$env:PATH;<winlibs>\bin"
windres res\usbmon.rc -O coff -o res\usbmon.res.o
gcc -std=c99 -O2 -Wall -Wextra -pedantic -Werror -mwindows -static `
  src\main.c src\util.c src\logjson.c src\json.c src\hook.c src\lock.c src\gui.c `
  src\hotpath.c src\scan_win32.c src\gui_win32.c src\tray_win32.c src\um_enum.c `
  src\um_toast_ui.c src\um_toast_win32.c res\usbmon.res.o -o usbmon.exe `
  -luser32 -lgdi32 -lshell32 -ladvapi32 -lcfgmgr32 -lsetupapi
.\tests\..\usbmon.exe --version
```

预期：编译零告警；`usbmon --version` 输出 `2.5.0`。

- [ ] **步骤 2：确认 manifest 已内嵌进 PE**

```powershell
gcc -Wl,--version > $null; windres --version > $null
```

或用 `strings` 替代方案：检查 PE 中出现 `PerMonitorV2`：

```powershell
Select-String -Path usbmon.exe -Pattern 'PerMonitorV2' -Encoding ascii -Quiet
```

预期：`True`。

- [ ] **步骤 3：跑仿真层单测（Linux/CI 权威，本机尝试 `-U_WIN32`）**

```powershell
gcc -std=c99 -O2 -Wall -Wextra -pedantic -Werror -U_WIN32 -Isrc -Itests -o tests\ui_test.exe `
  tests\ui_test.c src\um_toast_ui.c src\um_toast_win32.c tests\win32_shim.c
.\tests\ui_test.exe
```

预期：`N checks, 0 failures`，并生成 4 个 SVG 快照。若 mingw 头文件在 `-U_WIN32`
下不可用，则本步交由 CI（ubuntu `make strict && make selftest`）验证，本机只做
Windows 侧编译。

- [ ] **步骤 4：独立子代理（oracle）审查完整 diff**

用已存在的 oracle 会话复核：内核 0 行语义、fit 取整、命中测试反除、封送协议
内存生命周期（carrier 在回调里 free 是否安全）、`WM_DPICHANGED` 时序、
`apply_geometry` 每次 `SetWindowRgn` 的区域所有权。修复其发现后重跑步骤 1/3。

- [ ] **步骤 5：提交 + 推送**

```bash
git add -A src res Makefile tests tools .github CHANGELOG.md docs
git status --short          # 确认 usbmon_复查_v2_superpowers.md 未被加入
git commit -m "feat(ui): 2.5.0 — panel-kernel toasts, PerMonitorV2, work-area clamped sizing"
git push origin main
```

- [ ] **步骤 6：打标签并发布 release**

```bash
git tag -a v2.5.0 -m "2.5.0: panel-kernel toasts, PerMonitorV2 DPI, work-area clamped relative sizing"
git push origin v2.5.0
```

然后用 GitHub REST API（token 从 `git credential fill` 取，**不要打印**）创建
release；若 403/无权限，则请用户在 https://github.com/purrfecto114-lgtm/USBMonitor/releases
点 "Draft a new release" 并粘贴 tag `v2.5.0` + 上述 release notes。

- [ ] **步骤 7：收尾**

确认 CI 全绿（`build-and-test` / `analyze` / `build-windows` / `verify-windows`），
`verify-windows` 的 demo.ps1 会自动校验 `usbmon.exe --version` 与 `UM_VERSION` 一致。