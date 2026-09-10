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
    int              width, height;     /* 逻辑像素 */
    int              dpi;
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

static int ui_scale(const struct um_toast_win *w, int v)
{
    return (int)(v * w->dpi / 96.0 + 0.5);
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

    fonts[UM_F_TITLE]    = make_font(16, w->dpi, 1, font_face());
    fonts[UM_F_BODY]     = make_font(13, w->dpi, 0, font_face());
    fonts[UM_F_SMALL]    = make_font(12, w->dpi, 0, font_face());
    fonts[UM_F_BTN]      = make_font(13, w->dpi, 1, font_face());
    fonts[UM_F_ROWTITLE] = make_font(13, w->dpi, 1, font_face());
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
static void apply_geometry(struct um_toast_win *w)
{
    RECT wa;
    int x, y;

    w->height = um_toast_measure_height(&w->model, &w->state);
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        x = wa.right - ui_scale(w, w->width) - ui_scale(w, UM_UI_MARGIN);
        y = wa.bottom - ui_scale(w, w->height) - ui_scale(w, UM_UI_MARGIN);
    } else {
        x = GetSystemMetrics(SM_CXSCREEN) - ui_scale(w, w->width) - 24;
        y = GetSystemMetrics(SM_CYSCREEN) - ui_scale(w, w->height) - 48;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    SetWindowPos(w->hwnd, NULL, x, y, ui_scale(w, w->width),
                 ui_scale(w, w->height), SWP_NOZORDER | SWP_NOACTIVATE);
    {
        HRGN rgn = CreateRoundRectRgn(0, 0, ui_scale(w, w->width) + 1,
                                      ui_scale(w, w->height) + 1,
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
    if (hit == UM_HIT_NONE) return;
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

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
        return 1;                       /* 双缓冲：不需要擦除背景 */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        if (w) paint_toast(w, hdc);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_TIMER:
        if (!w) return 0;
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
        if (!w) return 0;
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
                                x / (w->dpi / 96.0), y / (w->dpi / 96.0));
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
        if (!w) return 0;
        w->tracking = 0;
        w->state.hover_row = -1;
        w->state.hover_btn = 0;
        um_toast_resume(&w->state);      /* 离开后继续倒计时 */
        InvalidateRect(hw, NULL, FALSE);
        return 0;

    case WM_MOUSEHOVER:
        if (!w) return 0;
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
        if (!w) return 0;
        on_click(w, (int)((short)LOWORD(lp) / (w->dpi / 96.0)),
                 (int)((short)HIWORD(lp) / (w->dpi / 96.0)));
        return 0;

    case WM_RBUTTONUP: {
        int x = (int)((short)LOWORD(lp) / (w->dpi / 96.0));
        int y = (int)((short)HIWORD(lp) / (w->dpi / 96.0));
        um_ui_hit hit;
        if (!w) return 0;
        hit = um_toast_hit_test(&w->model, &w->state, w->width, w->height,
                                x, y);
        if (hit >= UM_HIT_ROW && hit < UM_HIT_ROW + UM_UI_MAX_ROWS)
            show_row_menu(w, (int)hit - UM_HIT_ROW, x, y);
        else
            fire(w, UM_ACT_CLOSE, -1);
        return 0;
    }

    case WM_KEYDOWN:
        if (!w) return 0;
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
        if (w) {
            KillTimer(hw, UM_TIMER_TICK);
            KillTimer(hw, UM_TIMER_FADE);
        }
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* --------------------------------------------------------------- 公开 API -- */

int um_toast_win_init(void *hinst)
{
    HINSTANCE h = (HINSTANCE)hinst;
    WNDCLASSW wc;
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
                               int topmost, um_toast_cb cb, void *user)
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
    um_toast_state_init(&w->state, 10000);
    w->width = UM_UI_WIDTH;
    w->height = um_toast_measure_height(&w->model, &w->state);

    hdc = GetDC(NULL);
    w->dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    if (w->dpi < 96) w->dpi = 96;
    ReleaseDC(NULL, hdc);

    pw = ui_scale(w, w->width);
    ph = ui_scale(w, w->height);

    ex = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (topmost) ex |= WS_EX_TOPMOST;

    w->hwnd = CreateWindowExW(ex, g_class, L"usbmon", WS_POPUP,
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
    if (w->hwnd) DestroyWindow(w->hwnd);
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
