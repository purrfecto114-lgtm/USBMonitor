/* Native tray menus and volume actions. Inventory belongs to the GUI thread. */
#include "win_app.h"

#include <cfgmgr32.h>
#include <io.h>
#include <process.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
    ID_PANEL = 1, ID_RESCAN, ID_LOG, ID_CONFIG, ID_STARTUP, ID_QUIT, ID_OTHERS,
    ID_OPEN = 100, ID_REVEAL = 130, ID_EJECT = 160
};

/* Compile-time guard: the menu IDs must match win_app.h, which the GUI
 * thread uses for WM_COMMAND dispatch.  File-scope typedefs are the C99
 * equivalent of a static assert (negative array size on mismatch). */
typedef char um_tray_id_panel_check[(ID_PANEL == UM_TRAY_ID_PANEL) ? 1 : -1];
typedef char um_tray_id_others_check[(ID_OTHERS == UM_TRAY_ID_OTHERS) ? 1 : -1];

typedef struct {
    int disk;
    char instance[UM_EVID_ID_LEN];
} tray_entry;

static HWND owner;
static um_gui *app;
static UINT taskbar_created;
static HICON icon;
static int in_menu, eject_pending;

FILE *um_win_test_output(const char *variable)
{
    const char *path = getenv(variable);
    LARGE_INTEGER bytes;
    FILE *f;
    if (!path || !*path || !(f = fopen(path, "ab"))) return NULL;
    if (GetFileSizeEx((HANDLE)_get_osfhandle(_fileno(f)), &bytes) && bytes.QuadPart == 0)
        fputs("\xef\xbb\xbf", f);
    return f;
}

void um_tray_test_log(const char *key, const char *detail)
{
    FILE *f = um_win_test_output("USBMON_TRAY_TEST");
    if (!f) return;
    fprintf(f, "%s %s\n", key, detail);
    fclose(f);
}

/* ---- "显示其他设备" preference (HKCU\Software\USBMonitor\ShowOthers) ---- */

static int show_others_pref;

static int tray_test_mode(void)
{
    return getenv("USBMON_PANEL_TEST") || getenv("USBMON_TRAY_TEST");
}

static int load_show_others(void)
{
    DWORD value = 0, size = sizeof value;
    if (tray_test_mode()) return 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\USBMonitor", L"ShowOthers",
                     RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS)
        return 0;
    return value ? 1 : 0;
}

static void save_show_others(int on)
{
    if (tray_test_mode()) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\USBMonitor", L"ShowOthers",
                    REG_DWORD, &on, sizeof on);
}

void um_tray_toggle_show_others(void)
{
    show_others_pref = !show_others_pref;
    save_show_others(show_others_pref);
    um_gui_win_set_show_others(show_others_pref);
    um_tray_test_log("others", show_others_pref ? "on" : "off");
}

static void dump_menu(FILE *f, HMENU menu, int depth)
{
    int i;
    for (i = 0; i < GetMenuItemCount(menu); i++) {
        wchar_t text[256];
        char utf8[512];
        MENUITEMINFOW info = {0};
        info.cbSize = sizeof info;
        info.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_STATE;
        info.dwTypeData = text;
        info.cch = sizeof text / sizeof *text;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &info)) continue;
        um_wide_to_utf8(text, utf8, sizeof utf8);
        fprintf(f, "%*s%s%s\n", depth * 2, "", (info.fState & MFS_CHECKED) ? "[x] " : "",
                utf8);
        if (info.hSubMenu) dump_menu(f, info.hSubMenu, depth + 1);
    }
}

static int popup(HMENU menu, const char *name)
{
    FILE *f = um_win_test_output("USBMON_TRAY_TEST");
    int command = 0;
    if (f) {
        fprintf(f, "menu %s\n", name);
        dump_menu(f, menu, 0);
        fclose(f);
    } else {
        POINT pt;
        if (GetCursorPos(&pt)) {
            SetForegroundWindow(owner);
            in_menu = 1;
            command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        pt.x, pt.y, owner, NULL);
            in_menu = 0;
            PostMessageW(owner, WM_NULL, 0, 0);
        }
    }
    DestroyMenu(menu);
    return command;
}

static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

int um_startup_enabled(void)
{
    DWORD type;
    return RegGetValueW(HKEY_CURRENT_USER, run_key, L"usbmon", RRF_RT_REG_SZ,
                        &type, NULL, NULL) == ERROR_SUCCESS;
}

int um_startup_install(void)
{
    wchar_t path[MAX_PATH], command[MAX_PATH + 3];
    HKEY key;
    LONG result;
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!len || len > MAX_PATH - 2) return -1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) != ERROR_SUCCESS) return -1;
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\"", path);
    result = RegSetValueExW(key, L"usbmon", 0, REG_SZ, (const BYTE *)command,
                           (DWORD)((wcslen(command) + 1) * sizeof *command));
    RegCloseKey(key);
    return result == ERROR_SUCCESS ? 0 : -1;
}

int um_startup_uninstall(void)
{
    HKEY key;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_SET_VALUE, &key);
    if (result == ERROR_FILE_NOT_FOUND) return 0;
    if (result != ERROR_SUCCESS) return -1;
    result = RegDeleteValueW(key, L"usbmon");
    RegCloseKey(key);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND ? 0 : -1;
}

const char *um_startup_where(void) { return "注册表 HKCU Run\\usbmon"; }

void um_tray_open(char letter, int disk, int reveal)
{
    wchar_t path[] = L"A:\\";
    HINSTANCE result;
    if (disk < 0 || um_enum_letter_disk(letter) != disk) {
        um_gui_win_notice("设备状态已变化", "请重新扫描后再打开卷。", 2);
        return;
    }
    path[0] = (wchar_t)letter;
    if (reveal) {
        wchar_t explorer[MAX_PATH + 16], args[] = L"/select,A:\\";
        UINT len = GetWindowsDirectoryW(explorer, MAX_PATH);
        if (!len || len >= MAX_PATH) return;
        wcscat(explorer, L"\\explorer.exe");
        args[8] = path[0];
        result = ShellExecuteW(owner, L"open", explorer, args, NULL, SW_SHOWNORMAL);
    } else {
        result = ShellExecuteW(owner, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
    if ((INT_PTR)result <= 32)
        um_gui_win_notice("无法打开卷", "请检查卷是否可访问。", 2);
}

typedef struct {
    HWND target;
    char instance[UM_EVID_ID_LEN];
} eject_job;

static unsigned __stdcall eject_worker(void *arg)
{
    eject_job *job = arg;
    unsigned long result = um_enum_eject(job->instance);
    PostMessageW(job->target, UMWM_EJECT_DONE, result, 0);
    free(job);
    return 0;
}

void um_tray_eject(const char *instance)
{
    eject_job *job;
    uintptr_t thread;
    if (eject_pending) return;
    job = malloc(sizeof *job);
    if (!job) {
        um_gui_win_notice("无法发起弹出", "请稍后再试。", 2);
        return;
    }
    job->target = owner;
    um_copy_str(job->instance, sizeof job->instance, instance);
    thread = _beginthreadex(NULL, 0, eject_worker, job, 0, NULL);
    if (!thread) {
        free(job);
        um_gui_win_notice("无法发起弹出", "请稍后再试。", 2);
        return;
    }
    eject_pending = 1;
    CloseHandle((HANDLE)thread);
    um_gui_win_notice("正在安全弹出", "等待 Windows 完成设备移除请求。", 0);
}

void um_tray_finish_eject(unsigned long result)
{
    char detail[128];
    if (!eject_pending) return;
    eject_pending = 0;
    if (result == CR_SUCCESS) {
        um_gui_win_notice("设备已安全移除", "Windows 已完成设备移除。", 1);
        SetEvent((HANDLE)app->wake_event);
    } else {
        snprintf(detail, sizeof detail, "Windows 未完成移除（错误 0x%lX）。", result);
        um_gui_win_notice("无法安全移除", detail, 2);
    }
    um_tray_test_log("eject", result == CR_SUCCESS ? "ok" : "fail");
}

static void left_menu(void)
{
    const um_toast_model *model = um_gui_win_inventory();
    tray_entry entries[26] = {{0}};
    HMENU menu = CreatePopupMenu();
    int i, cmd, storage = 0;
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_PANEL, L"打开设备概览");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (i = 0; model && i < model->n_rows; i++) {
        const um_volume *v = &model->rows[i];
        int j;
        if (v->disk_number < 0) continue;
        storage++;
        if (!v->n_letters) {
            wchar_t title[192];
            char text[240];
            snprintf(text, sizeof text, "%s · 无盘符", v->title);
            um_utf8_to_wide(text, title, sizeof title / sizeof *title);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title);
        }
        for (j = 0; j < v->n_letters; j++) {
            int index = v->letters[j][0] - 'A';
            wchar_t title[192];
            char text[160];
            UINT open_flags;
            HMENU sub;
            if (index < 0 || index >= 26) continue;
            sub = CreatePopupMenu();
            if (!sub) continue;
            snprintf(text, sizeof text, "%s · %s", v->letters[j], v->title);
            um_utf8_to_wide(text, title, sizeof title / sizeof *title);
            entries[index].disk = v->disk_number;
            um_copy_str(entries[index].instance, sizeof entries[index].instance,
                         v->instance_id);
            open_flags = MF_STRING | ((v->accessible_letters & (1U << index)) ? 0 : MF_GRAYED);
            AppendMenuW(sub, open_flags, ID_OPEN + index, L"打开");
            AppendMenuW(sub, open_flags, ID_REVEAL + index, L"在资源管理器中显示");
            AppendMenuW(sub, MF_STRING | (v->ejectable && !eject_pending ? 0 : MF_GRAYED),
                        ID_EJECT + index, L"安全弹出");
            if (!AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, title)) DestroyMenu(sub);
        }
    }
    if (!storage)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, model ?
                    L"当前没有检测到 USB 存储设备" : L"正在扫描设备…");
    cmd = popup(menu, "left");
    if (cmd == ID_PANEL) um_gui_win_open_panel();
    else if (cmd >= ID_OPEN && cmd < ID_OPEN + 26)
        um_tray_open((char)('A' + cmd - ID_OPEN), entries[cmd - ID_OPEN].disk, 0);
    else if (cmd >= ID_REVEAL && cmd < ID_REVEAL + 26)
        um_tray_open((char)('A' + cmd - ID_REVEAL), entries[cmd - ID_REVEAL].disk, 1);
    else if (cmd >= ID_EJECT && cmd < ID_EJECT + 26)
        um_tray_eject(entries[cmd - ID_EJECT].instance);
}

static void open_directory(int log)
{
    char path[512];
    wchar_t wide[512];
    if (log) um_state_dir(path, sizeof path);
    else um_config_dir(path, sizeof path);
    um_utf8_to_wide(path, wide, sizeof wide / sizeof *wide);
    if (wide[0]) ShellExecuteW(owner, L"open", wide, NULL, NULL, SW_SHOWNORMAL);
}

static void right_menu(void)
{
    const um_toast_model *model = um_gui_win_inventory();
    HMENU menu = CreatePopupMenu();
    wchar_t status[128];
    if (!menu) return;
    if (model) swprintf(status, 128, L"状态：已检测到 %d 个设备", model->n_rows);
    else wcscpy(status, L"状态：正在扫描设备");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_PANEL, L"打开设备概览");
    AppendMenuW(menu, MF_STRING, ID_RESCAN, L"立即重新扫描");
    AppendMenuW(menu, MF_STRING | (show_others_pref ? MF_CHECKED : 0),
                ID_OTHERS, L"显示其他设备");
    AppendMenuW(menu, MF_STRING, ID_LOG, L"打开日志目录");
    AppendMenuW(menu, MF_STRING, ID_CONFIG, L"打开配置目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (um_startup_enabled() ? MF_CHECKED : 0),
                ID_STARTUP, L"随系统启动");
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"退出");

    switch (popup(menu, "right")) {
    case ID_PANEL: um_gui_win_open_panel(); break;
    case ID_RESCAN: SetEvent((HANDLE)app->wake_event); break;
    case ID_OTHERS: um_tray_toggle_show_others(); break;
    case ID_LOG: open_directory(1); break;
    case ID_CONFIG: open_directory(0); break;
    case ID_STARTUP: {
        int enabled = um_startup_enabled();
        if ((enabled ? um_startup_uninstall() : um_startup_install()) == 0)
            um_gui_win_notice("随系统启动", enabled ? "已关闭登录自启动。" : "已开启登录自启动。", 1);
        else
            um_gui_win_notice("自启动设置未保存", "Windows 未能更新登录自启动项。", 2);
        break;
    }
    case ID_QUIT: um_request_stop(); break;
    }
}

static NOTIFYICONDATAW icon_data(void)
{
    NOTIFYICONDATAW data = {0};
    data.cbSize = sizeof data;
    data.hWnd = owner;
    data.uID = 1;
    return data;
}

static void add_icon(const char *tag)
{
    NOTIFYICONDATAW data = icon_data();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = UMWM_TRAY;
    data.hIcon = icon;
    wcscpy(data.szTip, L"usbmon · USB 设备监控");
    um_tray_test_log(tag, Shell_NotifyIconW(NIM_ADD, &data) ? "ok" : "fail");
}

void um_tray_install(HWND hwnd, um_gui *gui)
{
    owner = hwnd;
    app = gui;
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!icon) icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    show_others_pref = load_show_others();
    um_gui_win_set_show_others(show_others_pref);
    add_icon("icon_add");
}

void um_tray_uninstall(void)
{
    NOTIFYICONDATAW data = icon_data();
    if (!owner) return;
    Shell_NotifyIconW(NIM_DELETE, &data);
    owner = NULL;
}

int um_tray_filter(UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (!owner) return 0;
    if (taskbar_created && msg == taskbar_created) {
        add_icon("icon_readd");
        return 1;
    }
    switch (msg) {
    case UMWM_TRAY:
        if (!in_menu) {
            if (lp == WM_LBUTTONUP) left_menu();
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) right_menu();
        }
        return 1;
    case UMWM_TRAY_RESCAN:
        SetEvent((HANDLE)app->wake_event);
        return 1;
    case UMWM_TRAY_QUIT:
        um_request_stop();
        return 1;
    case UMWM_TRAY_EJECT_TEST:
        if (getenv("USBMON_TRAY_TEST")) um_tray_eject("");
        return 1;
    default:
        return 0;
    }
}
