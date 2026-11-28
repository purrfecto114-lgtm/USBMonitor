/* Owns the Win32 listener, tray and presentation on one message thread. */
#include "win_app.h"
#include "um_toast_win32.h"

#include <dbt.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    um_toast_model model;
    int show;
} panel_update;

static um_gui *app;
static um_toast_win *panel, *notice;
static um_toast_model inventory;
static int inventory_ready;
static int show_others_pref;    /* 托盘“显示其他设备”偏好（2.6.3） */

const um_toast_model *um_gui_win_inventory(void)
{
    return inventory_ready ? &inventory : NULL;
}

static void test_panel(const char *action, int row, const char *arg)
{
    FILE *f = um_win_test_output("USBMON_PANEL_TEST");
    if (!f) return;
    if (!strcmp(action, "show")) {
        const um_toast_state *st = panel ? um_toast_win_state(panel) : NULL;
        int i, total = st ? um_toast_visible_count(&inventory, st)
                          : inventory.n_storage;
        fprintf(f, "panel show\n  head %s\n  sub %s\n  count %s\n  rows %d total %d\n",
                inventory.headline,
                (st && st->show_others && inventory.subtitle_all[0])
                    ? inventory.subtitle_all : inventory.subtitle,
                (st && st->show_others && inventory.count_all[0])
                    ? inventory.count_all : inventory.count,
                total, inventory.n_rows);
        for (i = 0; i < total; i++) {
            int index = st ? um_toast_visible_row(&inventory, st, i) : i;
            if (index < 0) continue;
            fprintf(f, "  row%d %s | %s\n", i, inventory.rows[index].title,
                    inventory.rows[index].subtitle);
        }
    } else {
        fprintf(f, "action %s row=%d %s\n", action, row, arg);
    }
    fclose(f);
}

static void copy_mount(HWND owner, char letter)
{
    wchar_t path[] = L"A:\\";
    HGLOBAL mem;
    void *dst;
    path[0] = letter;
    if (!OpenClipboard(owner)) return;
    mem = GlobalAlloc(GMEM_MOVEABLE, sizeof path);
    if (mem) {
        dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, path, sizeof path);
            GlobalUnlock(mem);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem)) mem = NULL;
        }
        if (mem) GlobalFree(mem);
    }
    CloseClipboard();
}

static void place_notice(void)
{
    int base = um_toast_win_is_visible(panel) ? um_toast_win_pixel_height(panel) : 0;
    if (notice) um_toast_win_move_slot_ex(notice, 0, base);
}

static void panel_action(um_toast_action action, int row, void *user)
{
    const um_toast_model *model = um_toast_win_model(panel);
    const um_volume *v = row >= 0 && row < model->n_rows ? &model->rows[row] : NULL;
    (void)user;
    switch (action) {
    case UM_ACT_NONE: break;
    case UM_ACT_OPEN:
    case UM_ACT_OPEN_ROW:
    case UM_ACT_REVEAL:
    case UM_ACT_COPY:
        if (!v || !v->openable || !v->path[0]) return;
        test_panel(action == UM_ACT_OPEN ? "open" : action == UM_ACT_OPEN_ROW ? "open_row" :
                   action == UM_ACT_REVEAL ? "reveal" : "copy", row, v->letters[0]);
        if (action == UM_ACT_COPY) copy_mount((HWND)um_toast_win_hwnd(panel), v->path[0]);
        else if (!getenv("USBMON_PANEL_TEST"))
            um_tray_open(v->path[0], v->disk_number, action == UM_ACT_REVEAL);
        /* 打开卷后立刻收起面板，让通知叠放基线按"面板不可见"重算。 */
        if (action == UM_ACT_OPEN || action == UM_ACT_OPEN_ROW) {
            um_toast_win_hide(panel);
            place_notice();
        }
        break;
    case UM_ACT_EJECT:
        if (v && v->ejectable && !getenv("USBMON_PANEL_TEST")) um_tray_eject(v->instance_id);
        break;
    case UM_ACT_CLOSE:
        test_panel("close", -1, "");
        um_toast_win_hide(panel);
        place_notice();
        break;
    case UM_ACT_AUTOHIDE:
    case UM_ACT_TOGGLE_EXPAND:
        place_notice();
        break;
    }
}

/* 托盘“显示其他设备”开关：记住偏好并同步到已存在的面板。
 * 面板不存在时只存偏好，下次创建/显示时应用。 */
void um_gui_win_set_show_others(int on)
{
    show_others_pref = on ? 1 : 0;
    if (!panel) return;
    um_toast_win_set_show_others(panel, show_others_pref);
    test_panel("show", -1, "");
    if (um_toast_win_is_visible(panel)) place_notice();
}

static void notice_action(um_toast_action action, int row, void *user)
{
    (void)row;
    (void)user;
    if (action == UM_ACT_CLOSE || action == UM_ACT_AUTOHIDE) um_toast_win_hide(notice);
}

void um_gui_win_notice(const char *title, const char *body, int accent)
{
    um_toast_model m = {0};
    um_theme theme;
    int width;
    um_copy_str(m.headline, sizeof m.headline, title);
    um_copy_str(m.summary, sizeof m.summary, body);
    m.accent_kind = accent;
    width = um_toast_suggest_width(&m);
    um_toast_system_theme(&theme);
    um_toast_win_destroy(notice);
    notice = um_toast_win_new(&m, &theme, 1, width, app->toast_ttl * 1000,
                              notice_action, NULL);
    if (notice) {
        um_toast_win_show(notice);
        place_notice();
    }
    um_tray_test_log("toast", notice ? "create ok" : "create fail");
}

static void show_panel(int interactive)
{
    um_theme theme;
    /* 手动打开（托盘“设备概览”）不倒计时；事件触发的面板才用 toast_ttl。 */
    int ttl = interactive ? 0 : app->toast_ttl * 1000;
    if (!inventory_ready) return;
    if (!panel) {
        um_toast_system_theme(&theme);
        panel = um_toast_win_new(&inventory, &theme, 1, 0, ttl,
                                 panel_action, NULL);
    } else {
        um_toast_win_update(panel, &inventory);
    }
    if (!panel) return;
    /* 手动打开或面板本来隐藏时重设 ttl；事件刷新已可见面板时保留原值，
     * 避免把事件倒计时或手动常驻状态改掉。 */
    if (interactive || !um_toast_win_is_visible(panel))
        um_toast_win_set_ttl(panel, ttl);
    um_toast_win_show(panel);
    /* show() 首次显示会重建状态，偏好必须在 show 之后应用。 */
    um_toast_win_set_show_others(panel, show_others_pref);
    if (interactive) SetForegroundWindow((HWND)um_toast_win_hwnd(panel));
    place_notice();
    test_panel("show", -1, "");
}

/* 首次扫描还没完成时也给出面板：8 点旋转 + "正在扫描设备"，而不是一个
 * 会立刻自动消失的普通通知（2.6.2 扫描视觉）。 */
static void show_scanning_panel(void)
{
    um_toast_model m = {0};
    um_theme theme;
    if (!panel) {
        snprintf(m.headline, sizeof m.headline, "正在扫描设备");
        snprintf(m.subtitle, sizeof m.subtitle, "USB 设备监控");
        snprintf(m.count, sizeof m.count, "…");
        snprintf(m.summary, sizeof m.summary, "正在枚举存储设备与卷…");
        snprintf(m.status, sizeof m.status, "完成后可在托盘菜单查看设备概览");
        m.scanning = 1;
        um_toast_system_theme(&theme);
        panel = um_toast_win_new(&m, &theme, 1, 0, 0, panel_action, NULL);
    } else {
        um_toast_win_set_scanning(panel, 1);
    }
    if (!panel) return;
    /* 扫描面板只由手动打开触发：不自动隐藏。 */
    um_toast_win_set_ttl(panel, 0);
    um_toast_win_show(panel);
    um_toast_win_set_show_others(panel, show_others_pref);
    SetForegroundWindow((HWND)um_toast_win_hwnd(panel));
    place_notice();
    test_panel("scan", -1, "");
}

void um_gui_win_open_panel(void)
{
    if (inventory_ready) show_panel(1);
    else show_scanning_panel();
}

static LRESULT CALLBACK listener_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (app->requested && um_tray_filter(msg, wp, lp)) return 0;
    switch (msg) {
    case WM_DEVICECHANGE:
        if (app->watch_devices && (wp == DBT_DEVNODES_CHANGED ||
            wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE))
            SetEvent((HANDLE)app->wake_event);
        return TRUE;
    case UMWM_PANEL: {
        panel_update *update = (void *)lp;
        inventory = update->model;
        inventory_ready = 1;
        if (update->show) show_panel(0);
        else if (panel) um_toast_win_update(panel, &inventory);
        free(update);
        return 0;
    }
    case UMWM_EJECT_DONE:
        um_tray_finish_eject((unsigned long)wp);
        return 0;
    /* 托盘菜单命令走标准 WM_COMMAND（ID_PANEL == 1，与 tray_win32.c 的
     * 私有枚举一致）。打开动作收起面板后，冒烟测试也能据此再次打开。 */
    case WM_COMMAND:
        if (LOWORD(wp) == UM_TRAY_ID_PANEL) um_gui_win_open_panel();
        else if (LOWORD(wp) == UM_TRAY_ID_OTHERS) um_tray_toggle_show_others();
        return 0;
    case WM_SYSCOLORCHANGE:
    case WM_SETTINGCHANGE: {
        um_theme theme;
        um_toast_system_theme(&theme);
        um_toast_win_set_theme(panel, &theme);
        um_toast_win_set_theme(notice, &theme);
        break;
    }
    case WM_CLOSE:
        um_request_stop();
        return 0;
    case UMWM_QUIT:
        EndMenu();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static DWORD WINAPI message_thread(void *arg)
{
    WNDCLASSW wc = {0};
    MSG msg;
    HWND listener;
    app = arg;
    wc.lpfnWndProc = listener_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"usbmonListen";
    if (!RegisterClassW(&wc)) return 1;
    if (app->requested && !um_toast_win_init(wc.hInstance))
        um_tray_test_log("panel", "class register fail");

    listener = CreateWindowExW(0, wc.lpszClassName, L"usbmon", WS_OVERLAPPED,
                               0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!listener) return 1;
    app->listener = listener;
    SetEvent((HANDLE)app->ready_event);
    if (app->requested) um_tray_install(listener, app);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    while (PeekMessageW(&msg, listener, UMWM_PANEL, UMWM_PANEL, PM_REMOVE))
        free((void *)msg.lParam);
    um_tray_uninstall();
    um_toast_win_destroy(panel);
    um_toast_win_destroy(notice);
    DestroyWindow(listener);
    return 0;
}

int um_gui_init(um_gui *g)
{
    HANDLE wait_for[2];
    if (!g->requested && !g->watch_devices) return 0;
    g->wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g->wake_event && g->ready_event)
        g->gui_thread = CreateThread(NULL, 0, message_thread, g, 0, NULL);
    wait_for[0] = g->ready_event;
    wait_for[1] = g->gui_thread;
    if (g->gui_thread && WaitForMultipleObjects(2, wait_for, FALSE, INFINITE) == WAIT_OBJECT_0) {
        g->enabled = g->requested;
        return g->enabled;
    }
    um_gui_shutdown(g);
    return 0;
}

void um_gui_reap(um_gui *g) { (void)g; }

void um_gui_shutdown(um_gui *g)
{
    if (g->gui_thread) {
        if (g->listener) PostMessageW((HWND)g->listener, UMWM_QUIT, 0, 0);
        WaitForSingleObject((HANDLE)g->gui_thread, INFINITE);
        CloseHandle((HANDLE)g->gui_thread);
    }
    if (g->wake_event) CloseHandle((HANDLE)g->wake_event);
    if (g->ready_event) CloseHandle((HANDLE)g->ready_event);
    g->gui_thread = g->wake_event = g->ready_event = g->listener = NULL;
}

static int contains(const um_evidence *items, int n, const char *id)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!strcmp(items[i].instance_id, id)) return 1;
    }
    return 0;
}

void um_gui_win_refresh(um_gui *g, const um_evidence *evidence, int n,
                        int added, int removed)
{
    static um_evidence previous[UM_EVID_MAX];
    static int last_n = -1;
    panel_update *update = malloc(sizeof *update);
    int i;
    if (!update) return;

    if (last_n < 0 && getenv("USBMON_PANEL_TEST")) {
        static const um_evidence test[] = {
            { .tags = UM_TAG_USB | UM_TAG_STORAGE, .disk_number = 2, .ejectable = 1,
              .letters = (1U << 4) | (1U << 5), .accessible_letters = 1U << 4,
              .volumes = 2, .kind = UM_KIND_USB_STORAGE,
              .fs_total = 62706522521ULL, .fs_free = 21582211482ULL },
            { .tags = UM_TAG_USB | UM_TAG_STORAGE, .disk_number = 3, .ejectable = 1,
              .letters = 1U << 7, .accessible_letters = 1U << 7,
              .volumes = 1, .kind = UM_KIND_USB_STORAGE, .model = "SD 卡",
              .fs_total = 127775277056ULL, .fs_free = 82141183590ULL },
            { .disk_number = -1, .kind = UM_KIND_DOCK_HUB },
            { .disk_number = -1, .kind = UM_KIND_HID_PEN },
        };
        um_enum_to_model(test, 4, &update->model);
        update->show = 1;
    } else {
        if (last_n >= 0) {
            for (i = 0; i < n; i++)
                if (!(evidence[i].tags & UM_TAG_STORAGE) &&
                    !contains(previous, last_n, evidence[i].instance_id)) added++;
            for (i = 0; i < last_n; i++)
                if (!(previous[i].tags & UM_TAG_STORAGE) &&
                    !contains(evidence, n, previous[i].instance_id)) removed++;
        }
        um_enum_to_model(evidence, n, &update->model);
        update->show = added || removed;
        if (update->show) {
            snprintf(update->model.summary, sizeof update->model.summary,
                     "本轮设备变化   +%d 接入   −%d 移除", added, removed);
            update->model.accent_kind = added ? 1 : 3;
        }
    }
    memcpy(previous, evidence, (size_t)n * sizeof *evidence);
    last_n = n;
    if (!PostMessageW((HWND)g->listener, UMWM_PANEL, 0, (LPARAM)update)) free(update);
}
