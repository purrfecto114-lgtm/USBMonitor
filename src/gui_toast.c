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

#define COL_BG     "#22272e"
#define COL_BORDER "#3a4150"
#define COL_TITLE  "#f2f4f7"
#define COL_BODY   "#c9ced6"
#define COL_DIM    "#8b929c"
#define COL_ACCENT_ADD    "#35b46a"
#define COL_ACCENT_REMOVE "#8b929c"

#define TOAST_W      400
#define PAD          14
#define TITLE_SIZE   15
#define BODY_SIZE    11
#define MARGIN_RIGHT 24
#define MARGIN_BOTTOM 48
#define SLOT_GAP     12

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
    unsigned long px_bg, px_border, px_accent;
    char      *lines[8];            /* heap strings, drawn top to bottom   */
    int        n_lines;
    int        title_idx;           /* which line is the title (0)         */
    int        body_first;          /* first body line uses COL_BODY       */
    int        n_body_dim_from;     /* from this line on, dim color        */
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

    t->lines[i++] = strdup(a->remove ? "USB 设备已拔出" : "USB 设备已插入");
    t->title_idx = 0;
    t->body_first = 1;
    t->n_body_dim_from = 2;

    /* line 1: model (key) — or just the key when the model is empty */
    if (a->model[0])
        snprintf(buf, sizeof buf, "%s (%s)", a->model, a->key);
    else
        snprintf(buf, sizeof buf, "%s", a->key[0] ? a->key : "USB 存储设备");
    t->lines[i++] = strdup(buf);

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
            t->lines[i++] = strdup(buf);
        }
        if (a->mount[0])
            snprintf(buf, sizeof buf, "挂载点 %s", a->mount);
        else
            snprintf(buf, sizeof buf, "未挂载");
        t->lines[i++] = strdup(buf);

        if (a->serial[0]) {
            snprintf(buf, sizeof buf, "序列 %s", a->serial);
            t->lines[i++] = strdup(buf);
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

static void draw_all(toast_win *t)
{
    int y = PAD;
    int i;

    XSetForeground(t->dpy, t->gc, t->px_bg);
    XFillRectangle(t->dpy, t->win, t->gc, 0, 0, t->w, t->h);
    XSetForeground(t->dpy, t->gc, t->px_accent);
    XFillRectangle(t->dpy, t->win, t->gc, 0, 0, 4, t->h);
    XSetForeground(t->dpy, t->gc, t->px_border);
    XDrawRectangle(t->dpy, t->win, t->gc, 0, 0, t->w - 1, t->h - 1);

    for (i = 0; i < t->n_lines; i++) {
        const XftColor *col;
        const XftFont *f;
        if (i == t->title_idx) { f = t->f_title; col = &t->c_title; }
        else {
            f = t->f_body;
            col = (i < t->n_body_dim_from) ? &t->c_body : &t->c_dim;
        }
        y += (int)f->ascent;
        XftDrawStringUtf8(t->draw, (XftColor *)col, (XftFont *)f,
                          PAD + 4, y,
                          (const FcChar8 *)t->lines[i],
                          (int)strlen(t->lines[i]));
        y += (int)f->descent + 5;
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
    int i;

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
        t.f_title = t.f_body = NULL;
        t.n_lines = 0;
    }

    {
        int line_h_title = t.f_title ? t.f_title->ascent + t.f_title->descent + 5 : 20;
        int line_h_body  = t.f_body  ? t.f_body->ascent  + t.f_body->descent + 5   : 16;
        int body_lines   = (t.n_lines > 1) ? t.n_lines - 1 : 0;
        t.w = TOAST_W;
        t.h = PAD + line_h_title + (body_lines * line_h_body) + PAD - 5;
        if (t.h < 60) t.h = 60;
    }

    x = screen_w - t.w - MARGIN_RIGHT;
    y = screen_h - t.h - MARGIN_BOTTOM - a.slot * (t.h + SLOT_GAP);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    vis = DefaultVisual(t.dpy, t.scr);
    cm  = DefaultColormap(t.dpy, t.scr);

    t.px_bg     = (unsigned long)alloc_color(t.dpy, &t, COL_BG,      0x2e2722UL);
    t.px_border = (unsigned long)alloc_color(t.dpy, &t, COL_BORDER,  0x50413aUL);
    t.px_accent = (unsigned long)alloc_color(t.dpy, &t,
                    a.remove ? COL_ACCENT_REMOVE : COL_ACCENT_ADD, 0x6ab435UL);

    t.win = XCreateSimpleWindow(t.dpy, RootWindow(t.dpy, t.scr),
                                x, y, (unsigned)t.w, (unsigned)t.h, 1,
                                t.px_border, t.px_bg);
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

    if (t.f_title) {
        t.draw = XftDrawCreate(t.dpy, t.win, vis, cm);
        XftColorAllocName(t.dpy, vis, cm, COL_TITLE, &t.c_title);
        XftColorAllocName(t.dpy, vis, cm, COL_BODY,  &t.c_body);
        XftColorAllocName(t.dpy, vis, cm, COL_DIM,   &t.c_dim);
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
    if (t.f_title) XftColorFree(t.dpy, vis, cm, &t.c_title);
    if (t.f_title) XftColorFree(t.dpy, vis, cm, &t.c_body);
    if (t.f_title) XftColorFree(t.dpy, vis, cm, &t.c_dim);
    if (t.f_title) XftFontClose(t.dpy, t.f_title);
    if (t.f_body && t.f_body != t.f_title) XftFontClose(t.dpy, t.f_body);
    XFreeGC(t.dpy, t.gc);
    XDestroyWindow(t.dpy, t.win);
    XCloseDisplay(t.dpy);
    for (i = 0; i < t.n_lines; i++) free(t.lines[i]);
    return 0;
}
