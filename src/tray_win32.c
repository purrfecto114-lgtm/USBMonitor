/* tray_win32.c — Windows system tray (Shell_NotifyIcon) + menus.
 *
 * Restores the 1.x PySide6 tray experience in native Win32, split the
 * same way the original TrayMenuController split it:
 *   left click  -> USB-device menu (per drive letter:
 *                  打开 / 在资源管理器中显示 / 安全弹出)
 *   right click -> application menu (状态 / 立即重新扫描 / 工具 /
 *                  随系统启动 / 退出)
 *
 * Everything runs on the GUI thread that already owns the invisible
 * WM_DEVICECHANGE listener, so menus and device-change wakeups share a
 * single message pump; TrackPopupMenu's modal loop can dispatch hot-path
 * messages while a menu is open and can never deadlock the daemon.
 *
 * The device list is a FRESH um_scan_platform() round at popup time —
 * no state shared with the main thread, no locks, always current.
 *
 * 安全弹出 uses IOCTL_STORAGE_EJECT_MEDIA, the primary path of the
 * Python original (Shell COM was only ever its fallback); the result is
 * confirmed by watching the drive letter disappear, surfaced as a toast.
 *
 * Testability: when USBMON_TRAY_TEST names a writable file, the tray
 * install result and every menu's full item list are appended there
 * (UTF-8, one line per item, submenu lines indented) and TrackPopupMenu
 * is skipped — tools/demo.ps1 asserts menu CONTENT on a headless CI
 * runner by posting the very same window messages a real click sends.
 */
#ifdef _WIN32

#include "usbmon.h"

#include <windows.h>
#include <shellapi.h>     /* Shell_NotifyIconW, ShellExecuteW */
#include <winioctl.h>
#include <process.h>      /* _beginthreadex: async safe-eject worker */
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FILE_DEVICE_MASS_STORAGE
#define FILE_DEVICE_MASS_STORAGE 0x0000002d
#endif
#ifndef IOCTL_STORAGE_EJECT_MEDIA
#define IOCTL_STORAGE_EJECT_MEDIA \
    CTL_CODE(FILE_DEVICE_MASS_STORAGE, 0x0202, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

/* ---- menu command ids ---------------------------------------------------- */
#define ID_RESCAN     1
#define ID_OPEN_LOG   2
#define ID_OPEN_CONF  3
#define ID_STARTUP    4
#define ID_QUIT       5
#define IDB_OPEN      100   /* + entry index (per drive letter) */
#define IDB_REVEAL    200
#define IDB_EJECT     300
#define TRAY_MAX_ENTRIES 32

typedef struct {
    char letter;                     /* 'E' — volume letter of the entry  */
    char model[UM_MODEL_MAX];        /* for the eject feedback toast      */
} tray_entry;

static HWND       g_owner;
static um_gui    *g_gui;
static UINT       g_msg_taskbar;     /* "TaskbarCreated" (explorer restart) */
static HICON      g_icon;

/* ---- test log (USBMON_TRAY_TEST=path; absent in production) --------------- */

static FILE *tray_test_out(void)
{
    const char *p = getenv("USBMON_TRAY_TEST");
    FILE *f;
    long sz;
    if (!p || !*p) return NULL;
    f = fopen(p, "a");
    if (!f) return NULL;
    /* BOM once — only when the file is truly empty.  ftell() immediately
     * after fopen("a") is implementation-defined (msvcrt vs glibc differ),
     * so seek to end first: writes still land at the end in append mode. */
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz == 0)
        fwrite("\xEF\xBB\xBF", 1, 3, f);
    return f;
}

static void tray_log_result(const char *key, int ok)
{
    FILE *f = tray_test_out();
    if (!f) return;
    fprintf(f, "%s %s\n", key, ok ? "ok" : "fail");
    fclose(f);
}

static void dump_level(FILE *f, HMENU m, int depth)
{
    int n = GetMenuItemCount(m), i;
    for (i = 0; i < n; i++) {
        MENUITEMINFOW mii;
        wchar_t buf[256];
        char u8[512];
        memset(&mii, 0, sizeof mii);
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_STRING | MIIM_SUBMENU;
        buf[0] = L'\0';
        mii.dwTypeData = buf;
        mii.cch = 256;
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mii)) continue;
        um_wide_to_utf8(buf, u8, sizeof u8);
        fprintf(f, "%*s%s\n", depth * 2, "", u8);
        if (mii.hSubMenu) dump_level(f, mii.hSubMenu, depth + 1);
    }
}

static void tray_dump_menu(const char *tag, HMENU m)
{
    FILE *f = tray_test_out();
    if (!f) return;
    fprintf(f, "%s\n", tag);
    dump_level(f, m, 0);
    fclose(f);
}

/* ---- registry: HKCU Run key for 随系统启动 (no admin needed) ---------------- */

static const WCHAR RUN_KEY[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR RUN_VAL[] = L"usbmon";

static int startup_enabled(void)
{
    HKEY k;
    LONG r;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k)
        != ERROR_SUCCESS)
        return 0;
    r = RegQueryValueExW(k, RUN_VAL, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void startup_set(int on)
{
    HKEY k;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 4];
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
            return;
        GetModuleFileNameW(NULL, path, MAX_PATH);
        _snwprintf_s(cmd, MAX_PATH + 4, _TRUNCATE, L"\"%s\"", path);
        RegSetValueExW(k, RUN_VAL, 0, REG_SZ,
                       (const BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    } else {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k)
            != ERROR_SUCCESS)
            return;
        RegDeleteValueW(k, RUN_VAL);
        RegCloseKey(k);
    }
}

/* ---- volume actions ------------------------------------------------------- */

static void tray_open_volume(char letter)
{
    wchar_t root[4];
    _snwprintf_s(root, 4, _TRUNCATE, L"%c:\\", letter);
    ShellExecuteW(g_owner, L"open", root, NULL, NULL, SW_SHOWNORMAL);
}

static void tray_reveal_volume(char letter)
{
    wchar_t params[16];
    _snwprintf_s(params, 16, _TRUNCATE, L"/select,%c:\\", letter);
    ShellExecuteW(g_owner, NULL, L"explorer.exe", params, NULL, SW_SHOWNORMAL);
}

/* 0 = ejected, 1 = request accepted but still mounted, -1 = refused.
 * Runs on the async-eject WORKER thread: plain Sleep only — message
 * pumping belongs to the thread that owns the windows. */
static int eject_volume_sync(char letter)
{
    wchar_t vol[8];
    HANDLE h;
    DWORD br = 0;
    int i;

    _snwprintf_s(vol, 8, _TRUNCATE, L"\\\\.\\%c:", letter);
    h = CreateFileW(vol, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0,
                         &br, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    for (i = 0; i < 75; i++) {              /* ~3 s for the letter to drop */
        Sleep(40);
        if (!(GetLogicalDrives() & (1UL << (letter - 'A')))) return 0;
    }
    return 1;
}

/* Everything the eject worker needs, heap-copied before the thread starts
 * (tray entries can be refreshed by a rescan mid-eject).  gui_tid/ttl/slot
 * are captured BY VALUE on the GUI thread — the worker never touches any
 * shared state, it only PostThreadMessageW's its result toast back. */
typedef struct {
    char          letter;
    char          model[UM_MODEL_MAX];
    unsigned long gui_tid;
    int           ttl;
    int           slot;
} eject_job;

static unsigned __stdcall eject_worker(void *arg)
{
    eject_job *j = (eject_job *)arg;
    char title[64], body[256];
    int r = eject_volume_sync(j->letter);

    snprintf(title, sizeof title, "安全弹出 %c:", j->letter);
    if (r == 0)
        snprintf(body, sizeof body, "%s 已安全弹出，现在可以拔出。",
                 j->model[0] ? j->model : "设备");
    else if (r == 1)
        snprintf(body, sizeof body,
                 "已发送弹出请求，但 %c: 仍可访问。请关闭占用它的文件后重试。",
                 j->letter);
    else
        snprintf(body, sizeof body,
                 "无法弹出 %c:（设备被占用或系统拒绝）。请关闭相关文件后重试。",
                 j->letter);
    /* same slot as the "正在安全弹出" line -> replaces it in place */
    um_gui_win_post(j->gui_tid, title, body, r == 0, j->slot, j->ttl);
    tray_log_result("eject", r == 0);
    free(j);
    return 0;
}

/* v1.1.1 parity: eject runs on a worker thread so the GUI thread (message
 * pump for WM_DEVICECHANGE, tray menus, toasts) never stalls 1~10 s inside
 * DeviceIoControl.  The user sees "正在安全弹出…" immediately; the same
 * toast slot is then upgraded to the final result by the worker. */
static void tray_do_eject(const tray_entry *e)
{
    eject_job *j;

    if (!g_gui || !g_gui->gui_tid) return;
    j = malloc(sizeof *j);
    if (!j) return;
    memset(j, 0, sizeof *j);
    j->letter = e->letter;
    snprintf(j->model, sizeof j->model, "%s", e->model);
    j->gui_tid = g_gui->gui_tid;
    j->ttl = g_gui->toast_ttl;
    j->slot = (int)(g_gui->slot_seq++ % UM_GUI_SLOTS);

    um_gui_win_post(j->gui_tid, "正在安全弹出", "正在安全弹出，请稍候…",
                    1, j->slot, 12);
    {
        HANDLE th = (HANDLE)_beginthreadex(NULL, 0, eject_worker, j, 0, NULL);
        if (th) {
            CloseHandle(th);   /* thread keeps running on its own */
        } else {
            /* could not spawn (~never): run inline so the action is never
             * lost — this is exactly the old blocking behavior */
            eject_worker(j);
        }
    }
}

static void tray_open_dir_utf8(const char *dir)
{
    wchar_t w[512];
    um_utf8_to_wide(dir, w, 512);
    if (w[0]) ShellExecuteW(g_owner, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void tray_toggle_startup(void)
{
    int on = !startup_enabled();
    startup_set(on);
    if (g_gui)
        um_gui_win_notify(g_gui, "随系统启动",
                          on ? "已开启：登录时自动启动 usbmon。"
                             : "已关闭：不再随登录启动。", on);
}

/* ---- menus ---------------------------------------------------------------- */

static tray_entry g_entries[TRAY_MAX_ENTRIES];
static int        g_entry_n;

static void tray_dispatch(int cmd)
{
    if (cmd >= IDB_OPEN && cmd < IDB_OPEN + TRAY_MAX_ENTRIES) {
        int idx = cmd - IDB_OPEN;
        if (idx < g_entry_n) tray_open_volume(g_entries[idx].letter);
    } else if (cmd >= IDB_REVEAL && cmd < IDB_REVEAL + TRAY_MAX_ENTRIES) {
        int idx = cmd - IDB_REVEAL;
        if (idx < g_entry_n) tray_reveal_volume(g_entries[idx].letter);
    } else if (cmd >= IDB_EJECT && cmd < IDB_EJECT + TRAY_MAX_ENTRIES) {
        int idx = cmd - IDB_EJECT;
        if (idx < g_entry_n) tray_do_eject(&g_entries[idx]);
    } else {
        switch (cmd) {
        case ID_RESCAN:
            if (g_gui && g_gui->wake_event)
                SetEvent((HANDLE)g_gui->wake_event);
            break;
        case ID_OPEN_LOG: {
            char d[512];
            um_state_dir(d, sizeof d);
            tray_open_dir_utf8(d);
            break;
        }
        case ID_OPEN_CONF: {
            char d[512];
            um_config_dir(d, sizeof d);
            tray_open_dir_utf8(d);
            break;
        }
        case ID_STARTUP:
            tray_toggle_startup();
            break;
        case ID_QUIT:
            um_request_stop("tray-quit");
            break;
        default:
            break;          /* 0 = menu dismissed without a choice */
        }
    }
}

/* Show (or, under test, dump) a menu and dispatch the picked command. */
static void tray_run_menu(HMENU menu, const char *tag)
{
    const char *t = getenv("USBMON_TRAY_TEST");
    int cmd = 0;

    if (t && t[0]) {
        tray_dump_menu(tag, menu);
        DestroyMenu(menu);
        return;
    }

    {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(g_owner);
        cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    (int)pt.x, (int)pt.y, g_owner, NULL);
        PostMessageW(g_owner, WM_NULL, 0, 0);   /* the canonical dismiss fix */
    }
    DestroyMenu(menu);
    tray_dispatch(cmd);
}

/* Left click: the USB-device menu (per drive letter, like the original
 * volume menu; multi-letter sticks get one entry per letter). */
static void tray_menu_devices(void)
{
    um_snapshot snap;
    HMENU menu = CreatePopupMenu();
    int i, j, shown = 0;

    g_entry_n = 0;
    memset(&snap, 0, sizeof snap);
    if (um_scan_platform(&snap, NULL) != 0) snap.count = 0;

    for (i = 0; i < snap.count; i++) {
        const um_device *d = &snap.devs[i];

        if (d->partition_count > 0) {
            for (j = 0; j < d->partition_count && g_entry_n < TRAY_MAX_ENTRIES;
                 j++) {
                char letter = d->partitions[j][0];
                char txt[UM_MODEL_MAX + UM_MOUNT_MAX];
                wchar_t wtxt[UM_MODEL_MAX + UM_MOUNT_MAX];
                HMENU sub;
                int idx = g_entry_n;
                char sz[32];

                if (d->size_bytes > 0) {
                    um_human_size(d->size_bytes, sz, sizeof sz);
                    snprintf(txt, sizeof txt, "%c: · %s (%s)", letter,
                             d->model[0] ? d->model : "USB 存储设备", sz);
                } else {
                    snprintf(txt, sizeof txt, "%c: · %s", letter,
                             d->model[0] ? d->model : "USB 存储设备");
                }
                um_utf8_to_wide(txt, wtxt,
                                sizeof wtxt / sizeof(WCHAR));

                g_entries[idx].letter = letter;
                um_copy_str(g_entries[idx].model, sizeof g_entries[idx].model,
                            d->model);
                g_entry_n++;

                sub = CreatePopupMenu();
                AppendMenuW(sub, MF_STRING, IDB_OPEN + idx, L"打开");
                AppendMenuW(sub, MF_STRING, IDB_REVEAL + idx,
                            L"在资源管理器中显示");
                AppendMenuW(sub, MF_STRING, IDB_EJECT + idx, L"安全弹出");
                AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, wtxt);
                shown++;
            }
        } else {
            /* present but unmounted (raw disk / no filesystem) */
            char txt[UM_MODEL_MAX + 32];
            wchar_t wtxt[UM_MODEL_MAX + 32];
            snprintf(txt, sizeof txt, "%s · 未挂载",
                     d->model[0] ? d->model : "USB 存储设备");
            um_utf8_to_wide(txt, wtxt, sizeof wtxt / sizeof(WCHAR));
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, wtxt);
            shown++;
        }
    }
    if (!shown)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    L"当前没有检测到 USB 存储设备");

    tray_run_menu(menu, "menu left");
}

/* Right click: the compact application menu (settings-oriented, no
 * device list — exactly the split the original documented). */
static void tray_menu_app(void)
{
    um_snapshot snap;
    HMENU menu, tools;
    char st[96];
    wchar_t wst[96];
    int n = 0;

    memset(&snap, 0, sizeof snap);
    if (um_scan_platform(&snap, NULL) == 0) n = snap.count;

    if (n > 0) snprintf(st, sizeof st, "状态：%d 台 USB 存储设备", n);
    else       snprintf(st, sizeof st, "状态：未检测到 USB 存储设备");
    um_utf8_to_wide(st, wst, sizeof wst / sizeof(WCHAR));

    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, wst);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_RESCAN, L"立即重新扫描");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, ID_OPEN_LOG, L"打开日志目录");
    AppendMenuW(tools, MF_STRING, ID_OPEN_CONF, L"打开配置目录");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)tools, L"工具");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (startup_enabled() ? MF_CHECKED : 0),
                ID_STARTUP, L"随系统启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"退出");

    tray_run_menu(menu, "menu right");
}

/* ---- public API (called from gui_win32.c on the GUI thread) ---------------- */

static int tray_add_icon(int readd)
{
    NOTIFYICONDATAW nid;
    char tip8[128];
    wchar_t tip[128];
    int ok;

    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_owner;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = UMWM_TRAY;
    nid.hIcon = g_icon;
    snprintf(tip8, sizeof tip8, "usbmon %s — USB 存储监控", UM_VERSION);
    um_utf8_to_wide(tip8, tip, 128);
    wcscpy_s(nid.szTip, 128, tip);

    ok = Shell_NotifyIconW(NIM_ADD, &nid) ? 1 : 0;
    tray_log_result(readd ? "icon_readd" : "icon_add", ok);
    return ok;
}

void um_tray_install(void *owner, void *gui)
{
    g_owner = (HWND)owner;
    g_gui = (um_gui *)gui;
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");

    g_icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!g_icon)
        g_icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    if (!g_owner) {
        tray_log_result("icon_add", 0);
        return;
    }
    tray_add_icon(0);
}

void um_tray_uninstall(void)
{
    NOTIFYICONDATAW nid;
    if (!g_owner) return;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_owner;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_owner = NULL;
}

/* Returns 1 when the message was tray-related and fully handled.  Called
 * FIRST from the listener window procedure (gui_win32.c). */
int um_tray_filter(void *hwnd, unsigned msg, void *wp, void *lp)
{
    size_t lpv = (size_t)lp;
    size_t wpv = (size_t)wp;

    (void)hwnd;

    if (msg == UMWM_TRAY && g_owner) {
        switch ((UINT)lpv) {
        case WM_LBUTTONUP:
            tray_menu_devices();
            return 1;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_menu_app();
            return 1;
        default:
            return 1;              /* hover/move: swallow, nothing to do */
        }
    }
    if (msg == UMWM_TRAY_RESCAN) {
        if (g_gui && g_gui->wake_event)
            SetEvent((HANDLE)g_gui->wake_event);
        return 1;
    }
    if (msg == UMWM_TRAY_QUIT) {
        um_request_stop("tray-quit");
        return 1;
    }
    if (msg == UMWM_TRAY_EJECT_TEST && tray_test_out()) {
        /* test-only (USBMON_TRAY_TEST set): drive the async eject chain —
         * worker spawn -> IOCTL attempt -> result toast marshaled back —
         * headlessly.  WPARAM packs the drive letter in the low byte. */
        tray_entry e;
        e.letter = (char)(wpv & 0xFF);
        um_copy_str(e.model, sizeof e.model, "Test Stick");
        tray_do_eject(&e);
        return 1;
    }
    if (g_msg_taskbar && msg == g_msg_taskbar) {
        if (g_owner) tray_add_icon(1);   /* explorer restarted: re-add */
        return 1;
    }
    return 0;
}

/* ---- volume actions, shared with the device panel --------------------------
 * gui_win32.c wires the panel's 打开 / 在资源管理器中显示 / 安全弹出 buttons
 * to these, so the tray menus and the panel share ONE implementation of each
 * action (ShellExecuteW / explorer /select / IOCTL eject + confirmation +
 * feedback toast).  GUI thread only; the eject itself hops to a worker. */

void um_tray_open_letter(char letter)
{
    tray_open_volume(letter);
}

void um_tray_reveal_letter(char letter)
{
    tray_reveal_volume(letter);
}

void um_tray_eject_letter(char letter, const char *model)
{
    tray_entry e;
    e.letter = letter;
    um_copy_str(e.model, sizeof e.model, model ? model : "");
    tray_do_eject(&e);
}

/* ---- login autostart: HKCU Run (v1.1.1 --install-startup parity) ---------- */

int um_startup_install(void)
{
    startup_set(1);
    return startup_enabled() ? 0 : -1;
}

int um_startup_uninstall(void)
{
    startup_set(0);
    return startup_enabled() ? -1 : 0;
}

int um_startup_enabled(void)
{
    return startup_enabled();
}

const char *um_startup_where(void)
{
    return "注册表 HKCU Run\\usbmon";
}

#endif /* _WIN32 */
