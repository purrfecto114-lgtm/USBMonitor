/* gui_win32.c — Windows GUI backend: device panel + toasts + hot path.
 *
 * One dedicated GUI thread owns every window:
 *   - an invisible TOP-LEVEL listener window receiving WM_DEVICECHANGE
 *     (DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE, DBT_DEVTYP_VOLUME)
 *     — the Microsoft-recommended event model; it sets the wake event
 *     the daemon main loop waits on (instant rounds on plug/unplug).
 *     The window MUST be top-level: message-only windows (created with
 *     a HWND_MESSAGE parent) do not receive broadcast messages at all
 *     (see "Window Features" on learn.microsoft.com), and WM_DEVICECHANGE
 *     device events are broadcast to top-level windows.  It is never
 *     shown, so it stays invisible.
 *   - the single-instance DEVICE PANEL (um_toast_win32.c, class
 *     "usbmonToast2"): the v1.1.1 ToastWindow semantics — the panel IS
 *     the add/remove notification.  The daemon thread builds a heap
 *     um_toast_model from fresh evidence (um_enum.c: SetupAPI + volume
 *     extents, including drive-letter-less volumes and non-storage USB
 *     devices) and marshals it via the UMWM_PANEL thread message; the
 *     GUI thread copies it into the panel, re-anchors bottom-right and
 *     restarts the fade/countdown.  Panel buttons call back into the
 *     SAME volume actions the tray menus use (tray_win32.c).
 *   - top-level TEXT toast windows (WS_POPUP | WS_EX_TOPMOST | tool
 *     window, class "usbmonToast"): tray action feedback (eject result,
 *     startup toggle) and the fallback when the evidence layer yields
 *     nothing, so an event is never silently dropped.
 *
 * user32/gdi32/setupapi/cfgmgr32 ship with every Windows install, so
 * unlike POSIX there is no reason to split rendering into a helper
 * process; a thread is enough and keeps the message pump on one owner.
 *
 * Verified on a real Windows machine by CI (windows-latest runner runs
 * tools/demo.ps1, including a simulated WM_DEVICECHANGE broadcast that
 * must wake the daemon and log a "wake":"hot" round, and the full
 * panel input -> model -> window -> hit-test -> callback chain).
 */
#include "usbmon.h"

#ifdef _WIN32

#include "um_enum.h"
#include "um_toast_ui.h"
#include "um_toast_win32.h"

#include <windows.h>
#include <dbt.h>           /* DEV_BROADCAST_*, DBT_* (device-change events) */
#include <wchar.h>         /* wcslen, wcscpy_s */
#include <string.h>
#include <stdlib.h>

/* UMWM_TOAST / UMWM_QUIT / UMWM_TRAY* come from usbmon.h (numeric,
 * WM_APP-based) so tray_win32.c and this file agree on every id. */

/* ---------------------------------------------------------------- palette */

#define TW_BG       RGB(0x22, 0x27, 0x2e)
#define TW_BORDER   RGB(0x3a, 0x41, 0x50)
#define TW_TITLE    RGB(0xf2, 0xf4, 0xf7)
#define TW_BODY     RGB(0xc9, 0xce, 0xd6)
#define TW_DIM      RGB(0x8b, 0x92, 0x9c)
#define TW_ACC_ADD  RGB(0x35, 0xb4, 0x6a)
#define TW_ACC_RM   RGB(0x8b, 0x92, 0x9c)

#define TW_WIDTH        400
#define TW_PAD          14
#define TW_TITLE_H      26
#define TW_LINE_H       18
#define TW_MARGIN_R     24
#define TW_MARGIN_B     48
#define TW_SLOT_GAP     12

/* ------------------------------------------------------------------ types */

typedef struct {
    int   is_add;
    int   ttl;
    int   slot;
    wchar_t lines[6][192];
    int   n_lines;
    int   n_dim_from;       /* lines from this index on are dim-colored */
} toast_data;

static const wchar_t *g_class_toast  = L"usbmonToast";
static const wchar_t *g_class_listen = L"usbmonListen";

/* ------------------------------------------------------------------ panel -- */

static um_toast_win *g_panel;      /* single instance; events refresh it */

/* USBMON_PANEL_TEST=path (test-only, same pattern as USBMON_TRAY_TEST):
 *   - evidence comes from a fixed 3-device set instead of the machine
 *     (CI runners have no USB devices to plug in);
 *   - every panel show/refresh appends the model content to the file;
 *   - every panel action appends one line.
 * Together this lets demo.ps1 assert the whole chain — input -> model ->
 * UMWM_PANEL marshaling -> window -> hit test -> callback -> tray volume
 * action — on a headless runner, without a single real USB device. */
static FILE *panel_test_out(void)
{
    const char *p = getenv("USBMON_PANEL_TEST");
    FILE *f;
    long sz;
    if (!p || !*p) return NULL;
    f = fopen(p, "a");
    if (!f) return NULL;
    /* BOM once when the file is empty (append mode; see tray_test_out). */
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz == 0)
        fwrite("\xEF\xBB\xBF", 1, 3, f);
    return f;
}

/* Dump the live panel content (headline / subtitle / rows / flags). */
static void panel_dump(const char *tag)
{
    FILE *f = panel_test_out();
    const um_toast_model *m;
    const um_toast_state *st;
    int i;

    if (!f || !g_panel) return;
    m  = um_toast_win_model(g_panel);
    st = um_toast_win_state(g_panel);
    fprintf(f, "panel %s visible=%d expanded=%d rows=%d\n",
            tag, st->visible, st->expanded, m->n_rows);
    fprintf(f, "  head %s\n", m->headline);
    fprintf(f, "  sub %s\n", m->subtitle);
    for (i = 0; i < m->n_rows; i++)
        fprintf(f, "  row%d %s | %s | kind=%d open=%d eject=%d letters=%s\n",
                i, m->rows[i].title, m->rows[i].subtitle,
                (int)m->rows[i].kind, m->rows[i].openable,
                m->rows[i].ejectable, m->rows[i].letters[0]);
    fclose(f);
}

static void panel_log_action(const char *act, int row, const char *arg)
{
    FILE *f = panel_test_out();
    if (!f) return;
    fprintf(f, "action %s row=%d %s\n", act, row, arg ? arg : "");
    fclose(f);
}

/* Fixed evidence set for USBMON_PANEL_TEST: one dual-partition stick
 * (E:+F:), one dock, one HID pen — every interactive shape (an openable+
 * ejectable row, non-storage rows with no actions) in three rows. */
static int panel_test_evidence(um_evidence *evs, int max)
{
    um_evidence *e;

    if (max < 3) return 0;
    memset(evs, 0, (size_t)max * sizeof *evs);

    e = &evs[0];
    um_copy_str(e->instance_id, sizeof e->instance_id,
                "USB\\VID_0781&PID_5583\\4C5312345");
    e->tags = UM_TAG_USB | UM_TAG_STORAGE | UM_TAG_VOLUME;
    e->disk_number = 2;
    e->bus_is_usb = 1;
    e->removable = 1;
    e->media_present = 1;
    e->unlocked = 1;
    e->n_letters = 2;
    um_copy_str(e->letters[0], sizeof e->letters[0], "E:");
    um_copy_str(e->letters[1], sizeof e->letters[1], "F:");
    e->kind = um_ui_classify(e->tags, 1, e->bus_is_usb, e->removable,
                             e->media_present, e->unlocked);

    e = &evs[1];
    um_copy_str(e->instance_id, sizeof e->instance_id,
                "USB\\VID_2109&PID_0817\\6&1A2B3C4D&0&1");
    e->tags = UM_TAG_USB | UM_TAG_HUB;
    e->disk_number = -1;
    e->kind = um_ui_classify(e->tags, 0, 1, 0, 0, 1);

    e = &evs[2];
    um_copy_str(e->instance_id, sizeof e->instance_id,
                "USB\\VID_256C&PID_006D\\9&5E7F0A1B&0&2");
    e->tags = UM_TAG_USB | UM_TAG_HID;
    e->disk_number = -1;
    e->kind = um_ui_classify(e->tags, 0, 1, 0, 1, 1);

    return 3;
}

static int panel_collect_evidence(um_evidence *evs, int max)
{
    const char *test = getenv("USBMON_PANEL_TEST");
    if (test && test[0]) return panel_test_evidence(evs, max);
    return um_enum_collect(evs, max);
}

/* Copy "X:\" to the clipboard (the panel's 复制路径 action).  GUI thread;
 * on success the system owns the global memory from then on. */
static void panel_copy_path(HWND owner, char letter)
{
    HGLOBAL h;
    wchar_t *w;

    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, 4 * sizeof *w);
    if (h) {
        w = (wchar_t *)GlobalLock(h);
        if (w) {
            w[0] = (wchar_t)(unsigned char)letter;
            w[1] = L':';
            w[2] = L'\\';
            w[3] = L'\0';
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h))
                GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

/* Panel actions -> tray volume actions: tray_win32.c owns the real
 * implementations (ShellExecuteW / explorer /select / eject IOCTL), so
 * the tray menus and the panel buttons share ONE code path. */
static void panel_action_cb(um_toast_action act, int row, void *user)
{
    const um_toast_model *m = g_panel ? um_toast_win_model(g_panel) : NULL;

    (void)user;
    if (!m) return;
    switch (act) {
    case UM_ACT_OPEN: {
        int i;
        for (i = 0; i < m->n_rows; i++) {
            if (m->rows[i].openable && m->rows[i].n_letters > 0) {
                panel_log_action("open", i, m->rows[i].letters[0]);
                um_tray_open_letter(m->rows[i].letters[0][0]);
                break;
            }
        }
        break;
    }
    case UM_ACT_OPEN_ROW:
        if (row >= 0 && row < m->n_rows && m->rows[row].n_letters > 0) {
            panel_log_action("open_row", row, m->rows[row].letters[0]);
            um_tray_open_letter(m->rows[row].letters[0][0]);
        }
        break;
    case UM_ACT_REVEAL:
        if (row >= 0 && row < m->n_rows && m->rows[row].n_letters > 0) {
            panel_log_action("reveal", row, m->rows[row].letters[0]);
            um_tray_reveal_letter(m->rows[row].letters[0][0]);
        }
        break;
    case UM_ACT_COPY:
        if (row >= 0 && row < m->n_rows && m->rows[row].n_letters > 0) {
            panel_log_action("copy", row, m->rows[row].letters[0]);
            panel_copy_path(g_panel ? (HWND)um_toast_win_hwnd(g_panel)
                                    : NULL,
                            m->rows[row].letters[0][0]);
        }
        break;
    case UM_ACT_EJECT:
        if (row >= 0 && row < m->n_rows && m->rows[row].n_letters > 0) {
            panel_log_action("eject", row, m->rows[row].letters[0]);
            um_tray_eject_letter(m->rows[row].letters[0][0],
                                 m->rows[row].title);
        }
        break;
    case UM_ACT_CLOSE:
        panel_log_action("close", -1, "");
        if (g_panel) um_toast_win_hide(g_panel);
        break;
    default:
        break;      /* TOGGLE_EXPAND / AUTOHIDE stay inside the panel */
    }
}

/* --------------------------------------------------------------- helpers -- */

static void wcopy(const char *utf8, wchar_t *out, size_t n)
{
    um_utf8_to_wide(utf8 ? utf8 : "", out, (int)n);
}

/* Build the toast text (zh labels, same content as the Linux helper). */
static toast_data *toast_data_make(const um_device *dev, int is_add, um_gui *g)
{
    toast_data *td = malloc(sizeof *td);
    char buf[512];
    wchar_t wbuf[192];
    if (!td) return NULL;
    memset(td, 0, sizeof *td);
    td->is_add = is_add;
    td->ttl = g->toast_ttl;
    td->slot = g->slot_seq % UM_GUI_SLOTS;
    g->slot_seq++;

    wcscpy_s(td->lines[td->n_lines], 192, is_add ? L"USB 设备已插入"
                                                 : L"USB 设备已拔出");
    td->n_lines++;

    if (dev->model[0])
        snprintf(buf, sizeof buf, "%s (%s)", dev->model, dev->key);
    else
        snprintf(buf, sizeof buf, "%s",
                 dev->key[0] ? dev->key : "USB 存储设备");
    wcopy(buf, wbuf, 192);
    wcscpy_s(td->lines[td->n_lines], 192, wbuf);
    td->n_lines++;

    if (is_add) {
        if (dev->size_bytes > 0) {
            char sz[32];
            um_human_size(dev->size_bytes, sz, sizeof sz);
            if (dev->partition_count > 0)
                snprintf(buf, sizeof buf, "容量 %s · %d 个分区",
                         sz, dev->partition_count);
            else
                snprintf(buf, sizeof buf, "容量 %s", sz);
            wcopy(buf, wbuf, 192);
            wcscpy_s(td->lines[td->n_lines], 192, wbuf);
            td->n_lines++;
        }
        if (dev->mount[0]) snprintf(buf, sizeof buf, "挂载点 %s", dev->mount);
        else               snprintf(buf, sizeof buf, "%s", "未挂载");
        wcopy(buf, wbuf, 192);
        wcscpy_s(td->lines[td->n_lines], 192, wbuf);
        td->n_lines++;

        {
            const char *serial = (g->raw_serial && dev->serial[0])
                                 ? dev->serial : dev->serial_fp;
            if (serial[0]) {
                snprintf(buf, sizeof buf, "序列 %s", serial);
                wcopy(buf, wbuf, 192);
                wcscpy_s(td->lines[td->n_lines], 192, wbuf);
                td->n_lines++;
            }
        }
    }
    td->n_dim_from = 2;      /* first two lines: title + model */
    return td;
}

/* ---------------------------------------------------------- toast window -- */

static LRESULT CALLBACK toast_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        SetTimer(hw, 1, ((toast_data *)cs->lpCreateParams)->ttl * 1000, NULL);
        return 0;
    }
    case WM_PAINT: {
        toast_data *td = (toast_data *)GetWindowLongPtrW(hw, GWLP_USERDATA);
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc, rc_bar;
        HBRUSH bg, bar;
        HPEN border;
        int i;

        if (!td) return 0;
        hdc = BeginPaint(hw, &ps);
        GetClientRect(hw, &rc);

        bg     = CreateSolidBrush(TW_BG);
        bar    = CreateSolidBrush(td->is_add ? TW_ACC_ADD : TW_ACC_RM);
        border = CreatePen(PS_SOLID, 1, TW_BORDER);
        FillRect(hdc, &rc, bg);
        rc_bar = rc;
        rc_bar.right = 4;
        FillRect(hdc, &rc_bar, bar);
        SelectObject(hdc, border);
        MoveToEx(hdc, rc.left, rc.top, NULL);
        LineTo(hdc, rc.right - 1, rc.top);
        LineTo(hdc, rc.right - 1, rc.bottom - 1);
        LineTo(hdc, rc.left, rc.bottom - 1);
        LineTo(hdc, rc.left, rc.top);

        SetBkMode(hdc, TRANSPARENT);
        for (i = 0; i < td->n_lines; i++) {
            COLORREF col = TW_TITLE;
            int h = TW_LINE_H;
            if (i == 0) {
                HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                SelectObject(hdc, f);
                h = TW_TITLE_H;
            } else {
                HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                SelectObject(hdc, f);
                col = (i < td->n_dim_from) ? TW_BODY : TW_DIM;
            }
            SetTextColor(hdc, col);
            TextOutW(hdc, TW_PAD + 4, TW_PAD + i * h, td->lines[i],
                     (int)wcslen(td->lines[i]));
        }

        DeleteObject(bg);
        DeleteObject(bar);
        DeleteObject(border);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        DestroyWindow(hw);
        return 0;
    case WM_TIMER:
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY: {
        toast_data *td = (toast_data *)GetWindowLongPtrW(hw, GWLP_USERDATA);
        if (td) free(td);
        SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
        return 0;
    }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* -------------------------------------------------------- listener window -- */

static LRESULT CALLBACK listen_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    /* tray first: tray_win32.c owns UMWM_TRAY*, the taskbar-restart
     * message and the test-injection ids; 1 = fully handled. */
    if (um_tray_filter(hw, msg, (void *)(size_t)wp, (void *)(size_t)lp))
        return 0;

    if (msg == WM_DEVICECHANGE) {
        PDEV_BROADCAST_HDR hdr = (PDEV_BROADCAST_HDR)lp;
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            /* volume arrivals/removals are what the OS broadcasts to all
             * top-level windows; DBT_DEVTYP_DISK is not a broadcast device
             * type (dbt.h defines OEM/PORT/VOLUME/DEVICEINTERFACE/HANDLE) */
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
                SetEvent((HANDLE)GetWindowLongPtrW(hw, GWLP_USERDATA));
        }
        return TRUE;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ------------------------------------------------------------- GUI thread -- */

static DWORD WINAPI gui_thread_main(LPVOID param)
{
    /* um_gui lives in static storage inside main() -- always valid, no
     * lifetime race (the old stack-copied ctx was technically racy). */
    um_gui *g = (um_gui *)param;
    WNDCLASSW wc;
    HWND listener;
    MSG msg;
    ATOM at, al;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = toast_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = g_class_toast;
    at = RegisterClassW(&wc);
    (void)at;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = listen_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class_listen;
    al = RegisterClassW(&wc);
    (void)al;

    /* Invisible top-level listener (never ShowWindow'd).  Top-level is
     * REQUIRED: message-only windows (HWND_MESSAGE parent) never receive
     * broadcast messages such as WM_DEVICECHANGE. */
    listener = CreateWindowExW(0, g_class_listen, L"usbmon", WS_OVERLAPPED,
                               0, 0, 0, 0, NULL, NULL,
                               GetModuleHandleW(NULL), NULL);
    if (listener) {
        SetWindowLongPtrW(listener, GWLP_USERDATA, (LONG_PTR)g->wake_event);
        /* The same invisible top-level window doubles as the tray owner:
         * one message pump serves WM_DEVICECHANGE, tray clicks, menus
         * and toasts — the tray can never deadlock the hot path. */
        um_tray_install(listener, g);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == UMWM_TOAST) {
            toast_data *td = (toast_data *)msg.lParam;
            if (td) {
                int sw = GetSystemMetrics(SM_CXSCREEN);
                int sh = GetSystemMetrics(SM_CYSCREEN);
                int h = TW_PAD + TW_TITLE_H +
                        (td->n_lines - 1) * TW_LINE_H + TW_PAD;
                int x = sw - TW_WIDTH - TW_MARGIN_R;
                int y = sh - h - TW_MARGIN_B - td->slot * (h + TW_SLOT_GAP);
                HWND t = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                         g_class_toast, L"usbmon-toast",
                                         WS_POPUP,
                                         x, y, TW_WIDTH, h,
                                         NULL, NULL, GetModuleHandleW(NULL),
                                         td);
                if (t) {
                    ShowWindow(t, SW_SHOWNOACTIVATE);
                    UpdateWindow(t);
                } else {
                    free(td);
                }
            }
            continue;
        }
        if (msg.message == UMWM_PANEL) {
            /* heap um_toast_model built on the daemon thread; copy it in,
             * re-anchor, restart fade+countdown, then free the courier. */
            um_toast_model *m = (um_toast_model *)msg.lParam;
            if (m) {
                if (!g_panel) {
                    um_theme th;
                    um_theme_resolve(&th, "auto", um_toast_system_dark());
                    g_panel = um_toast_win_new(m, &th, 1, panel_action_cb, g);
                }
                if (g_panel) {
                    um_toast_win_update(g_panel, m);
                    um_toast_win_show(g_panel);
                    panel_dump("show");
                }
                free(m);
            }
            continue;
        }
        if (msg.message == UMWM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_panel) {
        um_toast_win_destroy(g_panel);
        g_panel = NULL;
    }
    if (listener) {
        um_tray_uninstall();      /* remove the icon before the window dies */
        DestroyWindow(listener);
    }
    return 0;
}

/* ------------------------------------------------------------- public API -- */

int um_gui_win_init(um_gui *g)
{
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev) return 0;
    g->wake_event = ev;

    g->gui_thread = CreateThread(NULL, 0, gui_thread_main, g, 0,
                                 &g->gui_tid);
    if (!g->gui_thread) {
        CloseHandle(ev);
        g->wake_event = NULL;
        return 0;
    }
    /* give the thread a moment to create its listener so wakeups from
     * the very first seconds are not missed */
    Sleep(50);

    /* USBMON_PANEL_TEST (CI only): runners have no USB devices to plug,
     * so exercise the full panel path once right after the GUI thread is
     * up — same marshaling (heap model + UMWM_PANEL) as production. */
    {
        const char *pt = getenv("USBMON_PANEL_TEST");
        if (pt && pt[0])
            um_gui_win_show(g, NULL, 1);
    }
    return 1;
}

/* Fallback/feedback text toast (the pre-2.4 renderer), kept for tray
 * action feedback and for device events when the evidence layer yields
 * nothing (the panel model would be empty). */
static void text_toast_post(um_gui *g, const um_device *dev, int is_add)
{
    toast_data *td = toast_data_make(dev, is_add, g);
    if (!td) return;
    if (!PostThreadMessageW(g->gui_tid, UMWM_TOAST, (WPARAM)is_add,
                            (LPARAM)td)) {
        free(td);            /* GUI thread gone: drop the toast quietly */
    }
}

void um_gui_win_show(um_gui *g, const um_device *dev, int is_add)
{
    um_evidence evs[UM_EVID_MAX];
    um_toast_model *m;
    int n;

    /* 2.4.0: device add/remove drives the rich single-instance panel
     * (v1.1.1 ToastWindow semantics — the panel IS the notification).
     * The model is built HERE on the daemon thread from fresh evidence
     * (um_enum.c: SetupAPI + volume extents — sees drive-letter-less
     * volumes AND non-storage USB devices); it is marshaled to the GUI
     * thread via UMWM_PANEL.  No evidence on this machine -> fall back
     * to the simple text toast so the event is never dropped. */
    m = (um_toast_model *)malloc(sizeof *m);
    if (!m) return;
    memset(m, 0, sizeof *m);
    memset(evs, 0, sizeof evs);
    n = panel_collect_evidence(evs, UM_EVID_MAX);
    if (n <= 0) {
        free(m);
        text_toast_post(g, dev, is_add);
        return;
    }
    um_enum_to_model(evs, n, m);
    if (dev) {
        const char *what = dev->model[0] ? dev->model : dev->key;
        if (!is_add) {
            snprintf(m->status, sizeof m->status, "已拔出：%s",
                     what[0] ? what : "USB 设备");
            m->accent_kind = 3;
        } else if (what[0]) {
            snprintf(m->summary, sizeof m->summary, "已插入：%s", what);
        }
    }
    if (!PostThreadMessageW(g->gui_tid, UMWM_PANEL, (WPARAM)is_add,
                            (LPARAM)m))
        free(m);
}

/* Arbitrary-text toast (tray action feedback: eject result, startup
 * toggle).  Green accent when `accent_ok`, gray otherwise. */
void um_gui_win_notify(um_gui *g, const char *title, const char *body,
                        int accent_ok)
{
    toast_data *td = malloc(sizeof *td);
    wchar_t wbuf[192];

    if (!td) return;
    memset(td, 0, sizeof *td);
    td->is_add = accent_ok ? 1 : 0;
    td->ttl = g->toast_ttl;
    td->slot = g->slot_seq % UM_GUI_SLOTS;
    g->slot_seq++;

    wcopy(title, wbuf, 192);
    wcscpy_s(td->lines[td->n_lines], 192, wbuf);
    td->n_lines++;
    if (body && body[0]) {
        wcopy(body, wbuf, 192);
        wcscpy_s(td->lines[td->n_lines], 192, wbuf);
        td->n_lines++;
    }
    td->n_dim_from = 1;   /* title bright, body dim */

    if (!PostThreadMessageW(g->gui_tid, UMWM_TOAST, 1, (LPARAM)td))
        free(td);
}

void um_gui_win_shutdown(um_gui *g)
{
    if (g->gui_tid)
        PostThreadMessageW(g->gui_tid, UMWM_QUIT, 0, 0);
    if (g->gui_thread) {
        WaitForSingleObject(g->gui_thread, 2000);
        CloseHandle(g->gui_thread);
        g->gui_thread = NULL;
    }
    if (g->wake_event) {
        CloseHandle(g->wake_event);
        g->wake_event = NULL;
    }
}

#endif /* _WIN32 */
