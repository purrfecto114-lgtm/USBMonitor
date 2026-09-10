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

#define SHIM_WINOBJ ((HWND)0x1234)

static unsigned long obj_color(void *obj)
{
    LONG_PTR id = (LONG_PTR)obj;
    if (id >= 0x1000 && id < 0x2000) return g_brush_color[id - 0x1000];
    return 0;
}

/* ------------------------------------------------------------ 仿真 API --- */

void shim_reset(void) { g_ndraws = 0; }

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

BOOL SystemParametersInfoW(UINT a, UINT b, void *c, UINT d)
{
    (void)b; (void)d;
    if (a == SPI_GETWORKAREA && c) {
        RECT *r = (RECT *)c;
        r->left = 0; r->top = 0; r->right = 1920; r->bottom = 1040;
        return 1;
    }
    return 0;
}

int GetSystemMetrics(int i) { return i == SM_CXSCREEN ? 1920 : 1080; }
HDC GetDC(HWND hw) { (void)hw; return (HDC)1; }
int ReleaseDC(HWND hw, HDC dc) { (void)hw; (void)dc; return 1; }
int GetDeviceCaps(HDC dc, int idx) { (void)dc; (void)idx; return 96; }
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
                "<text x=\"%d\" y=\"%d\" fill=\"%s\" font-family=\"Noto Sans CJK SC,"
                "WenQuanYi Micro Hei,DejaVu Sans,sans-serif\" font-size=\"%d\" "
                "font-weight=\"%d\" text-anchor=\"%s\" "
                "style=\"dominant-baseline:hanging\">%s</text>\n",
                tx, d->y + 2, c1, d->fontpx ? d->fontpx : 13,
                d->bold ? 600 : 400, anchor, esc);
        }
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

/* ==========================================================================
 * 设备枚举仿真：让 src/um_enum.c 这份生产代码能在 Linux 上被真实驱动。
 * ========================================================================== */

#define SHIM_MAX_DEV 32

static shim_dev g_world[SHIM_MAX_DEV];
static int      g_world_n = 0;

void shim_set_world(const shim_dev *devs, int n)
{
    if (n > SHIM_MAX_DEV) n = SHIM_MAX_DEV;
    memcpy(g_world, devs, (size_t)n * sizeof(shim_dev));
    g_world_n = n;
}

/* DevInfo 集合：用接口类过滤器 + 游标表示 */
typedef struct {
    int iface;
    int cur;
} shim_set;

HDEVINFO SetupDiGetClassDevsW(const void *guid, const wchar_t *enumerator,
                              void *hwnd, DWORD flags)
{
    shim_set *s;
    (void)enumerator; (void)hwnd; (void)flags;
    s = (shim_set *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->iface = (int)(ULONG_PTR)guid;    /* 测试里用代号代替 GUID 指针 */
    s->cur = 0;
    return (HDEVINFO)s;
}

int SetupDiEnumDeviceInterfaces(HDEVINFO set, void *a,
                                const void *guid, DWORD idx,
                                SP_DEVICE_INTERFACE_DATA_SHIM *d)
{
    shim_set *s = (shim_set *)set;
    int i, seen = 0;
    (void)a; (void)guid;
    if (!s) return 0;
    for (i = 0; i < g_world_n; i++) {
        if (g_world[i].iface != s->iface) continue;
        if (seen++ == (int)idx) {
            if (d) { d->cbSize = sizeof(*d); d->Flags = 0; }
            return 1;
        }
    }
    return 0;
}

int SetupDiGetDeviceInterfaceDetailW(HDEVINFO set,
                                     SP_DEVICE_INTERFACE_DATA_SHIM *d,
                                     void *detail, DWORD sz,
                                     DWORD *need, void *devinfo)
{
    shim_set *s = (shim_set *)set;
    int i, seen = 0;
    (void)d; (void)sz; (void)devinfo;
    if (!s || !detail) return 0;
    for (i = 0; i < g_world_n; i++) {
        if (g_world[i].iface != s->iface) continue;
        if (seen++ == s->cur) {
            /* detail 布局：前 8 字节是 cbSize(DWORD)+pad，后接 wchar 路径 */
            char *p = (char *)detail;
            DWORD cb = 8;
            memcpy(p, &cb, sizeof cb);
            /* 用 UTF-8 直接放进宽字符缓冲（测试世界只含 ASCII 路径足够） */
            {
                wchar_t *wp = (wchar_t *)(p + 8);
                const char *src = g_world[i].device_path;
                int k = 0;
                for (; src && src[k] && k < 250; k++) wp[k] = (wchar_t)src[k];
                wp[k] = 0;
            }
            s->cur++;
            if (need) *need = 8;
            return 1;
        }
    }
    return 0;
}

int SetupDiDestroyDeviceInfoList(HDEVINFO set)
{
    free(set);
    return 1;
}

/* 仿真里用"世界数组下标 + 1"充当 DEVINST（0 保留为无效） */
CONFIGRET CM_Get_Parent(DEVINST *parent, DEVINST child, DWORD flags)
{
    int i;
    (void)flags;
    if (!parent || child == 0 || child > (DEVINST)g_world_n) return 1;
    for (i = 0; i < g_world_n; i++) {
        const shim_dev *p;
        int j;
        if (!g_world[i].parent_inst) continue;
        /* 找到 child 对应的设备，再看它的 parent_inst 指向谁 */
        if ((DEVINST)(i + 1) != child) continue;
        p = &g_world[i];
        for (j = 0; j < g_world_n; j++) {
            if (strcmp(g_world[j].instance_id, p->parent_inst) == 0) {
                *parent = (DEVINST)(j + 1);
                return CR_SUCCESS;
            }
        }
        return 1;
    }
    return 1;
}

CONFIGRET CM_Get_Device_IDW(DEVINST inst, wchar_t *buf, DWORD n, DWORD flags)
{
    const char *src;
    int k;
    (void)flags;
    if (!buf || n == 0 || inst == 0 || inst > (DEVINST)g_world_n) return 1;
    src = g_world[inst - 1].instance_id;
    for (k = 0; src[k] && k < (int)n - 1; k++) buf[k] = (wchar_t)src[k];
    buf[k] = 0;
    return CR_SUCCESS;
}

/* 供 um_enum 反查：按实例 ID 找 DEVINST */
DEVINST shim_devnode_of(const char *instance_id)
{
    int i;
    if (!instance_id) return 0;
    for (i = 0; i < g_world_n; i++)
        if (strcmp(g_world[i].instance_id, instance_id) == 0)
            return (DEVINST)(i + 1);
    return 0;
}

/* --- 卷枚举：把世界里 IF_VOLUME 的设备当作卷返回 --- */

typedef struct { int cur; } shim_volfind;

void *FindFirstVolumeW(wchar_t *buf, DWORD n)
{
    shim_volfind *f = (shim_volfind *)calloc(1, sizeof *f);
    int i;
    (void)n;
    if (!f) return INVALID_HANDLE_VALUE;
    /* 定位第一个卷类设备 */
    for (i = 0; i < g_world_n; i++) {
        if (g_world[i].iface == IF_VOLUME) {
            f->cur = i;
            if (buf) {
                int k;
                for (k = 0; g_world[i].device_path[k] && k < 250; k++)
                    buf[k] = (wchar_t)g_world[i].device_path[k];
                buf[k] = 0;
            }
            return (void *)f;
        }
    }
    free(f);
    return INVALID_HANDLE_VALUE;
}

int FindNextVolumeW(void *h, wchar_t *buf, DWORD n)
{
    shim_volfind *f = (shim_volfind *)h;
    int i;
    (void)n;
    if (!f || h == INVALID_HANDLE_VALUE) return 0;
    for (i = f->cur + 1; i < g_world_n; i++) {
        if (g_world[i].iface == IF_VOLUME) {
            f->cur = i;
            if (buf) {
                int k;
                for (k = 0; g_world[i].device_path[k] && k < 250; k++)
                    buf[k] = (wchar_t)g_world[i].device_path[k];
                buf[k] = 0;
            }
            return 1;
        }
    }
    return 0;
}

int FindVolumeClose(void *h)
{
    free(h);
    return 1;
}

int GetVolumePathNamesForVolumeNameW(const wchar_t *vol,
                                     wchar_t *names, DWORD n, DWORD *ret)
{
    int i, o = 0;
    (void)n;
    if (!vol || !names) return 0;
    for (i = 0; i < g_world_n; i++) {
        const shim_dev *d = &g_world[i];
        int match = 1, j;
        if (d->iface != IF_VOLUME) continue;
        for (j = 0; d->device_path[j]; j++) {
            if ((wchar_t)d->device_path[j] != vol[j]) { match = 0; break; }
        }
        if (!match || vol[j]) continue;
        /* 生成 "E:\0F:\0G:\0\0" 形式 */
        for (j = 0; d->letters && d->letters[j]; j += 2) {
            if (o + 4 >= (int)n) break;
            names[o++] = (wchar_t)d->letters[j];
            names[o++] = (wchar_t)':';
            names[o++] = (wchar_t)'\\';
            names[o++] = 0;
        }
        names[o++] = 0;
        if (ret) *ret = (DWORD)o;
        return 1;
    }
    return 0;
}

/* --- 设备 I/O：按路径里的盘符 / 磁盘号查世界 --- */

typedef struct { int dev; int is_phys; } shim_handle;

void *CreateFileW(const wchar_t *path, DWORD access, DWORD share,
                  void *sa, DWORD disp, DWORD flags, void *tmpl)
{
    shim_handle *h;
    int i;
    char p8[256];
    int k;
    (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)tmpl;

    for (k = 0; path && path[k] && k < 255; k++) p8[k] = (char)path[k];
    p8[k] = 0;

    /* \\.\PhysicalDriveN */
    if (strstr(p8, "PhysicalDrive")) {
        int dn = atoi(strstr(p8, "PhysicalDrive") + 13);
        for (i = 0; i < g_world_n; i++) {
            if (g_world[i].disk_number == dn) {
                h = (shim_handle *)calloc(1, sizeof *h);
                if (!h) return INVALID_HANDLE_VALUE;
                h->dev = i; h->is_phys = 1;
                return (void *)h;
            }
        }
        return INVALID_HANDLE_VALUE;
    }
    /* 设备接口路径精确匹配（disk / hid / net / hub 接口） */
    for (i = 0; i < g_world_n; i++) {
        if (strcmp(p8, g_world[i].device_path) == 0) {
            h = (shim_handle *)calloc(1, sizeof *h);
            if (!h) return INVALID_HANDLE_VALUE;
            h->dev = i; h->is_phys = 1;
            return (void *)h;
        }
    }
    /* \\?\Volume{...} 或 \\.\X: */
    for (i = 0; i < g_world_n; i++) {
        if (g_world[i].iface != IF_VOLUME) continue;
        if (strstr(p8, g_world[i].device_path) ||
            (strlen(p8) >= 2 && p8[strlen(p8) - 1] == ':')) {
            const char *L = g_world[i].letters;
            int want = (strlen(p8) >= 2) ? p8[strlen(p8) - 2] : 0;
            if (strstr(p8, g_world[i].device_path) ||
                (want && L && strchr(L, want))) {
                h = (shim_handle *)calloc(1, sizeof *h);
                if (!h) return INVALID_HANDLE_VALUE;
                h->dev = i; h->is_phys = 0;
                return (void *)h;
            }
        }
    }
    return INVALID_HANDLE_VALUE;
}

int DeviceIoControl(void *h, DWORD code, void *in, DWORD in_sz,
                    void *out, DWORD out_sz, DWORD *ret, void *ov)
{
    shim_handle *sh = (shim_handle *)h;
    (void)in; (void)in_sz; (void)ov;
    if (!sh || h == INVALID_HANDLE_VALUE || !out) return 0;

    if (code == IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) {
        /* out: NumberOfDiskExtents(DWORD) + DISK_EXTENT{DiskNumber,...} */
        DWORD n = 1;
        int   dn = g_world[sh->dev].disk_number;
        if (dn < 0) return 0;
        if (out_sz < sizeof(DWORD) + 24) return 0;
        memcpy(out, &n, sizeof n);
        memcpy((char *)out + sizeof(DWORD), &dn, sizeof dn);
        if (ret) *ret = (DWORD)(sizeof(DWORD) + 24);
        return 1;
    }
    if (code == IOCTL_STORAGE_GET_DEVICE_NUMBER) {
        /* out: STORAGE_DEVICE_NUMBER{DeviceType(4) DeviceNumber(4) PartitionNumber(4)} */
        int dn = g_world[sh->dev].disk_number;
        int zero = 0;
        if (dn < 0) return 0;
        if (out_sz < 12) return 0;
        memset(out, 0, out_sz);
        memcpy((char *)out + 4, &dn, sizeof dn);
        memcpy((char *)out + 8, &zero, sizeof zero);
        if (ret) *ret = 12;
        return 1;
    }
    if (code == IOCTL_STORAGE_QUERY_PROPERTY) {
        /* out: STORAGE_DEVICE_DESCRIPTOR 简化：BusType 在偏移 28 */
        int bus = g_world[sh->dev].bus_type;
        int rem = g_world[sh->dev].removable;
        if (out_sz < 40) return 0;
        memset(out, 0, out_sz);
        /* 偏移必须与 STORAGE_DEVICE_DESCRIPTOR 一致：
         *   BusType        @ 28 (uint32)
         *   RemovableMedia @ 10 (uint8)  —— 之前错写成 32，读到了
         *                                   RawPropertiesLength 的位置 */
        memcpy((char *)out + 28, &bus, sizeof bus);
        ((unsigned char *)out)[10] = (unsigned char)(rem ? 1 : 0);
        if (ret) *ret = 40;
        return 1;
    }
    return 0;
}

int CloseHandleShim(void *h)
{
    free(h);
    return 1;
}
