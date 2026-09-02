/* gui_win32.c — Windows GUI backend: toasts + WM_DEVICECHANGE hot path.
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
 *   - top-level toast windows (WS_POPUP | WS_EX_TOPMOST | tool window,
 *     bottom-right stacking, WM_TIMER auto-dismiss, click to dismiss).
 *
 * user32/gdi32 ship with every Windows install, so unlike POSIX there
 * is no reason to split rendering into a helper process; a thread is
 * enough and keeps the message pump on one owner.
 *
 * Verified on a real Windows machine by CI (windows-latest runner runs
 * tools/demo.ps1, including a simulated WM_DEVICECHANGE broadcast that
 * must wake the daemon and log a "wake":"hot" round).
 */
#include "usbmon.h"

#ifdef _WIN32

#include <windows.h>
#include <dbt.h>           /* DEV_BROADCAST_*, DBT_* (device-change events) */
#include <wchar.h>         /* wcslen, wcscpy_s */
#include <string.h>
#include <stdlib.h>

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

#define UMWM_TOAST   (WM_APP + 1)   /* wparam: 1 add / 0 remove; lparam: toast_data*  */
#define UMWM_QUIT    (WM_APP + 2)

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
        int i, y;

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
    if (listener)
        SetWindowLongPtrW(listener, GWLP_USERDATA, (LONG_PTR)g->wake_event);

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
        if (msg.message == UMWM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (listener) DestroyWindow(listener);
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
    return 1;
}

void um_gui_win_show(um_gui *g, const um_device *dev, int is_add)
{
    toast_data *td = toast_data_make(dev, is_add, g);
    if (!td) return;
    if (!PostThreadMessageW(g->gui_tid, UMWM_TOAST, (WPARAM)is_add,
                            (LPARAM)td)) {
        free(td);            /* GUI thread gone: drop the toast quietly */
    }
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
