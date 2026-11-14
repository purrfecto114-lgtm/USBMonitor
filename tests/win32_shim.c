/* win32_shim.c — Win32 仿真层实现（状态唯一，供 um_toast_win32.c 与
 * ui_test.c 两个编译单元共享）。
 */
#include "win32_shim.h"

/* ------------------------------------------------------------ 共享状态 -- */

shim_draw g_draws[SHIM_MAXDRAW];
int       g_ndraws;
HWND      g_painting;

static SHIM_WNDPROC g_wndproc;
static void        *g_win_user;
static int          g_win_visible;
static int          g_win_x, g_win_y, g_win_w, g_win_h;
static int          g_win_alpha;
static int          g_next_menu_cmd;
static int          g_menu_items;

static int g_font_px = 13, g_font_bold = 0;
static int g_font_px_arr[SHIM_MAXOBJ], g_font_bold_arr[SHIM_MAXOBJ];
static unsigned long g_text_color = 0x000000;
static unsigned long g_brush_color[SHIM_MAXOBJ];
static unsigned long g_pen_color[SHIM_MAXOBJ];
static int  g_next_obj;
static void *g_cur_brush;
static void *g_cur_pen;

/* 注入的"当前显示器"：默认 1920x1080 屏幕 / 1920x1040 工作区 / 96dpi。 */
static int g_screen_w = 1920, g_screen_h = 1080;
static int g_work_w = 1920, g_work_h = 1040;
static int g_dpi = 96;
static int g_font_px_log[16], g_font_log_n;

#define SHIM_WINOBJ ((HWND)0x1234)

static unsigned long obj_color(void *obj)
{
    LONG_PTR id = (LONG_PTR)obj;
    if (id >= 0x1000 && id < 0x2000) return g_brush_color[id - 0x1000];
    return 0;
}

/* ------------------------------------------------------------ 仿真 API --- */

/* 清空绘制记录 + 恢复默认显示器/DPI 注入（等价"干净的开场"）。 */
void shim_reset(void)
{
    g_ndraws = 0;
    g_font_log_n = 0;
    g_screen_w = 1920; g_screen_h = 1080;
    g_work_w = 1920; g_work_h = 1040;
    g_dpi = 96;
}

void shim_set_next_menu_result(int cmd) { g_next_menu_cmd = cmd; }

int shim_menu_item_count(void) { return g_menu_items; }

void *shim_userdata(HWND hw) { (void)hw; return g_win_user; }

int shim_visible(HWND hw) { (void)hw; return g_win_visible; }

void shim_window_geometry(int *x, int *y, int *w, int *h)
{
    if (x) *x = g_win_x;
    if (y) *y = g_win_y;
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
}

HWND shim_window(void *userdata, int w, int h)
{
    g_win_user = userdata;
    g_win_visible = 0;
    g_win_x = 100; g_win_y = 100; g_win_w = w; g_win_h = h;
    return SHIM_WINOBJ;
}

LRESULT shim_send(HWND hw, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_wndproc) return 0;
    return g_wndproc(hw, msg, w, l);
}

void shim_paint(HWND hw) { shim_send(hw, WM_PAINT, 0, 0); }

/* ------------------------------------------------------------- USER32 ---- */

int RegisterClassW(const WNDCLASSW *wc)
{
    g_wndproc = (SHIM_WNDPROC)wc->lpfnWndProc;
    return 1;
}

HWND CreateWindowExW(DWORD ex, const wchar_t *cls, const wchar_t *name,
                     DWORD style, int x, int y, int w, int h, HWND parent,
                     HMENU menu, HINSTANCE inst, LPVOID p)
{
    (void)ex; (void)cls; (void)name; (void)style; (void)parent; (void)menu;
    (void)inst; (void)p;
    g_win_x = x; g_win_y = y; g_win_w = w; g_win_h = h;
    return SHIM_WINOBJ;
}

HMODULE GetModuleHandleW(const wchar_t *n) { (void)n; return (HMODULE)1; }
void *LoadCursorW(HINSTANCE h, LPCWSTR n) { (void)h; (void)n; return (void *)1; }
void *GetStockObject(int i) { (void)i; return (void *)1; }

LONG_PTR SetWindowLongPtrW(HWND hw, int idx, LONG_PTR v)
{
    (void)hw; (void)idx; g_win_user = (void *)v; return 0;
}

LONG_PTR GetWindowLongPtrW(HWND hw, int idx)
{
    (void)hw; (void)idx; return (LONG_PTR)g_win_user;
}

UINT SetTimer(HWND hw, UINT id, UINT elapse, void *fn)
{
    (void)hw; (void)id; (void)elapse; (void)fn; return 1;
}

BOOL KillTimer(HWND hw, UINT id) { (void)hw; (void)id; return 1; }

BOOL ShowWindow(HWND hw, int cmd)
{
    (void)hw;
    g_win_visible = (cmd == SW_HIDE) ? 0 : 1;
    return 1;
}

BOOL InvalidateRect(HWND hw, const RECT *r, BOOL e)
{
    (void)hw; (void)r; (void)e; return 1;
}

BOOL SetWindowPos(HWND hw, HWND after, int x, int y, int w, int h, UINT f)
{
    (void)hw; (void)after;
    if (!(f & SWP_NOMOVE)) { g_win_x = x; g_win_y = y; }
    if (!(f & SWP_NOSIZE)) { g_win_w = w; g_win_h = h; }
    return 1;
}

BOOL SetForegroundWindow(HWND hw) { (void)hw; return 1; }

BOOL PostMessageW(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)hw; (void)msg; (void)wp; (void)lp;
    return 1;
}

HRGN CreateRoundRectRgn(int a, int b, int c, int d, int e, int f)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return (HRGN)malloc(1);
}

int SetWindowRgn(HWND hw, HRGN r, BOOL redraw)
{
    /* 真 Win32 语义：SetWindowRgn 成功后区域归系统所有（调用方不再释放）。
     * shim 里区域是 malloc(1)——在这里释放，否则 ASan 泄漏检查报 6×1B
     * 假泄漏（真机上不是泄漏，但测试基建应当干净）。 */
    (void)hw; (void)redraw;
    free(r);
    return 1;
}

BOOL SetLayeredWindowAttributes(HWND hw, COLORREF key, BYTE a, DWORD f)
{
    (void)hw; (void)key; (void)f; g_win_alpha = a; return 1;
}

BOOL TrackMouseEvent(TRACKMOUSEEVENT *t) { (void)t; return 1; }

BOOL ClientToScreen(HWND hw, POINT *p) { (void)hw; (void)p; return 1; }

/* 注入的"当前显示器"：默认 1920x1080 屏幕 / 1920x1040 工作区 / 96dpi。 */
void shim_set_workarea(int w, int h)
{
    if (w < 240) w = 240;
    if (h < 200) h = 200;
    g_screen_w = g_work_w = w;
    g_screen_h = g_work_h = h;
}

void shim_set_dpi(int dpi) { g_dpi = (dpi >= 16) ? dpi : 96; }

int shim_font_log_count(void) { return g_font_log_n; }

int shim_font_log_px(int idx)
{
    return (idx >= 0 && idx < g_font_log_n) ? g_font_px_log[idx] : -1;
}

BOOL SystemParametersInfoW(UINT a, UINT b, void *c, UINT d)
{
    (void)b; (void)d;
    if (a == SPI_GETWORKAREA && c) {
        RECT *r = (RECT *)c;
        r->left = 0; r->top = 0; r->right = g_work_w; r->bottom = g_work_h;
        return 1;
    }
    return 0;
}

int GetSystemMetrics(int i) { return i == SM_CXSCREEN ? g_screen_w : g_screen_h; }
HDC GetDC(HWND hw) { (void)hw; return (HDC)1; }
int ReleaseDC(HWND hw, HDC dc) { (void)hw; (void)dc; return 1; }
int GetDeviceCaps(HDC dc, int idx) { (void)dc; return (idx == LOGPIXELSY) ? g_dpi : 96; }

/* 只有一块"屏"：MonitorFromPoint 命中工作区内的点，否则返回 NULL
 * （生产代码据此回退 SPI_GETWORKAREA）。 */
BOOL GetCursorPos(POINT *pt)
{
    if (!pt) return 0;
    pt->x = 0; pt->y = 0;          /* headless：等价桌面 (0,0) */
    return 1;
}

HMONITOR MonitorFromPoint(POINT pt, DWORD flags)
{
    (void)flags;
    return ((pt.x >= 0 && pt.x < g_work_w && pt.y >= 0 && pt.y < g_work_h)
            ? (HMONITOR)(LONG_PTR)1 : NULL);
}

BOOL GetMonitorInfoA(HMONITOR mon, MONITORINFO *mi)
{
    if (!mon || !mi || mi->cbSize != (DWORD)sizeof *mi) return 0;
    mi->rcMonitor.left = 0; mi->rcMonitor.top = 0;
    mi->rcMonitor.right = g_screen_w; mi->rcMonitor.bottom = g_screen_h;
    mi->rcWork = mi->rcMonitor;
    mi->rcWork.right = g_work_w; mi->rcWork.bottom = g_work_h;
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

BOOL DestroyWindow(HWND hw) { (void)hw; g_win_visible = 0; return 1; }

LRESULT DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h; (void)m; (void)w; (void)l; return 0;
}

/* --------------------------------------------------------------- GDI32 ---- */

HBRUSH CreateSolidBrush(COLORREF c)
{
    int id = g_next_obj++ % SHIM_MAXOBJ;
    g_brush_color[id] = (unsigned long)c;
    return (HBRUSH)(LONG_PTR)(0x1000 + id);
}

HPEN CreatePen(int style, int w, COLORREF c)
{
    int id = g_next_obj++ % SHIM_MAXOBJ;
    (void)style; (void)w;
    g_pen_color[id] = (unsigned long)c;
    return (HPEN)(LONG_PTR)(0x2000 + id);
}

HFONT CreateFontW(int h, int w, int esc, int orient, int weight, DWORD it,
                  DWORD ul, DWORD st, DWORD charset, DWORD outp, DWORD clip,
                  DWORD quality, DWORD pitch, LPCWSTR face)
{
    int id = g_next_obj++ % SHIM_MAXOBJ;
    (void)w; (void)esc; (void)orient; (void)it; (void)ul; (void)st;
    (void)charset; (void)outp; (void)clip; (void)quality; (void)pitch;
    (void)face;
    g_font_px_arr[id] = h < 0 ? -h : h;
    g_font_bold_arr[id] = (weight >= 600);
    /* 字号日志：paint_toast 里 UM_F_TITLE..UM_F_ROWTITLE 的创建顺序 = 索引。 */
    if (g_font_log_n < 16) g_font_px_log[g_font_log_n++] = g_font_px_arr[id];
    return (HFONT)(LONG_PTR)(0x3000 + id);
}

void DeleteObject(void *o) { (void)o; }

void *SelectObject(HDC dc, void *obj)
{
    LONG_PTR id = (LONG_PTR)obj;
    int k;
    (void)dc;
    if (id >= 0x1000 && id < 0x2000)      g_cur_brush = obj;
    else if (id >= 0x2000 && id < 0x3000) g_cur_pen = obj;
    else if (id >= 0x3000) {
        k = (int)(id - 0x3000);
        if (k >= 0 && k < SHIM_MAXOBJ) {
            g_font_px = g_font_px_arr[k];
            g_font_bold = g_font_bold_arr[k];
        }
    }
    return (void *)1;
}

int SetBkMode(HDC dc, int mode) { (void)dc; (void)mode; return 0; }

COLORREF SetTextColor(HDC dc, COLORREF c)
{
    (void)dc; g_text_color = (unsigned long)c; return c;
}

static shim_draw *new_draw(int kind)
{
    shim_draw *d;
    if (g_ndraws >= SHIM_MAXDRAW) return NULL;
    d = &g_draws[g_ndraws++];
    memset(d, 0, sizeof *d);
    d->kind = kind;
    return d;
}

int FillRect(HDC dc, const RECT *rc, HBRUSH br)
{
    shim_draw *d;
    (void)dc;
    d = new_draw(0);
    if (!d) return 0;
    d->x = (int)rc->left; d->y = (int)rc->top;
    d->w = (int)(rc->right - rc->left); d->h = (int)(rc->bottom - rc->top);
    d->fill = ((LONG_PTR)br >= 0x1000) ? obj_color(br) : 0;
    return 1;
}

BOOL RoundRect(HDC dc, int x1, int y1, int x2, int y2, int w, int h)
{
    shim_draw *d;
    (void)dc;
    d = new_draw(1);
    if (!d) return 0;
    d->x = x1; d->y = y1; d->w = x2 - x1; d->h = y2 - y1;
    d->r = w > h ? h : w;
    d->fill = obj_color(g_cur_brush);
    d->stroke_col = ((LONG_PTR)g_cur_pen >= 0x2000)
                    ? g_pen_color[((LONG_PTR)g_cur_pen - 0x2000) % SHIM_MAXOBJ]
                    : obj_color(g_cur_brush);
    d->stroke = (d->fill == 0) ? 1 : 0;
    return 1;
}

BOOL Ellipse(HDC dc, int x1, int y1, int x2, int y2)
{
    shim_draw *d;
    (void)dc;
    d = new_draw(3);
    if (!d) return 0;
    d->x = x1; d->y = y1; d->w = x2 - x1; d->h = y2 - y1;
    d->fill = obj_color(g_cur_brush);
    return 1;
}

BOOL Rectangle(HDC dc, int x1, int y1, int x2, int y2)
{
    RECT rc;
    rc.left = x1; rc.top = y1; rc.right = x2; rc.bottom = y2;
    return FillRect(dc, &rc, (HBRUSH)g_cur_brush);
}

BOOL MoveToEx(HDC dc, int x, int y, POINT *old)
{
    shim_draw *d;
    (void)dc; (void)old;
    d = new_draw(4);
    if (!d) return 0;
    d->x = x; d->y = y;
    return 1;
}

BOOL LineTo(HDC dc, int x, int y)
{
    shim_draw *d;
    (void)dc;
    if (g_ndraws > 0 && g_draws[g_ndraws - 1].kind == 4 &&
        g_draws[g_ndraws - 1].w == 0 && g_draws[g_ndraws - 1].h == 0) {
        d = &g_draws[g_ndraws - 1];
        d->w = x - d->x; d->h = y - d->y;
        d->stroke_col = g_pen_color[(g_next_obj - 1 + SHIM_MAXOBJ) % SHIM_MAXOBJ];
        return 1;
    }
    d = new_draw(4);
    if (!d) return 0;
    d->x = x; d->y = y;
    return 1;
}

/* 文本实测宽度：ASCII ≈ 0.55×px、CJK/全角 ≈ 1.0×px。够覆盖
 * suggest_width 的夹取断言，且无需真字体。 */
BOOL GetTextExtentPoint32W(HDC dc, const uint16_t *text, int len, SIZE *sz)
{
    int i, wid = 0;
    (void)dc;
    if (!sz) return 0;
    sz->cy = g_font_px + g_font_px / 3;
    if (text) {
        for (i = 0; i < len && text[i]; i++)
            wid += (text[i] >= 0x2E80) ? g_font_px : (g_font_px * 55 / 100);
    }
    sz->cx = wid;
    return 1;
}

int DrawTextW(HDC dc, const uint16_t *text, int len, RECT *rc, UINT fmt)
{
    shim_draw *d;
    size_t o = 0;
    (void)dc; (void)len;
    d = new_draw(2);
    if (!d) return 0;
    d->x = (int)rc->left; d->y = (int)rc->top;
    d->w = (int)(rc->right - rc->left); d->h = (int)(rc->bottom - rc->top);
    d->fill = g_text_color;
    d->fontpx = g_font_px;
    d->bold = g_font_bold;
    d->align = (fmt & DT_RIGHT) ? 1 : ((fmt & DT_CENTER) ? 2 : 0);
    /* UTF-16 → UTF-8：代理对必须合并成一个码点，否则 emoji 会变两坨乱码 */
    for (; *text && o + 4 < sizeof d->text; text++) {
        unsigned long c = *text;
        if (c >= 0xD800 && c <= 0xDBFF && text[1] >= 0xDC00 && text[1] <= 0xDFFF) {
            unsigned long lo = text[1];
            text++;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c < 0x80) d->text[o++] = (char)c;
        else if (c < 0x800) {
            d->text[o++] = (char)(0xC0 | (c >> 6));
            d->text[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            d->text[o++] = (char)(0xE0 | (c >> 12));
            d->text[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            d->text[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            d->text[o++] = (char)(0xF0 | (c >> 18));
            d->text[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            d->text[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            d->text[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    d->text[o] = 0;
    return 1;
}

HDC CreateCompatibleDC(HDC dc) { (void)dc; return (HDC)2; }

HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h)
{
    (void)dc; (void)w; (void)h; return (HBITMAP)1;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy,
            DWORD op)
{
    (void)dst; (void)x; (void)y; (void)w; (void)h; (void)src; (void)sx;
    (void)sy; (void)op;
    return 1;
}

BOOL DeleteDC(HDC dc) { (void)dc; return 1; }

HDC BeginPaint(HWND hw, PAINTSTRUCT *ps)
{
    (void)hw;
    memset(ps, 0, sizeof *ps);
    ps->hdc = (HDC)1;
    g_ndraws = 0;
    g_painting = hw;
    return ps->hdc;
}

BOOL EndPaint(HWND hw, const PAINTSTRUCT *ps) { (void)hw; (void)ps; return 1; }

/* ---------------------------------------------------------------- 菜单 ---- */

HMENU CreatePopupMenu(void) { g_menu_items = 0; return (HMENU)1; }

BOOL AppendMenuW(HMENU m, UINT f, unsigned long long id, LPCWSTR t)
{
    (void)m; (void)f; (void)id; (void)t;
    g_menu_items++;
    return 1;
}

BOOL DestroyMenu(HMENU m) { (void)m; return 1; }

int TrackPopupMenuEx(HMENU m, UINT f, int x, int y, HWND hw, void *p)
{
    (void)m; (void)f; (void)x; (void)y; (void)hw; (void)p;
    return g_next_menu_cmd;
}

/* ------------------------------------------------------------ SVG 导出 --- */

static void hexcol(char *out, unsigned long c)
{
    unsigned long r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    snprintf(out, 8, "#%02X%02X%02X", (unsigned)r, (unsigned)g, (unsigned)b);
}

static void svg_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 8 < n; in++) {
        if (*in == '&')      o += (size_t)snprintf(out + o, n - o, "&amp;");
        else if (*in == '<') o += (size_t)snprintf(out + o, n - o, "&lt;");
        else if (*in == '>') o += (size_t)snprintf(out + o, n - o, "&gt;");
        else out[o++] = *in;
    }
    out[o] = 0;
}

void shim_svg(const char *path, int width, int height, const char *bg)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
        "viewBox=\"0 0 %d %d\">\n"
        "<rect width=\"%d\" height=\"%d\" fill=\"%s\"/>\n",
        width, height, width, height, width, height, bg);
    for (i = 0; i < g_ndraws; i++) {
        const shim_draw *d = &g_draws[i];
        char c1[8], c2[8];
        hexcol(c1, d->fill);
        hexcol(c2, d->stroke_col);
        if (d->kind == 0) {
            fprintf(f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"%s\"/>\n",
                    d->x, d->y, d->w, d->h, c1);
        } else if (d->kind == 1) {
            fprintf(f,
                "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" "
                "fill=\"%s\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                d->x, d->y, d->w, d->h, d->r,
                d->stroke ? "none" : c1, c2);
        } else if (d->kind == 3) {
            fprintf(f,
                "<ellipse cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\" fill=\"%s\"/>\n",
                d->x + d->w / 2, d->y + d->h / 2, d->w / 2, d->h / 2, c1);
        } else if (d->kind == 4) {
            fprintf(f,
                "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"%s\" "
                "stroke-width=\"2\" stroke-linecap=\"round\"/>\n",
                d->x, d->y, d->x + d->w, d->y + d->h, c2);
        } else if (d->kind == 2) {
            char esc[512];
            const char *anchor = d->align == 1 ? "end"
                               : (d->align == 2 ? "middle" : "start");
            int tx = d->x;
            svg_escape(d->text, esc, sizeof esc);
            if (d->align == 1) tx = d->x + d->w;
            else if (d->align == 2) tx = d->x + d->w / 2;
            fprintf(f,
                "<clipPath id=\"text%d\"><rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"/>"
                "</clipPath>\n"
                "<text x=\"%d\" y=\"%d\" fill=\"%s\" font-family=\"Noto Sans CJK SC,"
                "WenQuanYi Zen Hei,DejaVu Sans,sans-serif\" font-size=\"%d\" "
                "font-weight=\"%d\" text-anchor=\"%s\" clip-path=\"url(#text%d)\">%s</text>\n",
                i, d->x, d->y, d->w, d->h,
                tx, d->y + (d->fontpx ? d->fontpx : 13), c1, d->fontpx ? d->fontpx : 13,
                d->bold ? 600 : 400, anchor, i, esc);
        }
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

/* Device-world adapter. The inventory uses the same enumeration and IO code
 * in these tests as in the Windows build. */
#include "usbmon.h"

#define SHIM_MAX_DEV 128

static shim_dev world[SHIM_MAX_DEV];
static int world_n, enum_fails, eject_veto;
static DWORD last_error;
static DEVINST last_eject;

void shim_set_world(const shim_dev *devs, int n)
{
    if (n > SHIM_MAX_DEV) n = SHIM_MAX_DEV;
    if (n) memcpy(world, devs, (size_t)n * sizeof *devs);
    world_n = n;
    last_eject = 0;
    eject_veto = 0;
    enum_fails = 0;
}

void shim_enum_failure(int fail) { enum_fails = fail; }
void shim_eject_veto(int veto) { eject_veto = veto; }
DEVINST shim_last_eject(void) { return last_eject; }
DWORD GetLastError(void) { return last_error; }
BOOL SetThreadErrorMode(DWORD mode, DWORD *old)
{
    (void)mode;
    if (old) *old = 0;
    return 1;
}

void um_utf8_to_wide(const char *in, wchar_t *out, size_t n)
{
    size_t k;
    for (k = 0; k + 1 < n && in[k]; k++) out[k] = (unsigned char)in[k];
    if (n) out[k] = L'\0';
}

void um_wide_to_utf8(const wchar_t *in, char *out, size_t n)
{
    size_t k;
    for (k = 0; k + 1 < n && in[k]; k++) out[k] = (char)in[k];
    if (n) out[k] = '\0';
}

HDEVINFO SetupDiGetClassDevsW(const GUID *guid, const wchar_t *enumerator,
                              HWND hwnd, DWORD flags)
{
    (void)enumerator; (void)hwnd; (void)flags;
    return enum_fails ? INVALID_HANDLE_VALUE : (void *)guid;
}

BOOL SetupDiEnumDeviceInterfaces(HDEVINFO set, void *a, const GUID *guid,
                                 DWORD idx, SP_DEVICE_INTERFACE_DATA *d)
{
    int i;
    (void)a; (void)guid;
    for (i = 0; i < world_n; i++) {
        if (world[i].iface == (int)(uintptr_t)set && idx-- == 0) {
            d->index = i;
            return 1;
        }
    }
    last_error = ERROR_NO_MORE_ITEMS;
    return 0;
}

BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO set, SP_DEVICE_INTERFACE_DATA *d,
                                      SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail,
                                      DWORD sz, DWORD *need, SP_DEVINFO_DATA *info)
{
    const char *path = world[d->index].device_path;
    DWORD size = (DWORD)(sizeof *detail + strlen(path) * sizeof(wchar_t));
    (void)set;
    if (need) *need = size;
    if (!detail || sz < size) {
        last_error = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    um_utf8_to_wide(path, detail->DevicePath, strlen(path) + 1);
    if (info) info->DevInst = (DEVINST)d->index + 1;
    return 1;
}

BOOL SetupDiGetDeviceRegistryPropertyW(HDEVINFO set, SP_DEVINFO_DATA *info, DWORD prop,
                                       DWORD *type, BYTE *buf, DWORD size, DWORD *need)
{
    (void)set; (void)info; (void)prop; (void)type; (void)buf; (void)size; (void)need;
    return 0;
}

BOOL SetupDiDestroyDeviceInfoList(HDEVINFO set) { (void)set; return 1; }

static DEVINST node_of(const char *id)
{
    int i;
    for (i = 0; i < world_n; i++) {
        if (world[i].instance_id && !strcmp(id, world[i].instance_id)) return (DEVINST)i + 1;
    }
    return 0;
}

CONFIGRET CM_Get_Parent(DEVINST *parent, DEVINST child, DWORD flags)
{
    (void)flags;
    if (!child || child > (DEVINST)world_n || !world[child - 1].parent_inst)
        return CR_NO_SUCH_DEVNODE;
    *parent = node_of(world[child - 1].parent_inst);
    return *parent ? CR_SUCCESS : CR_NO_SUCH_DEVNODE;
}

CONFIGRET CM_Get_Device_IDW(DEVINST inst, wchar_t *buf, DWORD n, DWORD flags)
{
    (void)flags;
    if (!inst || inst > (DEVINST)world_n) return CR_NO_SUCH_DEVNODE;
    um_utf8_to_wide(world[inst - 1].instance_id, buf, n);
    return CR_SUCCESS;
}

CONFIGRET CM_Locate_DevNodeW(DEVINST *inst, wchar_t *id, DWORD flags)
{
    char utf8[MAX_DEVICE_ID_LEN];
    (void)flags;
    um_wide_to_utf8(id, utf8, sizeof utf8);
    *inst = node_of(utf8);
    return *inst ? CR_SUCCESS : CR_NO_SUCH_DEVNODE;
}

CONFIGRET CM_Get_DevNode_Registry_PropertyW(DEVINST inst, DWORD prop, DWORD *type,
                                           void *out, DWORD *size, DWORD flags)
{
    (void)type; (void)flags;
    if (!inst || inst > (DEVINST)world_n || prop != CM_DRP_CAPABILITIES || *size < sizeof(DWORD))
        return CR_NO_SUCH_DEVNODE;
    *(DWORD *)out = world[inst - 1].removable ? CM_DEVCAP_REMOVABLE : 0;
    *size = sizeof(DWORD);
    return CR_SUCCESS;
}

CONFIGRET CM_Request_Device_EjectW(DEVINST inst, void *type, wchar_t *name, DWORD n, DWORD flags)
{
    (void)type; (void)name; (void)n; (void)flags;
    last_eject = inst;
    return eject_veto ? CR_REMOVE_VETOED : CR_SUCCESS;
}

static int volume_path(const wchar_t *path, int slash)
{
    char utf8[MAX_PATH];
    int i;
    um_wide_to_utf8(path, utf8, sizeof utf8);
    for (i = 0; i < world_n; i++) {
        size_t len;
        if (world[i].iface != IF_VOLUME) continue;
        len = strlen(world[i].device_path);
        if (strncmp(utf8, world[i].device_path, len)) continue;
        if (slash ? (!strcmp(utf8 + len, "\\")) : !utf8[len]) return i;
    }
    return -1;
}

BOOL FindNextVolumeW(HANDLE h, wchar_t *buf, DWORD n)
{
    int *index = h;
    while (++*index < world_n) {
        if (world[*index].iface == IF_VOLUME) {
            size_t len;
            um_utf8_to_wide(world[*index].device_path, buf, n);
            len = wcslen(buf);
            if (len + 1 >= n) return 0;
            buf[len] = L'\\';
            buf[len + 1] = L'\0';
            return 1;
        }
    }
    return 0;
}

HANDLE FindFirstVolumeW(wchar_t *buf, DWORD n)
{
    int *index = malloc(sizeof *index);
    if (!index) return INVALID_HANDLE_VALUE;
    *index = -1;
    if (FindNextVolumeW(index, buf, n)) return index;
    free(index);
    return INVALID_HANDLE_VALUE;
}

BOOL FindVolumeClose(HANDLE h) { free(h); return 1; }

BOOL GetVolumePathNamesForVolumeNameW(const wchar_t *vol, wchar_t *names, DWORD n, DWORD *ret)
{
    int index = volume_path(vol, 1);
    size_t used = 0;
    const char *path;
    if (index < 0 || !n) return 0;
    path = world[index].letters;
    if (path && strlen(path) > 2 && path[2] == '\\') {
        used = strlen(path) + 1;
        if (used + 1 > n) {
            last_error = ERROR_MORE_DATA;
            if (ret) *ret = (DWORD)used + 1;
            return 0;
        }
        um_utf8_to_wide(path, names, n);
    } else {
        while (path && *path && used + 4 < n) {
            names[used++] = (wchar_t)*path;
            names[used++] = L':';
            names[used++] = L'\\';
            names[used++] = L'\0';
            path += 2;
        }
    }
    names[used++] = L'\0';
    if (ret) *ret = (DWORD)used;
    return 1;
}

static int volume_at_letter(char letter)
{
    int i;
    for (i = 0; i < world_n; i++) {
        const char *p = world[i].letters;
        if (world[i].iface != IF_VOLUME || !p) continue;
        if (strlen(p) > 2 && p[2] == '\\') continue;
        for (; *p; p += 2) if (*p == letter) return i;
    }
    return -1;
}

BOOL GetDiskFreeSpaceExW(const wchar_t *path, ULARGE_INTEGER *available,
                         ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes)
{
    int index = wcslen(path) == 3 ? volume_at_letter((char)path[0]) : -1;
    if (index < 0) {
        char utf8[MAX_PATH];
        int i;
        um_wide_to_utf8(path, utf8, sizeof utf8);
        for (i = 0; i < world_n; i++)
            if (world[i].iface == IF_VOLUME && world[i].letters && !strcmp(world[i].letters, utf8)) {
                index = i;
                break;
            }
    }
    if (index < 0 || !world[index].unlocked) return 0;
    if (available) available->QuadPart = 100000;
    if (total) total->QuadPart = 200000;
    if (free_bytes) free_bytes->QuadPart = 100000;
    return 1;
}

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share,
                   void *sa, DWORD disp, DWORD flags, void *tmpl)
{
    char utf8[MAX_PATH];
    int index, i;
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)tmpl;
    um_wide_to_utf8(path, utf8, sizeof utf8);
    index = volume_path(path, 0);
    if (index < 0 && strlen(utf8) == 6 && !strncmp(utf8, "\\\\.\\", 4))
        index = volume_at_letter(utf8[4]);
    for (i = 0; index < 0 && i < world_n; i++)
        if (world[i].iface != IF_VOLUME && !strcmp(utf8, world[i].device_path)) index = i;
    if (index < 0) return INVALID_HANDLE_VALUE;
    return &world[index];
}

BOOL DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD in_sz,
                      void *out, DWORD out_sz, DWORD *ret, void *ov)
{
    const shim_dev *dev = h;
    (void)in; (void)in_sz; (void)ov;
    if (h == INVALID_HANDLE_VALUE || dev->disk_number < 0) return 0;
    *ret = 0;
    switch (code) {
    case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS: {
        VOLUME_DISK_EXTENTS ext = {0};
        if (dev->iface != IF_VOLUME || out_sz < sizeof ext) return 0;
        ext.NumberOfDiskExtents = dev->media_present == 2 ? 2 : 1;
        ext.Extents[0].DiskNumber = (DWORD)dev->disk_number;
        memcpy(out, &ext, sizeof ext);
        *ret = sizeof ext;
        return 1;
    }
    case IOCTL_STORAGE_GET_DEVICE_NUMBER: {
        STORAGE_DEVICE_NUMBER num = {0, (DWORD)dev->disk_number, 0};
        if (out_sz < sizeof num) return 0;
        memcpy(out, &num, sizeof num);
        *ret = sizeof num;
        return 1;
    }
    case IOCTL_STORAGE_QUERY_PROPERTY: {
        STORAGE_DEVICE_DESCRIPTOR desc = {0};
        if (out_sz < sizeof desc) return 0;
        desc.Size = sizeof desc;
        desc.RemovableMedia = dev->removable != 0;
        desc.BusType = (DWORD)dev->bus_type;
        memcpy(out, &desc, sizeof desc);
        *ret = sizeof desc;
        return 1;
    }
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: {
        DISK_GEOMETRY_EX geo = {0};
        if (!dev->media_present || out_sz < sizeof geo) return 0;
        geo.DiskSize.QuadPart = 640000;
        memcpy(out, &geo, sizeof geo);
        *ret = sizeof geo;
        return 1;
    }
    case IOCTL_STORAGE_CHECK_VERIFY2:
        if (dev->media_present) return 1;
        last_error = ERROR_NO_MEDIA_IN_DRIVE;
        return 0;
    default: return 0;
    }
}

BOOL CloseHandle(HANDLE h) { (void)h; return 1; }
