/* gui_toast.c — usbmon-toast: the popup GUI helper binary.
 *
 * Spawned by the usbmon daemon (fork + execv with an argv array — never
 * a shell) whenever a USB storage device is inserted or removed.  Renders
 * a small dark toast window in the screen's bottom-right corner:
 *
 *   USB 设备已插入
 *   Kingston DataTraveler 3.0 (sdc)
 *   容量 62.7 GB · 2 个分区
 *   挂载点 /media/usb-sdc1
 *   序列 sha256:60a44c4c12ab
 *
 * Design notes:
 *   - Xlib + Xft only (fontconfig resolves a CJK-capable "sans", so
 *     Chinese device names render correctly; falls back to any font,
 *     then to the server's built-in "fixed").
 *   - override-redirect window: shows up instantly, needs no window
 *     manager, works under bare Xvfb as well as GNOME/KDE.
 *   - auto-dismisses after --ttl seconds; click anywhere to dismiss.
 *   - several concurrent toasts stack via --slot (0..3, bottom-up).
 *   - crashes here can never hurt the daemon: separate process, and a
 *     display-gone condition exits 0 via the IO error handler.
 *
 * Standalone usage (for tests / screenshots):
 *   usbmon-toast --add  --model "Kingston DataTraveler 3.0" --key sdc \
 *                --size 62668800000 --parts sdc1,sdc2 \
 *                --mount /media/usb-sdc1 --serial sha256:60a44c4c12ab \
 *                --ttl 12 --slot 0
 */
#define _XOPEN_SOURCE 700
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

/* ---------------------------------------------------------------- palette */

#define COL_BG           "#111923"
#define COL_BORDER       "#374a60"
#define COL_TITLE        "#f2f6fc"
#define COL_BODY         "#e5edf7"
#define COL_DIM          "#b4c3d5"
#define COL_ACCENT_ADD   "#6be1b5"
#define COL_ACCENT_REMOVE "#ffa6a3"
#define COL_HEADER       "#1b2837"

#define TOAST_W       420
#define PAD           16
#define TITLE_SIZE    14
#define BODY_SIZE     11
#define MARGIN_RIGHT  24
#define MARGIN_BOTTOM 48
#define SLOT_GAP      12

/* ---------------------------------------------------------------- state --- */

typedef struct {
    int  remove;                    /* 0 = add, 1 = remove                */
    char key[64];
    char model[192];
    unsigned long long size;
    char parts[256];                /* "sdb1,sdb2"                        */
    char mount[192];
    char serial[96];
    int  ttl;                       /* seconds                            */
    int  slot;                      /* 0..3 stacking slot                 */
} toast_args;

typedef struct {
    Display   *dpy;
    int        scr;
    Window     win;
    int        w, h;
    XftDraw   *draw;
    XftFont   *f_title, *f_body;
    XftColor   c_title, c_body, c_dim;
    GC         gc;
    unsigned long px_bg, px_border, px_accent, px_header;
    char       lines[5][256];
    int        n_lines;
} toast_win;

/* ------------------------------------------------------------- utilities -- */

static void die_usage(const char *msg)
{
    fprintf(stderr, "usbmon-toast: %s (try --help)\n", msg);
    exit(2);
}

static void parse_args(int argc, char **argv, toast_args *a)
{
    int i;
    memset(a, 0, sizeof *a);
    a->ttl = 12;
    a->slot = 0;
    for (i = 1; i < argc; i++) {
        const char *o = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (!strcmp(o, "--add"))            a->remove = 0;
        else if (!strcmp(o, "--remove"))    a->remove = 1;
        else if (!strcmp(o, "--ttl"))       { a->ttl = atoi(v); i++; }
        else if (!strcmp(o, "--slot"))      { a->slot = atoi(v); i++; }
        else if (!strcmp(o, "--key"))       { snprintf(a->key,   sizeof a->key,   "%s", v); i++; }
        else if (!strcmp(o, "--model"))     { snprintf(a->model, sizeof a->model, "%s", v); i++; }
        else if (!strcmp(o, "--size"))      { a->size = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(o, "--parts"))     { snprintf(a->parts, sizeof a->parts, "%s", v); i++; }
        else if (!strcmp(o, "--mount"))     { snprintf(a->mount, sizeof a->mount, "%s", v); i++; }
        else if (!strcmp(o, "--serial"))    { snprintf(a->serial, sizeof a->serial, "%s", v); i++; }
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) {
            printf("usbmon-toast — popup toast for usbmon (X11)\n"
                   "  --add | --remove     event style\n"
                   "  --key --model --size --parts --mount --serial   device fields\n"
                   "  --ttl SECONDS        auto-dismiss (default 12)\n"
                   "  --slot N             stacking slot 0..3\n");
            exit(0);
        }
        else die_usage("unknown option");
    }
    if (a->ttl < 1) a->ttl = 1;
    if (a->ttl > 600) a->ttl = 600;
    if (a->slot < 0 || a->slot > 3) a->slot = 0;
}

static void human_size(unsigned long long bytes, char *out, size_t n)
{
    double v = (double)bytes;
    const char *unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    if (u <= 1) snprintf(out, n, "%.0f %s", v, unit[u]);
    else snprintf(out, n, "%.1f %s", v, unit[u]);
}

/* Count comma-separated non-empty tokens. */
static int count_parts(const char *s)
{
    int n = 0;
    const char *p = s;
    if (!s || !*s) return 0;
    n = 1;
    while ((p = strchr(p, ',')) != NULL) { n++; p++; }
    return n;
}

/* Build the text lines from the device fields (UTF-8, zh-CN labels). */
static void build_lines(const toast_args *a, toast_win *t)
{
    char sz[32];
    char buf[512];
    int i = 0;

    snprintf(t->lines[i++], sizeof t->lines[0], "%s",
             a->remove ? "USB 设备已拔出" : "USB 设备已插入");

    /* line 1: model (key) — or just the key when the model is empty */
    if (a->model[0])
        snprintf(buf, sizeof buf, "%s (%s)", a->model, a->key);
    else
        snprintf(buf, sizeof buf, "%s", a->key[0] ? a->key : "USB 存储设备");
    snprintf(t->lines[i++], sizeof t->lines[0], "%.255s", buf);

    if (!a->remove) {
        if (a->size > 0) {
            human_size(a->size, sz, sizeof sz);
            {
                int np = count_parts(a->parts);
                if (np > 0)
                    snprintf(buf, sizeof buf, "容量 %s · %d 个分区", sz, np);
                else
                    snprintf(buf, sizeof buf, "容量 %s", sz);
            }
            snprintf(t->lines[i++], sizeof t->lines[0], "%.255s", buf);
        }
        if (a->mount[0])
            snprintf(buf, sizeof buf, "挂载点 %s", a->mount);
        else
            snprintf(buf, sizeof buf, "未挂载");
        snprintf(t->lines[i++], sizeof t->lines[0], "%.255s", buf);

        if (a->serial[0]) {
            snprintf(buf, sizeof buf, "序列 %s", a->serial);
            snprintf(t->lines[i++], sizeof t->lines[0], "%.255s", buf);
        }
    }
    t->n_lines = i;
}

/* ------------------------------------------------------------- rendering -- */

static int alloc_color(Display *dpy, toast_win *t, const char *name,
                       unsigned long fallback)
{
    XColor exact, nearest;
    Colormap cm = DefaultColormap(dpy, t->scr);
    if (XAllocNamedColor(dpy, cm, name, &exact, &nearest))
        return (int)nearest.pixel;
    return (int)fallback;
}

/* Open a font that can actually render CJK: iterate well-known
 * families and VERIFY a CJK glyph exists (fontconfig on minimal
 * systems happily resolves "sans:lang=zh" to a Latin-only font,
 * which would turn every Chinese label into tofu). */
static XftFont *open_cjk_font(Display *d, int scr, int size)
{
    static const char *cands[] = {
        "sans:lang=zh",          /* proper fontconfig setups            */
        "Noto Sans CJK SC",      /* Debian/Ubuntu/Arch standard         */
        "WenQuanYi Micro Hei",   /* older distros                       */
        "WenQuanYi Zen Hei",
        "Sarasa Mono SC",        /* common on dev boxes (this sandbox)  */
        "LXGW WenKai",
        "sans"                   /* last resort: Latin-only             */
    };
    const int NCAND = (int)(sizeof cands / sizeof cands[0]);
    XftFont *fallback = NULL;
    int i;

    for (i = 0; i < NCAND; i++) {
        char pat[96];
        XftFont *f;
        snprintf(pat, sizeof pat, "%s:size=%d", cands[i], size);
        f = XftFontOpenName(d, scr, pat);
        if (!f) continue;
        if (XftGlyphExists(d, f, (FcChar32)0x8BBE)) {   /* U+8BBE "设" */
            if (fallback) XftFontClose(d, fallback);
            return f;                          /* CJK-capable: winner */
        }
        if (!fallback) fallback = f;           /* remember first opened */
        else XftFontClose(d, f);
    }
    return fallback;    /* Latin-only font or NULL: degrade gracefully */
}

static int open_fonts(toast_win *t)
{
    t->f_title = open_cjk_font(t->dpy, t->scr, TITLE_SIZE);
    t->f_body  = open_cjk_font(t->dpy, t->scr, BODY_SIZE);
    if (!t->f_title || !t->f_body)
        return -1;
    return 0;
}

static void draw_text(toast_win *t, XftFont *font, XftColor *color,
                      int x, int y, const char *line)
{
    XGlyphInfo extent, ellipsis;
    const FcChar8 *text = (const FcChar8 *)line;
    static const FcChar8 dots[] = "...";
    int len, limit;
    if (!line || !t || !font || !color || !t->draw) return;
    len = (int)strlen(line);
    limit = t->w - x - PAD;
    if (len <= 0 || limit <= 0) return;   /* 窄窗口/空文本：不进入截断循环 */
    XftTextExtentsUtf8(t->dpy, font, text, len, &extent);
    if (extent.xOff > limit) {
        XftTextExtentsUtf8(t->dpy, font, dots, 3, &ellipsis);
        do {
            do { len--; } while (len > 0 && (text[len] & 0xc0) == 0x80);
            XftTextExtentsUtf8(t->dpy, font, text, len, &extent);
        } while (len > 0 && extent.xOff + ellipsis.xOff > limit);
        if (ellipsis.xOff <= limit) XftDrawStringUtf8(t->draw, color, font,
                                                     x + extent.xOff, y, dots, 3);
    }
    if (len) XftDrawStringUtf8(t->draw, color, font, x, y, text, len);
}

static void draw_all(toast_win *t)
{
    int y = PAD, i;

    XSetForeground(t->dpy, t->gc, t->px_bg);
    XFillRectangle(t->dpy, t->win, t->gc, 0, 0, t->w, t->h);
    XSetForeground(t->dpy, t->gc, t->px_header);
    XFillRectangle(t->dpy, t->win, t->gc, PAD, PAD, 32, 32);
    XSetForeground(t->dpy, t->gc, t->px_accent);
    XDrawRectangle(t->dpy, t->win, t->gc, PAD + 9, PAD + 12, 13, 11);
    XDrawRectangle(t->dpy, t->win, t->gc, PAD + 12, PAD + 7, 7, 5);
    XDrawPoint(t->dpy, t->win, t->gc, PAD + 14, PAD + 9);
    XDrawPoint(t->dpy, t->win, t->gc, PAD + 17, PAD + 9);
    XSetForeground(t->dpy, t->gc, t->px_border);
    XDrawRectangle(t->dpy, t->win, t->gc, 0, 0, t->w - 1, t->h - 1);

    if (!t->draw || !t->f_title || !t->f_body) { XFlush(t->dpy); return; }
    draw_text(t, t->f_title, &t->c_title, PAD + 44,
              PAD + (32 + t->f_title->ascent - t->f_title->descent) / 2, t->lines[0]);
    y += 44;
    XDrawLine(t->dpy, t->win, t->gc, PAD, y - 5, t->w - PAD, y - 5);
    for (i = 1; i < t->n_lines; i++) {
        y += t->f_body->ascent;
        draw_text(t, t->f_body, i == 1 ? &t->c_body : &t->c_dim, PAD, y, t->lines[i]);
        y += t->f_body->descent + 5;
    }
    XFlush(t->dpy);
}

/* Xlib default behaviour on fatal IO errors is to abort the process;
 * a toast dying with the display is not an error worth a core file. */
static int on_io_error(Display *dpy)
{
    (void)dpy;
    _exit(0);
    return 0;
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    toast_args a;
    toast_win t;
    Visual *vis;
    Colormap cm;
    XSetWindowAttributes attrs;
    struct timespec now;
    double deadline;
    int screen_w, screen_h, x, y;
    int slot_pitch;
    unsigned color_mask = 0;

    parse_args(argc, argv, &a);
    memset(&t, 0, sizeof t);

    t.dpy = XOpenDisplay(NULL);
    if (!t.dpy) {
        /* No display (headless): not an error for a toast. */
        fprintf(stderr, "usbmon-toast: no X display\n");
        return 0;
    }
    XSetIOErrorHandler(on_io_error);
    t.scr = DefaultScreen(t.dpy);
    screen_w = DisplayWidth(t.dpy, t.scr);
    screen_h = DisplayHeight(t.dpy, t.scr);

    build_lines(&a, &t);
    if (open_fonts(&t) != 0) {
        /* no usable font: draw a plain colored card (still informative
         * through its presence + colors; never crash) */
        fprintf(stderr, "usbmon-toast: no fonts, drawing minimal card\n");
        t.n_lines = 0;
    }

    {
        int body_height = t.f_body ? t.f_body->ascent + t.f_body->descent + 5 : 16;
        int body_lines = t.n_lines > 1 ? t.n_lines - 1 : 0;
        t.w = screen_w > TOAST_W + MARGIN_RIGHT ? TOAST_W : screen_w - 2;
        if (t.w < 1) t.w = 1;
        t.h = 2 * PAD + 44 + body_lines * body_height;
        slot_pitch = 2 * PAD + 44 + 4 * body_height + SLOT_GAP;
    }

    x = screen_w - t.w - MARGIN_RIGHT;
    y = screen_h - t.h - MARGIN_BOTTOM - a.slot * slot_pitch;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    vis = DefaultVisual(t.dpy, t.scr);
    cm  = DefaultColormap(t.dpy, t.scr);

    t.px_bg     = (unsigned long)alloc_color(t.dpy, &t, COL_BG,      0x2e2722UL);
    t.px_border = (unsigned long)alloc_color(t.dpy, &t, COL_BORDER, 0x374a60UL);
    t.px_header = (unsigned long)alloc_color(t.dpy, &t, COL_HEADER, 0x1b2837UL);
    t.px_accent = (unsigned long)alloc_color(t.dpy, &t,
                    a.remove ? COL_ACCENT_REMOVE : COL_ACCENT_ADD, 0x6ab435UL);

    t.win = XCreateSimpleWindow(t.dpy, RootWindow(t.dpy, t.scr),
                                x, y, (unsigned)t.w, (unsigned)t.h, 1,
                                t.px_border, t.px_bg);
    XStoreName(t.dpy, t.win, "usbmon-toast");
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask;
    attrs.background_pixel = t.px_bg;
    XChangeWindowAttributes(t.dpy, t.win, CWOverrideRedirect | CWEventMask |
                                           CWBackPixel, &attrs);
    {
        XGCValues gcv;
        memset(&gcv, 0, sizeof gcv);
        t.gc = XCreateGC(t.dpy, t.win, 0, &gcv);
    }

    if (t.f_title && t.f_body) {
        if (XftColorAllocName(t.dpy, vis, cm, COL_TITLE, &t.c_title)) color_mask |= 1;
        if (XftColorAllocName(t.dpy, vis, cm, COL_BODY, &t.c_body)) color_mask |= 2;
        if (XftColorAllocName(t.dpy, vis, cm, COL_DIM, &t.c_dim)) color_mask |= 4;
        if (color_mask == 7) t.draw = XftDrawCreate(t.dpy, t.win, vis, cm);
    }

    XMapWindow(t.dpy, t.win);
    draw_all(&t);       /* draw before the map settles (override-redirect) */

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (double)now.tv_sec + (double)now.tv_nsec / 1e9 + (double)a.ttl;

    for (;;) {
        double now_d;
        int ms_left;
        fd_set rfds;
        struct timeval tv;
        XEvent ev;

        while (XPending(t.dpy)) {
            XNextEvent(t.dpy, &ev);
            if (ev.type == ButtonPress) goto done;
            if (ev.type == Expose && ev.xexpose.count == 0) draw_all(&t);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        now_d = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
        ms_left = (int)((deadline - now_d) * 1000.0);
        if (ms_left <= 0) break;
        if (ms_left > 100) ms_left = 100;

        FD_ZERO(&rfds);
        FD_SET(ConnectionNumber(t.dpy), &rfds);
        tv.tv_sec = ms_left / 1000;
        tv.tv_usec = (ms_left % 1000) * 1000;
        (void)select(ConnectionNumber(t.dpy) + 1, &rfds, NULL, NULL, &tv);
    }

done:
    if (t.draw) XftDrawDestroy(t.draw);
    if (color_mask & 1) XftColorFree(t.dpy, vis, cm, &t.c_title);
    if (color_mask & 2) XftColorFree(t.dpy, vis, cm, &t.c_body);
    if (color_mask & 4) XftColorFree(t.dpy, vis, cm, &t.c_dim);
    if (t.f_title) XftFontClose(t.dpy, t.f_title);
    if (t.f_body && t.f_body != t.f_title) XftFontClose(t.dpy, t.f_body);
    XFreeGC(t.dpy, t.gc);
    XDestroyWindow(t.dpy, t.win);
    XCloseDisplay(t.dpy);
    return 0;
}
