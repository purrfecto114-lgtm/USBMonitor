/* um_toast_win32.c — Win32 渲染后端（usbmon 通知浮层）。
 *
 * 这一层只做三件事：
 *   1. 把 um_toast_ui.c 算好的 draw list 用 GDI 画出来（双缓冲，无闪烁）；
 *   2. 把鼠标/键盘/定时器消息翻译成 um_toast_state 上的状态迁移；
 *   3. 把用户动作以回调形式抛回给调用方（打开 / 定位 / 复制 / 安全弹出…）。
 *
 * 它刻意**不**知道 USB、不碰注册表、不做 IOCTL：安全弹出、复制路径这些
 * 动作交给 tray/gui 层复用已有实现（tray_win32.c 里已经有了）。
 *
 * 关键实现取舍（均有据可查，见 docs/PORT_AND_TRADEOFFS.md）：
 *   - 按钮不用子窗口控件，而用"自绘 + 命中测试"：省掉 BS_OWNERDRAW 的
 *     消息往返，也避免 layered window 下子窗口不跟随合成的老问题；
 *     键盘可达性用状态机里的 focus_row / focus_btn 补齐（Tab 焦点环）。
 *   - 容量条自绘：PBM_SETBARCOLOR 在启用视觉样式时直接失效（Microsoft
 *     Learn 明确说明），自绘是唯一稳定可控的路径。
 *   - 深色跟随系统：HKCU\...\Themes\Personalize\AppsUseLightTheme（0=深色）。
 *   - hover 暂停：鼠标在窗内（WM_MOUSEMOVE，同 1.1.1 enterEvent 语义）即
 *     暂停；TrackMouseEvent(TME_LEAVE|TME_HOVER) 兜底（触发一次后须重注）。
 *   - 圆角用 SetWindowRgn(CreateRoundRectRgn)：比 layered window 简单，
 *     不引入 UpdateLayeredWindow 的整套位图合成。
 */
#include "um_toast_ui.h"
#include "um_toast_win32.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include "win32_shim.h"     /* 仅用于 Linux 上的仿真测试与 SVG 快照 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ 动作 -- */

/* um_toast_action / um_toast_cb / um_toast_win 见 um_toast_win32.h */

/* ------------------------------------------------------------------ 窗口 -- */

#define UM_TIMER_TICK   1
#define UM_TIMER_FADE   2
#define UM_TICK_MS      100
#define UM_FADE_STEP    34

struct um_toast_win {
    HWND             hwnd;
    um_toast_model   model;
    um_toast_state   state;
    um_theme         theme;
    int              width, height;     /* 设计空间（96DPI）像素 */
    int              dpi;
    double           fit;              /* 工作区钳制因子（0.05..1.0） */
    int              topmost;
    int              tracking;          /* TrackMouseEvent 已注册 */
    int              alpha;             /* 0..255 淡入 */
    int              last_sec;          /* 上次重画时的倒计时秒（-1 = 已暂停） */
    um_toast_cb      cb;
    void            *user;
};

static HINSTANCE g_hinst;              /* um_toast_win_init 设定一次 */

/* 文本绘制的签名差异：Windows 要 LPCWSTR(16 位)，shim 用显式 uint16_t。 */
#ifdef _WIN32
#define UM_TXT(x) ((LPCWSTR)(x))
#else
#define UM_TXT(x) ((const uint16_t *)(x))
#endif

static const wchar_t *g_class = L"usbmonToast2";

/* ------------------------------------------------------------ 小工具 ------ */

/* 物理像素 = 设计空间值 x (dpi/96) x fit。布局、字体、圆角、命中测试共用
 * 同一个因子，所以缩放不会让布局错位（2.4.0 的根因：字号与坐标用
 * w->dpi，但窗口物理尺寸在 create 时就按设计空间写死了）。 */
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

/* 物理 -> 设计空间（鼠标坐标）：与 ui_scale 同一因子。 */
static int to_logical(const struct um_toast_win *w, int phys)
{
    double f = ui_factor(w);
    return (f < 0.01) ? phys : (int)(phys / f);
}

/* 传给 make_font 的 DPI：等价于把字号直接乘 ui_factor，避免两套取整。 */
static int ui_font_dpi(const struct um_toast_win *w)
{
    int v = (int)(ui_factor(w) * 96.0 + 0.5);
    return (v < 16) ? 96 : v;
}

static COLORREF cref(unsigned long rgb)
{
    return (COLORREF)(((rgb & 0xFF) << 16) | (rgb & 0x00FF00) |
                      ((rgb >> 16) & 0xFF));
}

static HFONT make_font(int px, int dpi, int bold, const wchar_t *face)
{
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static const wchar_t *font_face(void)
{
    /* 中文优先：Segoe UI 无 CJK 字形时系统会回退，这里显式给 YaHei UI。 */
    return L"Microsoft YaHei UI";
}

/* 与 1.1.1 usb_svg() 同构的 USB 图标：外壳 + trident + 右下角状态角标。 */
static void draw_usb_icon(HDC hdc, int x, int y, int w, int h,
                          const char *status, unsigned long badge_rgb,
                          unsigned long fg_rgb)
{
    HBRUSH shell = CreateSolidBrush(cref(0x2d3746));
    HBRUSH socket = CreateSolidBrush(cref(0x3d4a5c));
    HBRUSH badge = CreateSolidBrush(cref(badge_rgb));
    HBRUSH light = CreateSolidBrush(cref(fg_rgb));
    HPEN   pen = CreatePen(PS_SOLID, 1, cref(0x55708f));
    HPEN   mark = CreatePen(PS_SOLID, 2, cref(0xffffff));
    HPEN   oldp;
    HBRUSH oldb;
    int add = (status && strcmp(status, "add") == 0);
    int rm  = (status && (strcmp(status, "remove") == 0 ||
                          strcmp(status, "error") == 0));

    oldb = (HBRUSH)SelectObject(hdc, socket);
    oldp = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, x + w * 30 / 100, y, x + w * 70 / 100, y + h * 28 / 100,
              w * 12 / 100, h * 12 / 100);
    SelectObject(hdc, light);
    Rectangle(hdc, x + w * 37 / 100, y + h * 7 / 100,
              x + w * 46 / 100, y + h * 21 / 100);
    Rectangle(hdc, x + w * 54 / 100, y + h * 7 / 100,
              x + w * 63 / 100, y + h * 21 / 100);

    SelectObject(hdc, shell);
    RoundRect(hdc, x + w * 20 / 100, y + h * 31 / 100,
              x + w * 80 / 100, y + h * 95 / 100,
              w * 20 / 100, h * 20 / 100);

    /* trident：竖 + 横 + 两条向下的分叉（用线段而非反向矩形：GDI 的
     * Rectangle 要求 left<right/top<bottom，写反了只会画出一块实心） */
    SelectObject(hdc, light);
    Rectangle(hdc, x + w * 47 / 100, y + h * 42 / 100,
              x + w * 53 / 100, y + h * 76 / 100);
    Rectangle(hdc, x + w * 35 / 100, y + h * 52 / 100,
              x + w * 65 / 100, y + h * 58 / 100);
    {
        /* 叉笔：命名创建、用完即删（原来 SelectObject(CreatePen(...)) 的写法
         * 每次泄漏 1 个 HPEN，长会话会耗尽 GDI 句柄配额） */
        HPEN fork = CreatePen(PS_SOLID, 2, cref(fg_rgb));
        HPEN prev = (HPEN)SelectObject(hdc, fork);
        MoveToEx(hdc, x + w * 36 / 100, y + h * 51 / 100, NULL);
        LineTo(hdc, x + w * 43 / 100, y + h * 44 / 100);
        MoveToEx(hdc, x + w * 64 / 100, y + h * 51 / 100, NULL);
        LineTo(hdc, x + w * 57 / 100, y + h * 44 / 100);
        SelectObject(hdc, prev);
        DeleteObject(fork);
    }
    SelectObject(hdc, oldp);

    /* 角标 */
    SelectObject(hdc, badge);
    Ellipse(hdc, x + w * 62 / 100, y + h * 56 / 100,
            x + w * 98 / 100, y + h * 96 / 100);
    if (add || rm) {
        SelectObject(hdc, mark);
        if (add) {
            MoveToEx(hdc, x + w * 71 / 100, y + h * 76 / 100, NULL);
            LineTo(hdc, x + w * 78 / 100, y + h * 83 / 100);
            LineTo(hdc, x + w * 90 / 100, y + h * 68 / 100);
        } else {
            MoveToEx(hdc, x + w * 72 / 100, y + h * 70 / 100, NULL);
            LineTo(hdc, x + w * 89 / 100, y + h * 88 / 100);
            MoveToEx(hdc, x + w * 89 / 100, y + h * 70 / 100, NULL);
            LineTo(hdc, x + w * 72 / 100, y + h * 88 / 100);
        }
        SelectObject(hdc, oldp);
    }
    SelectObject(hdc, oldb);
    DeleteObject(shell); DeleteObject(socket); DeleteObject(badge);
    DeleteObject(light); DeleteObject(pen); DeleteObject(mark);
}

/* ------------------------------------------------------------- 绘制 ------- */

static void paint_toast(struct um_toast_win *w, HDC hdc)
{
    um_drawlist dl;
    HFONT fonts[5];
    HFONT old_font;
    HDC   mem;
    HBITMAP bmp, old_bmp;
    int pw = ui_scale(w, w->width), ph = ui_scale(w, w->height);
    int i;

    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, pw, ph);
    old_bmp = (HBITMAP)SelectObject(mem, bmp);

    um_toast_layout(&w->model, &w->state, &w->theme,
                    w->width, w->height, &dl);

    fonts[UM_F_TITLE]    = make_font(16, ui_font_dpi(w), 1, font_face());
    fonts[UM_F_BODY]     = make_font(13, ui_font_dpi(w), 0, font_face());
    fonts[UM_F_SMALL]    = make_font(12, ui_font_dpi(w), 0, font_face());
    fonts[UM_F_BTN]      = make_font(13, ui_font_dpi(w), 1, font_face());
    fonts[UM_F_ROWTITLE] = make_font(13, ui_font_dpi(w), 1, font_face());
    old_font = (HFONT)SelectObject(mem, fonts[UM_F_BODY]);
    SetBkMode(mem, TRANSPARENT);

    for (i = 0; i < dl.n; i++) {
        const um_draw *d = &dl.items[i];
        int x = ui_scale(w, d->x), y = ui_scale(w, d->y);
        int ww = ui_scale(w, d->w), hh = ui_scale(w, d->h);

        switch (d->kind) {
        case UM_D_RECT: {
            RECT rc;
            HBRUSH br = CreateSolidBrush(cref(d->color));
            rc.left = x; rc.top = y; rc.right = x + ww; rc.bottom = y + hh;
            FillRect(mem, &rc, br);
            DeleteObject(br);
            break;
        }
        case UM_D_ROUNDRECT: {
            int r = ui_scale(w, d->radius);
            HBRUSH br;
            HPEN pen;
            if (d->stroke) {
                /* NULL_BRUSH：真 Win32 的 stock object（=5，wingdi.h），
                 * 原来写成 shim 专有的 NULL_BRUSH_OBJ，真机编译必失败 */
                br = (HBRUSH)GetStockObject(NULL_BRUSH);
                pen = CreatePen(PS_SOLID, 1, cref(d->color));
            } else {
                br = CreateSolidBrush(cref(d->color));
                pen = CreatePen(PS_SOLID, 1, cref(d->color));
            }
            {
                HBRUSH ob = (HBRUSH)SelectObject(mem, br);
                HPEN op = (HPEN)SelectObject(mem, pen);
                RoundRect(mem, x, y, x + ww, y + hh, r, r);
                SelectObject(mem, ob);
                SelectObject(mem, op);
            }
            if (!d->stroke) DeleteObject(br);
            DeleteObject(pen);
            break;
        }
        case UM_D_LINE:
            break;
        case UM_D_TEXT: {
            uint16_t wt[UM_UI_MAX_TEXT];
            RECT rc;
            UINT fmt = DT_SINGLELINE | DT_NOPREFIX;
            /* 真正的 UTF-8 → UTF-16 解码。原来是逐字节 cast，中文会变
             * 成 "è®¾" 这类乱码（"设" 的 E8 AE BE 被当成三个拉丁码点）。*/
            um_ui_utf8_to_utf16(d->text, wt, UM_UI_MAX_TEXT);
            SelectObject(mem, fonts[d->font]);
            SetTextColor(mem, cref(d->color));
            rc.left = x; rc.top = y;
            rc.right = x + ww; rc.bottom = y + ui_scale(w, 22);
            if (d->align == UM_A_RIGHT)       fmt |= DT_RIGHT;
            else if (d->align == UM_A_CENTER) fmt |= DT_CENTER;
            else                              fmt |= DT_LEFT;
            if (d->clip) fmt |= DT_END_ELLIPSIS;
            DrawTextW(mem, UM_TXT(wt), -1, &rc, fmt);
            break;
        }
        case UM_D_ICON:
            draw_usb_icon(mem, x, y, ww, hh, d->text, d->color2, d->color);
            break;
        }
    }

    SelectObject(mem, old_font);
    for (i = 0; i < 5; i++) DeleteObject(fonts[i]);
    BitBlt(hdc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ------------------------------------------------------------ 菜单/动作 --- */

static void fire(struct um_toast_win *w, um_toast_action act, int row)
{
    if (w->cb) w->cb(act, row, w->user);
}

/* 重新计算高度、重新锚定到工作区右下角、套圆角区域。
 * update / toggle / 展开按钮共用：原来 update 用 SWP_NOMOVE 从左上角向下
 * 生长，展开后底部会越过工作区（被任务栏遮挡）；展开按钮路径则只改
 * w->height 不改真窗口尺寸（内容被裁、下半区收不到鼠标消息）。 */
/* 锚定区 = 光标所在显示器的工作区：多屏 + 负坐标虚拟桌面都正确（副屏在
 * 左侧/上方时坐标本来就可以为负）。依次回退 光标所在屏 rcWork ->
 * SPI_GETWORKAREA（主屏）-> SM_CX/CYSCREEN。 */
static void monitor_workarea(RECT *wa)
{
    POINT pt;

    memset(wa, 0, sizeof *wa);
    if (GetCursorPos(&pt)) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (mon) {
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoA(mon, &mi)) { *wa = mi.rcWork; return; }
        }
    }
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, wa, 0)) return;
    wa->left = 0; wa->top = 0;
    wa->right  = GetSystemMetrics(SM_CXSCREEN);
    wa->bottom = GetSystemMetrics(SM_CYSCREEN);
}

/* 重新计算高度、重新锚定到工作区右下角、套圆角区域。
 * update / toggle / 展开按钮 / WM_DPICHANGED / move_slot 共用。 */
static void apply_geometry(struct um_toast_win *w)
{
    RECT wa;
    int  pw, ph, x, y;

    w->height = um_toast_measure_height(&w->model, &w->state);
    monitor_workarea(&wa);

    /* fit：把窗口等比钳进工作区（<=70% 宽、<=85% 高）。设计空间布局不变，
     * 只是整体缩小 —— 任何分辨率/缩放下都不会超出所在显示器，也不需要
     * 为“大屏/小屏”写两套常量。 */
    pw = (int)(w->width  * w->dpi / 96.0 + 0.5);
    ph = (int)(w->height * w->dpi / 96.0 + 0.5);
    {
        double aw = (double)(wa.right  - wa.left);
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
    /* 不再钳 x/y 到 0：多屏虚拟桌面坐标可以为负，fit 已保证窗口落在所在
     * 显示器的工作区内（2.4.0 的 if (x<0) x=0 会把副屏通知弹回主屏）。 */
    SetWindowPos(w->hwnd, NULL, x, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, pw + 1, ph + 1,
                                      ui_scale(w, 20), ui_scale(w, 20));
        SetWindowRgn(w->hwnd, rgn, FALSE);
    }
}

static void show_row_menu(struct um_toast_win *w, int row, int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;

    int openable  = (row < w->model.n_rows) ? w->model.rows[row].openable : 0;
    int ejectable = (row < w->model.n_rows) ? w->model.rows[row].ejectable : 0;

    AppendMenuW(m, MF_STRING | (openable ? 0 : MF_GRAYED), 1, L"打开");
    AppendMenuW(m, MF_STRING | (openable ? 0 : MF_GRAYED), 2,
                L"在资源管理器中显示");
    AppendMenuW(m, MF_STRING, 3, L"复制路径");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (ejectable ? 0 : MF_GRAYED), 4, L"安全弹出");

    pt.x = ui_scale(w, x); pt.y = ui_scale(w, y);
    ClientToScreen(w->hwnd, &pt);
    /* 非前台窗口弹菜单的短板：点菜单外不会自动收起。经典修法 =
     * 弹前 SetForegroundWindow，弹后 PostMessage(WM_NULL)（Raymond Chen）。 */
    SetForegroundWindow(w->hwnd);
    cmd = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                pt.x, pt.y, w->hwnd, NULL);
    PostMessageW(w->hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case 1: fire(w, UM_ACT_OPEN_ROW, row); break;
    case 2: fire(w, UM_ACT_REVEAL, row);   break;
    case 3: fire(w, UM_ACT_COPY, row);     break;
    case 4: fire(w, UM_ACT_EJECT, row);    break;
    default: break;
    }
}

static void on_click(struct um_toast_win *w, int x, int y)
{
    um_ui_hit hit = um_toast_hit_test(&w->model, &w->state,
                                      w->width, w->height, x, y);
    if (hit == UM_HIT_NONE) {
        /* 0 行 = 纯文字信息框：整窗即关闭区（保持 2.4.0 文本 toast
         * “点任意处消失”的语义）。有行的面板点空区仍然什么都不做。 */
        if (w->model.n_rows == 0) fire(w, UM_ACT_CLOSE, -1);
        return;
    }
    if (hit == UM_HIT_OPEN) {
        int i;
        /* 打开"第一个可打开"的卷，而不是死板的 rows[0] */
        for (i = 0; i < w->model.n_rows; i++) {
            if (w->model.rows[i].openable) { fire(w, UM_ACT_OPEN, i); break; }
        }
        return;
    }
    else if (hit == UM_HIT_CLOSE)  fire(w, UM_ACT_CLOSE, -1);
    else if (hit == UM_HIT_EXPAND) {
        w->state.expanded = !w->state.expanded;
        apply_geometry(w);            /* 真的改窗口尺寸与锚点，不只是重画 */
        InvalidateRect(w->hwnd, NULL, FALSE);
        fire(w, UM_ACT_TOGGLE_EXPAND, -1);
    } else if (hit >= UM_HIT_ROW && hit < UM_HIT_ROW + UM_UI_MAX_ROWS) {
        /* 无盘符 / 非存储设备（智能笔、网卡、拓展坞、空槽、未解锁卷）
         * 没有"打开"语义：点了不能假装成功，也不该弹错 */
        int row = (int)hit - UM_HIT_ROW;
        if (row < w->model.n_rows && w->model.rows[row].openable)
            fire(w, UM_ACT_OPEN_ROW, row);
    } else if (hit >= UM_HIT_ROW_OPEN &&
             hit < UM_HIT_ROW_OPEN + UM_UI_MAX_ROWS) {
        int row = (int)hit - UM_HIT_ROW_OPEN;
        if (row < w->model.n_rows && w->model.rows[row].openable)
            fire(w, UM_ACT_OPEN_ROW, row);
    }
}

/* ---------------------------------------------------------- 窗口过程 ------ */

static LRESULT CALLBACK toast_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    struct um_toast_win *w =
        (struct um_toast_win *)GetWindowLongPtrW(hw, GWLP_USERDATA);

    /* CreateWindowEx 同步跑完整套创建消息，而 userdata 是它返回之后
     * um_toast_win_new 才写进去的 —— 所以创建期消息到达时 w 必然为 NULL。
     * 这里不能对空 w 一律 return 0：WM_NCCREATE 返回 FALSE 就等于否决创建，
     * CreateWindowEx 随即返回 NULL（GetLastError 还是 0，看起来像"没错误地
     * 失败"）；而且 WM_NCCREATE 等创建期消息必须交给 DefWindowProc，否则
     * CREATESTRUCT 里的窗口标题不会被记录，类+标题查找会全部落空。
     * 销毁后 userdata 已在 WM_DESTROY 清空，此处的 w 恒为 NULL，交给
     * DefWindowProc 不会碰到已 free 的对象（也就堵住了旧版 UAF）。 */
    if (!w)
        return DefWindowProcW(hw, msg, wp, lp);

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* 双缓冲：不需要擦除背景 */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        paint_toast(w, hdc);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == UM_TIMER_TICK) {
            if (um_toast_tick(&w->state, UM_TICK_MS)) {
                w->state.visible = 0;
                ShowWindow(hw, SW_HIDE);
                fire(w, UM_ACT_AUTOHIDE, -1);
            } else {
                /* 倒计时文案只在秒变化（或暂停态翻转）时才需要重画；
                 * 100ms 无条件全量重画会白白重建 drawlist（约 80KB）。 */
                int sec = w->state.paused ? -1 : w->state.remaining_ms / 1000;
                if (sec != w->last_sec) {
                    w->last_sec = sec;
                    InvalidateRect(hw, NULL, FALSE);
                }
            }
        } else if (wp == UM_TIMER_FADE) {
            w->alpha += UM_FADE_STEP;
            if (w->alpha >= 255) {
                w->alpha = 255;
                KillTimer(hw, UM_TIMER_FADE);
            }
            SetLayeredWindowAttributes(hw, 0, (BYTE)w->alpha, LWA_ALPHA);
        }
        return 0;

    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        um_ui_hit hit;
        int row = -1;
        if (!w->tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof tme;
            /* TME_LEAVE|TME_HOVER 都要注：只注 LEAVE 时真 Win32 根本不会发
             * WM_MOUSEHOVER（原实现漏了 TME_HOVER，hover 暂停在生产上是死
             * 功能；现在 WM_MOUSEMOVE 里也直接暂停，双保险）。 */
            tme.dwFlags = TME_LEAVE | TME_HOVER;
            tme.hwndTrack = hw;
            tme.dwHoverTime = HOVER_DEFAULT;
            if (TrackMouseEvent(&tme)) w->tracking = 1;
        }
        um_toast_pause(&w->state);   /* 鼠标在窗内即暂停（1.1.1 enterEvent） */
        hit = um_toast_hit_test(&w->model, &w->state, w->width, w->height,
                                to_logical(w, x), to_logical(w, y));
        if (hit >= UM_HIT_ROW && hit < UM_HIT_ROW + UM_UI_MAX_ROWS)
            row = (int)hit - UM_HIT_ROW;
        if (row != w->state.hover_row || (int)hit != w->state.hover_btn) {
            w->state.hover_row = row;
            w->state.hover_btn = (int)hit;
            InvalidateRect(hw, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        w->tracking = 0;
        w->state.hover_row = -1;
        w->state.hover_btn = 0;
        um_toast_resume(&w->state);      /* 离开后继续倒计时 */
        InvalidateRect(hw, NULL, FALSE);
        return 0;

    case WM_MOUSEHOVER:
        um_toast_pause(&w->state);       /* 悬停暂停（1.1.1 enterEvent） */
        return 0;

    case WM_MOUSEWHEEL: {
        /* 展开态行区滚动（1.1.1 QScroller 的替代：行多时展开区裁不下，
         * 没有滚轮时第 5 行以后永不可见不可点）。折叠态不滚。 */
        int delta = (int)(short)HIWORD(wp);
        int maxs;
        if (!w || !w->state.expanded) return 0;
        maxs = um_toast_max_scroll(&w->model, &w->state, w->height);
        w->state.scroll_px -= delta;     /* 向上滚 = 内容上移 */
        if (w->state.scroll_px < 0) w->state.scroll_px = 0;
        if (w->state.scroll_px > maxs) w->state.scroll_px = maxs;
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        on_click(w, to_logical(w, (int)(short)LOWORD(lp)),
                 to_logical(w, (int)(short)HIWORD(lp)));
        return 0;

    case WM_RBUTTONUP: {
        int x = to_logical(w, (int)(short)LOWORD(lp));
        int y = to_logical(w, (int)(short)HIWORD(lp));
        um_ui_hit hit;
        hit = um_toast_hit_test(&w->model, &w->state, w->width, w->height,
                                x, y);
        if (hit >= UM_HIT_ROW && hit < UM_HIT_ROW + UM_UI_MAX_ROWS)
            show_row_menu(w, (int)hit - UM_HIT_ROW, x, y);
        else
            fire(w, UM_ACT_CLOSE, -1);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { fire(w, UM_ACT_CLOSE, -1); return 0; }
        if (wp == VK_RETURN) {
            int row = w->state.focus_row;
            if (row >= 0) {
                if (row < w->model.n_rows && w->model.rows[row].openable)
                    fire(w, UM_ACT_OPEN_ROW, row);
            } else {
                /* 回车 = 主按钮：打开第一个可打开的卷 */
                int i;
                for (i = 0; i < w->model.n_rows; i++) {
                    if (w->model.rows[i].openable) { fire(w, UM_ACT_OPEN, i); break; }
                }
            }
            return 0;
        }
        if (wp == VK_TAB) {
            /* 焦点环：行 → 主按钮 → 展开 → 关闭 → 回到首行 */
            if (w->state.focus_row + 1 < w->model.n_rows) w->state.focus_row++;
            else { w->state.focus_row = -1; w->state.focus_btn = 0; }
            InvalidateRect(hw, NULL, FALSE);
            return 0;
        }
        if (wp == VK_UP || wp == VK_DOWN) {
            if (w->model.n_rows > 0) {
                w->state.focus_row += (wp == VK_DOWN) ? 1 : -1;
                if (w->state.focus_row < -1) w->state.focus_row = -1;
                if (w->state.focus_row >= w->model.n_rows)
                    w->state.focus_row = w->model.n_rows - 1;
                InvalidateRect(hw, NULL, FALSE);
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
        if (w) {
            KillTimer(hw, UM_TIMER_TICK);
            KillTimer(hw, UM_TIMER_FADE);
        }
        return 0;

    case WM_DPICHANGED: {
        /* HIWORD(wParam) = new DPI of the monitor we just moved onto.  lParam holds
         * the suggested rect, but apply_geometry() re-anchors bottom-right inside the
         * work area itself, so the suggestion is deliberately ignored.  Guard the
         * (abnormal) 0/low case: a 0 dpi would make ui_scale() collapse the window
         * to 0x0 with no way back. */
        int d = (int)HIWORD(wp);
        w->dpi = (d >= 16) ? d : 96;
        apply_geometry(w);
        InvalidateRect(hw, NULL, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* WS_POPUP + WS_EX_LAYERED, self-resizing for expand/fit/DPI: pin the
         * track sizes so DefWindowProc's style-derived limits can never clamp a
         * legitimate resize. */
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 0;
        mm->ptMinTrackSize.y = 0;
        mm->ptMaxTrackSize.x = 0x7fff;
        mm->ptMaxTrackSize.y = 0x7fff;
        return 0;
    }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* --------------------------------------------------------------- 公开 API -- */

int um_toast_win_enable_dpi_awareness(void)
{
#ifdef _WIN32
    /* PerMonitorV2 first (Win10 1703+): once the process is DPI-aware,
     * GetDeviceCaps(LOGPIXELSY) stops being virtualized to 96 and reports the
     * real DPI.  WM_DPICHANGED then arrives when the window is dragged across
     * monitors (or the scale changes) and re-lays it out for that monitor.
     *
     * These three entry points are resolved dynamically so the binary still
     * starts on Win7/8 (where the first two do not exist).  A union is used
     * instead of a cast so -Wcast-function-type stays quiet without needing
     * WINVER/_WIN32_WINNT bumps. */
    union proc {
        FARPROC                        raw;
        BOOL   (WINAPI *ctx)(void *);      /* SetProcessDpiAwarenessContext */
        HRESULT (WINAPI *aware)(int);      /* SetProcessDpiAwareness        */
        BOOL   (WINAPI *none)(void);       /* SetProcessDPIAware            */
    };
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    HMODULE shc = LoadLibraryW(L"shcore.dll");
    union proc set_ctx, set_aware, set_dpi;
    int ok = 0;

    set_ctx.raw = u32 ? GetProcAddress(u32,
                        "SetProcessDpiAwarenessContext") : NULL;
    set_aware.raw = shc ? GetProcAddress(shc,
                        "SetProcessDpiAwareness") : NULL;
    set_dpi.raw = u32 ? GetProcAddress(u32,
                        "SetProcessDPIAware") : NULL;

    if (set_ctx.raw) {
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4; passing
         * the magic value by hand avoids depending on the SDK constant. */
        void *v2 = (void *)(LONG_PTR)-4;
        if (set_ctx.ctx(v2)) ok = 1;
    }
    if (!ok && set_aware.raw) {
        /* PROCESS_PER_MONITOR_DPI_AWARE == 2 (Win8.1) */
        if (SUCCEEDED(set_aware.aware(2))) ok = 1;
    }
    if (!ok && set_dpi.raw) {
        /* Vista+: system-DPI only, but better than nothing. */
        if (set_dpi.none()) ok = 1;
    }
    if (shc) FreeLibrary(shc);
    return ok;
#else
    return 1;                       /* shim: LOGPIXELSY / monitor 全部可控 */
#endif
}

int um_toast_win_init(void *hinst)
{
    HINSTANCE h = (HINSTANCE)hinst;
    WNDCLASSW wc;
    /* 必须在创建任何 HWND 之前设置：一旦进程已经有窗口，API 会失败。 */
    um_toast_win_enable_dpi_awareness();
    /* NULL（测试/宿主进程未传）时用本进程模块句柄：真 Win32 上
     * RegisterClassW(hInstance=NULL) 会失败，而 GetModuleHandleW(NULL)
     * 恰好就是可执行文件的实例句柄。 */
    if (!h) h = (HINSTANCE)GetModuleHandleW(NULL);
    if (!h) return 0;
    g_hinst = h;
    memset(&wc, 0, sizeof wc);
#ifdef _WIN32
    wc.lpfnWndProc = toast_proc;
#else
    wc.lpfnWndProc = (LONG_PTR)toast_proc;   /* shim 用 LONG_PTR 存函数指针 */
#endif
    wc.hInstance = h;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = g_class;
    return RegisterClassW(&wc) ? 1 : 0;
}

/* 系统是否深色：HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\
 * Personalize\AppsUseLightTheme == 0 表示深色。读不到就按浅色处理。 */
int um_toast_system_dark(void)
{
#ifdef _WIN32
    HKEY k;
    DWORD v = 1, sz = sizeof v;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL,
                         (LPBYTE)&v, &sz) != ERROR_SUCCESS)
        v = 1;
    RegCloseKey(k);
    return v == 0;
#else
    return 1;
#endif
}

um_toast_win *um_toast_win_new(const um_toast_model *m, const um_theme *t,
                               int topmost, int width, int ttl_ms,
                               um_toast_cb cb, void *user)
{
    struct um_toast_win *w = (struct um_toast_win *)calloc(1, sizeof *w);
    DWORD ex;
    HDC   hdc;
    HRGN  rgn;
    int   pw, ph;

    if (!w) return NULL;
    w->model = *m;
    w->theme = *t;
    w->topmost = topmost;
    w->cb = cb;
    w->user = user;
    w->fit = 1.0;                       /* calloc 给的是 0，必须显式初始化 */
    um_toast_state_init(&w->state, (ttl_ms > 0) ? ttl_ms : 10000);
    w->width  = (width > 0) ? width : UM_UI_WIDTH;
    w->height = um_toast_measure_height(&w->model, &w->state);

    hdc = GetDC(NULL);
    w->dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (w->dpi < 16) w->dpi = 96;      /* 防御；注意 75/96 是合法的低 dpi */
    ReleaseDC(NULL, hdc);

    pw = ui_scale(w, w->width);
    ph = ui_scale(w, w->height);

    ex = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (topmost) ex |= WS_EX_TOPMOST;

    /* 0 行信息框与设备面板同一个窗口类（同一套渲染内核），只靠标题区分：
     * 标题 "usbmon" = 设备面板，"usbmon-toast" = 文字信息框。 */
    w->hwnd = CreateWindowExW(ex, g_class,
                              (m->n_rows > 0) ? L"usbmon" : L"usbmon-toast",
                              WS_POPUP,
                              0, 0, pw, ph, NULL, NULL,
                              g_hinst, NULL);
    if (!w->hwnd) { free(w); return NULL; }

    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    rgn = CreateRoundRectRgn(0, 0, pw + 1, ph + 1, ui_scale(w, 20),
                             ui_scale(w, 20));
    SetWindowRgn(w->hwnd, rgn, FALSE);
    w->alpha = 0;
    SetLayeredWindowAttributes(w->hwnd, 0, 0, LWA_ALPHA);
    SetTimer(w->hwnd, UM_TIMER_TICK, UM_TICK_MS, NULL);
    SetTimer(w->hwnd, UM_TIMER_FADE, 16, NULL);
    return w;
}

/* 按文本实测推导信息框宽度：三行按各自字号实测后取最大需求，
 * headline 16px / subtitle 12px / summary 13px（96dpi 基准，与 fit 无关），
 * 夹在 [300, UM_UI_WIDTH]。headline/subtitle 的文本盒是 W-2*PAD-60，
 * 所以需求是 w+92；再加 20px 裕量得 w+112；summary 全宽 W-2*PAD，
 * 需求 w+32，加 8px 裕量得 w+40。溢出防护统一交给 apply_geometry 的
 * fit 钳制，这里只管"别太宽"。 */
int um_toast_suggest_width(const um_toast_model *m)
{
    const char *lines[3];
    const int   px[3] = { 16, 12, 13 };
    uint16_t    buf[UM_UI_MAX_TEXT];
    HDC  dc;
    SIZE sz;
    int  widths[3] = { 0, 0, 0 };
    int  w, i;

    if (!m) return UM_UI_WIDTH;
    lines[0] = m->headline;
    lines[1] = m->subtitle;
    lines[2] = m->summary;
    dc = GetDC(NULL);
    if (!dc) return 300;
    for (i = 0; i < 3; i++) {
        HFONT f, old;
        int n;
        if (!lines[i] || !lines[i][0]) continue;
        f = make_font(px[i], 96, (i == 0), font_face());
        old = (HFONT)SelectObject(dc, f);
        n = um_ui_utf8_to_utf16(lines[i], buf, UM_UI_MAX_TEXT - 1);
        if (n < 0) n = 0;
        if (n >= UM_UI_MAX_TEXT) n = UM_UI_MAX_TEXT - 1;
        buf[n] = 0;
        if (GetTextExtentPoint32W(dc, buf, n, &sz))
            widths[i] = (int)sz.cx;
        SelectObject(dc, old);
        DeleteObject(f);
    }
    ReleaseDC(NULL, dc);

    w = widths[2] + 2 * UM_UI_PAD + 8;        /* summary：内边距 + 左色条 */
    if (widths[0] + 112 > w) w = widths[0] + 112;   /* headline：盒宽 + 裕量 */
    if (widths[1] + 112 > w) w = widths[1] + 112;   /* subtitle：同上 */
    if (w < 300) w = 300;
    if (w > UM_UI_WIDTH) w = UM_UI_WIDTH;
    return w;
}

/* 槽位偏移：先留出 base_px（设备像素，面板可见时=面板高度），再按槽叠放
 * （每个槽 = 通知高度 + 12 设计空间 px，都按 fit 缩放）。base_px<=0 且
 * slot<=0 时不移动（保持旧 move_slot(0) 的 no-op 语义）。用 SWP_NOSIZE：
 * 槽位变化不改窗口尺寸。 */
void um_toast_win_move_slot_ex(um_toast_win *w, int slot, int base_px)
{
    RECT rc;
    RECT wa;
    int  off, y;

    if (!w || !w->hwnd) return;
    if (slot < 0) slot = 0;
    if (base_px < 0) base_px = 0;
    if (base_px == 0 && slot == 0) return;
    if (!GetWindowRect(w->hwnd, &rc)) return;
    off = base_px + slot * (ui_scale(w, w->height) + ui_scale(w, 12));
    if (base_px > 0) off += ui_scale(w, 12);   /* 面板与本通知之间的间隙 */
    y = (int)(rc.top - off);
    if (um_toast_monitor_workarea(&wa)) {
        int min_y = (int)(wa.top + ui_scale(w, UM_UI_MARGIN));
        if (y < min_y) y = min_y;
    }
    SetWindowPos(w->hwnd, NULL, (int)rc.left, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void um_toast_win_move_slot(um_toast_win *w, int slot)
{
    um_toast_win_move_slot_ex(w, slot, 0);
}

/* 窗口当前物理高度（设备像素）。面板可见时宿主用它作为通知叠放的基线，
 * 这样 230px 的面板不会被 148px 的通知盖住。 */
int um_toast_win_pixel_height(const um_toast_win *w)
{
    RECT r;
    if (!w || !w->hwnd || !GetWindowRect(w->hwnd, &r)) return 0;
    return (int)(r.bottom - r.top);
}

/* 导出当前锚定显示器的工作区（调用方提供 RECT）。参数用 void* 以免本头
 * 文件引入 windows.h。返回 1 表示成功。 */
int um_toast_monitor_workarea(void *rc_work)
{
    RECT wa;
    if (!rc_work) return 0;
    monitor_workarea(&wa);
    *(RECT *)rc_work = wa;
    return 1;
}

void um_toast_win_update(um_toast_win *w, const um_toast_model *m)
{
    if (!w || !m) return;
    w->model = *m;
    apply_geometry(w);      /* 尺寸变化时同步窗口与锚点，再重画 */
    InvalidateRect(w->hwnd, NULL, FALSE);
}

void um_toast_win_show(um_toast_win *w)
{
    if (!w) return;
    um_toast_state_init(&w->state, w->state.ttl_ms);
    w->state.visible = 1;
    w->alpha = 0;
    w->last_sec = -1;
    SetLayeredWindowAttributes(w->hwnd, 0, 0, LWA_ALPHA);
    SetTimer(w->hwnd, UM_TIMER_FADE, 16, NULL);
    apply_geometry(w);      /* 每次展示都重锚右下角（多屏/分辨率可能变了） */
    SetWindowPos(w->hwnd, w->topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
    SetTimer(w->hwnd, UM_TIMER_TICK, UM_TICK_MS, NULL);
}

void um_toast_win_hide(um_toast_win *w)
{
    if (!w) return;
    w->state.visible = 0;
    KillTimer(w->hwnd, UM_TIMER_TICK);
    KillTimer(w->hwnd, UM_TIMER_FADE);
    ShowWindow(w->hwnd, SW_HIDE);
}

void um_toast_win_set_theme(um_toast_win *w, const um_theme *t)
{
    if (!w) return;
    w->theme = *t;
    InvalidateRect(w->hwnd, NULL, FALSE);
}

void um_toast_win_destroy(um_toast_win *w)
{
    if (!w) return;
    /* DestroyWindow 同步跑 WM_DESTROY（清 GWLP_USERDATA + KillTimer）后才返回。
     * 万一它失败（跨线程/已被系统销毁），也要手动清掉 userdata，绝不能让存活
     * 的 HWND 指向马上被 free 的 w。 */
    if (w->hwnd && !DestroyWindow(w->hwnd))
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    free(w);
}

/* ------------------------------------------------------- 外部可查询状态 -- */

int um_toast_win_is_visible(const um_toast_win *w)
{
    return (w && w->state.visible) ? 1 : 0;
}

int um_toast_win_height(const um_toast_win *w)
{
    return w ? w->height : 0;
}

int um_toast_win_is_expanded(const um_toast_win *w)
{
    return (w && w->state.expanded) ? 1 : 0;
}

void um_toast_win_toggle_expand(um_toast_win *w)
{
    if (!w) return;
    w->state.expanded = !w->state.expanded;
    apply_geometry(w);
    InvalidateRect(w->hwnd, NULL, FALSE);
}

/* 供宿主/真机冒烟测试直接向窗口投递消息（Esc 键、菜单注入等）。 */
void *um_toast_win_hwnd(const um_toast_win *w)
{
    return w ? (void *)w->hwnd : NULL;
}

void *um_toast_win_user(const um_toast_win *w)
{
    return w ? w->user : NULL;
}

const um_toast_state *um_toast_win_state(const um_toast_win *w)
{
    static um_toast_state nil;
    if (!w) { memset(&nil, 0, sizeof nil); nil.hover_row = -1; nil.focus_row = -1; return &nil; }
    return &w->state;
}

const um_toast_model *um_toast_win_model(const um_toast_win *w)
{
    static um_toast_model nil;
    if (!w) { memset(&nil, 0, sizeof nil); return &nil; }
    return &w->model;
}
