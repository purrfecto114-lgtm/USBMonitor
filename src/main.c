/* main.c — CLI, snapshot diff, round loop, GUI + hot-path wakeups.
 *
 *   usbmon                  daemon, one scan round per hour (default);
 *                           plug/unplug wake it instantly (uevent /
 *                           WM_DEVICECHANGE) and a toast pops up
 *   usbmon --once           single round (cron / Task Scheduler mode)
 *   usbmon --list           print current devices, exit
 *   usbmon --interval 300   custom round interval (seconds)
 */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "um_enum.h"
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- signals */

#ifndef _WIN32
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload_hooks;
#define stop_requested() (g_stop != 0)
#define stop_reason() "signal"
#else
static volatile LONG g_stop;
static int g_reload_hooks;
#define stop_requested() (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
#define stop_reason() (InterlockedCompareExchange(&g_stop, 0, 0) == 2 ? "tray-quit" : "signal")

void um_request_stop(void)
{
    InterlockedCompareExchange(&g_stop, 2, 0);
}
#endif

#ifndef _WIN32
static void on_signal(int sig)
{
    if (sig == SIGHUP) g_reload_hooks = 1;
    else g_stop = 1;
}
static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
#define win_cli_console_attach() ((void)0)   /* POSIX: nothing to attach */
#else
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT ||
        type == CTRL_LOGOFF_EVENT) InterlockedCompareExchange(&g_stop, 1, 0);
    return TRUE;
}
static void install_signals(void) { SetConsoleCtrlHandler(on_ctrl, TRUE); }

/* Windows GUI-subsystem build (-mwindows): the process owns no console,
 * so double-clicking usbmon.exe never flashes a black window.  When a
 * print-and-exit CLI mode runs from an INTERACTIVE console we attach to
 * the parent's console so --version/--help/--list are still visible.
 * Pipe/redirect captures (CI, `|`, $(...)) keep the inherited handle and
 * skip the attach, so output capture is unaffected. */
static void win_cli_console_attach(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD ft;
    if (h && h != INVALID_HANDLE_VALUE) {
        ft = GetFileType(h);
        if (ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK) return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!freopen("CONOUT$", "w", stdout)) return;
        if (!freopen("CONERR$", "w", stderr)) return;
    }
}
#endif

/* ---------------------------------------------------------------- helpers */

static void path_hint_for(const um_device *d, char *out, size_t n)
{
    if (d->mount[0]) { snprintf(out, n, "%s", d->mount); return; }
#ifdef _WIN32
    if (d->extra[0]) { snprintf(out, n, "%c:\\", d->extra[0]); return; }
#else
    snprintf(out, n, "/dev/%s", d->key);
    return;
#endif
    snprintf(out, n, "%s", d->key);
}

/* ------------------------------------------------------------ event lines */

static void emit_add(um_logger *lg, const um_device *d, int baseline)
{
    char e_model[UM_MODEL_MAX * 6], e_key[UM_KEY_MAX * 6];
    char e_mount[UM_MOUNT_MAX * 6], e_serial[UM_SERIAL_MAX * 6];
    char e_bus[sizeof d->bus * 6], parts[UM_MAX_PARTITIONS * (UM_NAME_MAX * 6 + 3) + 1];
    char ts[40];
    char full[sizeof parts + sizeof e_model + sizeof e_key + sizeof e_mount +
              sizeof e_serial + sizeof e_bus + 320];
    size_t used = 0;
    int i;

    um_json_escape(d->model, e_model, sizeof e_model);
    um_json_escape(d->key, e_key, sizeof e_key);
    um_json_escape(d->mount, e_mount, sizeof e_mount);
    um_json_escape(d->bus, e_bus, sizeof e_bus);
    um_json_escape(lg->raw && d->serial[0] ? d->serial : d->serial_fp,
                   e_serial, sizeof e_serial);
    parts[0] = '\0';
    for (i = 0; i < d->partition_count; i++) {
        char part[UM_NAME_MAX * 6];
        um_json_escape(d->partitions[i], part, sizeof part);
        used += (size_t)snprintf(parts + used, sizeof parts - used, "%s\"%s\"",
                                 i ? "," : "", part);
    }
    um_now_utc_iso(ts, sizeof ts);
    snprintf(full, sizeof full,
             "{\"ts\":\"%s\",\"ev\":\"add\",%s\"key\":\"%s\",\"bus\":\"%s\",\"model\":\"%s\","
             "\"serial\":\"%s\",\"size_bytes\":%llu,\"partitions\":[%s],"
             "\"mount\":\"%s\",\"fs_total_bytes\":%llu,\"fs_free_bytes\":%llu}",
             ts, baseline ? "\"baseline\":1," : "", e_key, e_bus, e_model, e_serial,
             d->size_bytes, parts, e_mount, d->fs_total, d->fs_free);
    um_log_line(lg, full);
}

static void emit_remove(um_logger *lg, const um_device *d)
{
    char e_key[UM_KEY_MAX * 6];
    char full[512];
    char ts[40];
    um_json_escape(d->key, e_key, sizeof e_key);
    um_now_utc_iso(ts, sizeof ts);
    snprintf(full, sizeof full, "{\"ts\":\"%s\",\"ev\":\"remove\",\"key\":\"%s\"}",
             ts, e_key);
    um_log_line(lg, full);
}

static void emit_round(um_logger *lg, int round_no, int external, int interval_s,
                       double scan_ms, const char *wake)
{
    char full[256];
    char ts[40];
    um_now_utc_iso(ts, sizeof ts);
    snprintf(full, sizeof full,
             "{\"ts\":\"%s\",\"ev\":\"round\",\"n\":%d,\"external\":%d,"
             "\"interval_s\":%d,\"wake\":\"%s\",\"scan_ms\":%.1f}",
             ts, round_no, external, interval_s, wake, scan_ms);
    um_log_line(lg, full);
}

static void emit_meta(um_logger *lg, const char *what, const char *detail)
{
    char full[512];
    char ts[40];
    char e_detail[256];
    um_json_escape(detail ? detail : "", e_detail, sizeof e_detail);
    um_now_utc_iso(ts, sizeof ts);
    snprintf(full, sizeof full,
             "{\"ts\":\"%s\",\"ev\":\"%s\",\"detail\":\"%s\"}",
             ts, what, e_detail);
    um_log_line(lg, full);
}

static int list_devices(const char *sys_root)
{
    um_snapshot snap;
    int i;
    if (um_scan_platform(&snap, sys_root) != 0) {
        fprintf(stderr, "usbmon: device scan failed\n");
        return 1;
    }
    printf("%-8s %-9s %-24s %10s  %s\n", "KEY", "BUS", "MODEL", "SIZE", "MOUNT");
    for (i = 0; i < snap.count; i++) {
        const um_device *dev = &snap.devs[i];
        char size[32], key[UM_KEY_MAX], model[UM_MODEL_MAX], mount[UM_MOUNT_MAX];
        um_display_text(dev->key, key, sizeof key);
        um_display_text(dev->model, model, sizeof model);
        um_display_text(dev->mount, mount, sizeof mount);
        um_human_size(dev->size_bytes, size, sizeof size);
        printf("%-8s %-9s %-24s %10s  %s\n", key, dev->bus, model, size, mount[0] ? mount : "-");
    }
    if (!snap.count) printf("(no external storage devices)\n");
    return 0;
}

static um_snapshot previous;
static int have_previous;

static int do_round(um_logger *lg, um_hooks *hk, um_gui *gui, const char *sys_root,
                    const char *state_path, int interval, int number, const char *wake)
{
    um_snapshot current;
    int i;
    double start = um_monotonic(), scan_ms;
#ifdef _WIN32
    um_evidence evidence[UM_EVID_MAX];
    int n = um_scan_windows(&current, evidence, UM_EVID_MAX);
    int added = 0, removed = 0;
    (void)sys_root;
    if (n < 0) {
#else
    if (um_scan_platform(&current, sys_root) != 0) {
#endif
        emit_meta(lg, "error", "scan_failed");
        return -1;
    }

    scan_ms = (um_monotonic() - start) * 1000.0;
    if (have_previous) {
        for (i = 0; i < previous.count; i++) {
            const um_device *dev = &previous.devs[i];
            if (um_snapshot_match(&current, dev)) continue;
            emit_remove(lg, dev);
#ifdef _WIN32
            removed++;
#else
            um_gui_show_remove(gui, dev);
#endif
        }
    }
    for (i = 0; i < current.count; i++) {
        const um_device *dev = &current.devs[i];
        char hint[UM_MOUNT_MAX];
        if (have_previous && um_snapshot_match(&previous, dev)) continue;
        emit_add(lg, dev, !have_previous);
        path_hint_for(dev, hint, sizeof hint);
        um_hooks_dispatch(hk, dev, hint);
        if (have_previous) {
#ifdef _WIN32
            added++;
#else
            um_gui_show_add(gui, dev);
#endif
        }
    }
#ifdef _WIN32
    if (gui->enabled) um_gui_win_refresh(gui, evidence, n, added, removed);
#endif
    emit_round(lg, number, current.count, interval, scan_ms, wake);
    previous = current;
    have_previous = 1;
    if (um_state_save(state_path, &current) != 0)
        emit_meta(lg, "error", "state_save_failed");
    return 0;
}

/* ---------------------------------------------------------------- usage --- */

static void usage(FILE *out)
{
    fprintf(out,
"usbmon " UM_VERSION " — native USB storage monitor (no webpage; tray on Windows)\n"
"\n"
"Usage: usbmon [OPTIONS]\n"
"  (default)          run as daemon, one scan round per hour (1h 一轮);\n"
"                     plug/unplug events wake it instantly (toast pops up);\n"
"                     Windows adds a system-tray icon with left/right menus\n"
"  --interval N       round interval in seconds (min 1, default 3600)\n"
"  --once             run a single round and exit (cron / Task Scheduler)\n"
"  --list             list external storage devices and exit\n"
"  --log FILE         JSONL event log path (default: state dir/events.jsonl)\n"
"  --hooks FILE       hooks config JSON (default: config dir/hooks.json)\n"
"  --no-hooks         disable hooks entirely\n"
"  --log-raw          log serial numbers verbatim (default: fingerprinted)\n"
"  --gui              force popups on (default: auto in daemon mode when a\n"
"                     display exists; --once defaults to headless)\n"
"  --no-gui           disable popups (pure headless daemon)\n"
"  --toast-secs N     popup auto-dismiss seconds (default 12)\n"
"  --no-hotpath       ignore plug/unplug wakeups, strict interval rounds\n"
"  --baseline         first round reports all present devices as adds\n"
"                     (default: state file suppresses re-adds on restart)\n"
"  --install-startup  register login autostart and exit (v1.1.1 parity:\n"
"                     HKCU Run on Windows, XDG autostart on Linux)\n"
"  --uninstall-startup remove login autostart and exit\n"
"  --startup-status   print whether login autostart is installed, exit\n"
"  --sys-root PATH    sysfs root override (Linux, for tests)\n"
"  --verbose          mirror event lines to stderr in human form\n"
"  --version          print version\n"
"  --help             this help\n"
"\n"
"Events are JSONL: add / remove / round / start / stop.\n"
"Hooks run as argv arrays, never through a shell (see README).\n");
}

/* ---------------------------------------------------------------- main ---- */

static int run(int argc, char **argv)
{
    int i;
    int interval_s = 3600;          /* 1h 一轮 (guaranteed max cadence) */
    int once = 0, list_only = 0, verbose = 0, raw = 0;
    const char *log_path = NULL, *hooks_path = NULL, *sys_root = NULL;
    int no_hooks = 0;
    int no_hotpath = 0, force_baseline = 0;

    um_logger lg;
    static um_hooks hk;          /* ~1.5 MB: keep off the stack */
    static um_gui gui;           /* GUI state; helper pids tracked here */
    char statebuf[512], confbuf[512], lockbuf[600], statefile[600];
    char defaultlog[600];
    int round_no = 0, round_result, hot_available = 0;

    gui.requested = -1;          /* auto */
    gui.toast_ttl = UM_GUI_TOAST_TTL_S;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            win_cli_console_attach();
            usage(stdout); return 0;
        }
        else if (!strcmp(a, "--version")) {
            win_cli_console_attach();
            printf("usbmon " UM_VERSION "\n"); return 0;
        }
        else if (!strcmp(a, "--once")) once = 1;
        else if (!strcmp(a, "--list")) list_only = 1;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "--log-raw")) raw = 1;
        else if (!strcmp(a, "--no-hooks")) no_hooks = 1;
        else if (!strcmp(a, "--gui")) gui.requested = 1;
        else if (!strcmp(a, "--no-gui")) gui.requested = 0;
        else if (!strcmp(a, "--no-hotpath")) no_hotpath = 1;
        else if (!strcmp(a, "--baseline")) force_baseline = 1;
        else if (!strcmp(a, "--install-startup")) {
            win_cli_console_attach();
            int r = um_startup_install();
            printf(r == 0 ? "usbmon: 已安装登录自启动（%s）。\n"
                          : "usbmon: 安装自启动失败。\n",
                   um_startup_where());
            return r == 0 ? 0 : 1;
        }
        else if (!strcmp(a, "--uninstall-startup")) {
            win_cli_console_attach();
            int r = um_startup_uninstall();
            printf(r == 0 ? "usbmon: 已移除登录自启动。\n"
                          : "usbmon: 移除自启动失败。\n");
            return r == 0 ? 0 : 1;
        }
        else if (!strcmp(a, "--startup-status")) {
            win_cli_console_attach();
            printf("usbmon 登录自启动：%s（%s）\n",
                   um_startup_enabled() ? "已启用" : "未启用",
                   um_startup_where());
            return 0;
        }
        else if (!strcmp(a, "--toast-secs") && i + 1 < argc) {
            char *endp = NULL;
            long v = strtol(argv[++i], &endp, 10);
            if (endp && *endp == '\0' && v >= 1 && v <= 600) gui.toast_ttl = (int)v;
        }
        else if (!strcmp(a, "--interval") && i + 1 < argc) {
            char *endp = NULL;
            long v = strtol(argv[++i], &endp, 10);
            if (endp && *endp == '\0' && v >= 1 && v <= 86400 * 7) interval_s = (int)v;
            else if (v > 86400 * 7) interval_s = 86400 * 7;
            else interval_s = 1;
        }
        else if (!strcmp(a, "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(a, "--hooks") && i + 1 < argc) hooks_path = argv[++i];
        else if (!strcmp(a, "--sys-root") && i + 1 < argc) sys_root = argv[++i];
        else {
            win_cli_console_attach();
            fprintf(stderr, "usbmon: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    if (list_only) {
        win_cli_console_attach();
        return list_devices(sys_root);
    }

    um_state_dir(statebuf, sizeof statebuf);
#ifdef _WIN32
#define STATE_SEP "\\"
#else
#define STATE_SEP "/"
#endif
    snprintf(statefile, sizeof statefile, "%s" STATE_SEP "last-snapshot.txt", statebuf);
    snprintf(lockbuf, sizeof lockbuf, "%s" STATE_SEP "usbmon.lock", statebuf);
    if (!log_path) {
        snprintf(defaultlog, sizeof defaultlog, "%s" STATE_SEP "events.jsonl", statebuf);
        log_path = defaultlog;
    }
    {
        int locked = um_single_instance_acquire(lockbuf);
        if (locked != 0) {
            win_cli_console_attach();
            if (locked > 0) {
                fprintf(stderr, "usbmon: another instance is already running\n");
                return 3;
            }
            fprintf(stderr, "usbmon: cannot acquire state lock %s\n", lockbuf);
            return 1;
        }
    }
    if (!hooks_path) {
        um_config_dir(confbuf, sizeof confbuf);
        static char defaulthooks[600];
#ifdef _WIN32
        snprintf(defaulthooks, sizeof defaulthooks, "%s\\hooks.json", confbuf);
#else
        snprintf(defaulthooks, sizeof defaulthooks, "%s/hooks.json", confbuf);
#endif
        hooks_path = defaulthooks;
    }

    if (um_log_open(&lg, log_path, raw, verbose) != 0) {
        win_cli_console_attach();
        fprintf(stderr, "usbmon: cannot open log %s: %s\n", log_path, strerror(errno));
        um_single_instance_release();
        return 1;
    }

    /* hooks */
    if (no_hooks) {
        hk.hooks_disabled = 1;
        hk.count = 0;
        hk.reap_n = 0;
        hk.fired_n = 0;
    } else if (um_hooks_load(&hk, hooks_path) != 0) {
        /* parse error already reported; run with hooks off */
        hk.hooks_disabled = 1;
        hk.count = 0;
    }

    if (gui.requested == -1) gui.requested = !once;
    gui.raw_serial = raw;
#ifdef _WIN32
    gui.watch_devices = !once && !no_hotpath;
#endif
    um_gui_init(&gui);
#ifndef _WIN32
    hot_available = !once && !no_hotpath && um_hot_init(sys_root) >= 0;
#else
    hot_available = gui.watch_devices && gui.wake_event != NULL;
#endif
    if (verbose && !once && !no_hotpath && !hot_available)
        fprintf(stderr, "usbmon: device notifications unavailable; using interval scans\n");
    if (verbose && gui.requested && !gui.enabled)
        fprintf(stderr, "usbmon: GUI unavailable; continuing headless\n");

    install_signals();
    {
        char detail[256];
        snprintf(detail, sizeof detail, "usbmon %s interval=%ds hooks=%d gui=%d hotpath=%d",
                 UM_VERSION, interval_s, hk.count, gui.enabled, hot_available);
        emit_meta(&lg, "start", detail);
    }
    if (!force_baseline) {
        int loaded = um_state_load(statefile, &previous);
        have_previous = loaded > 0;
        if (loaded < 0) emit_meta(&lg, "error", "state_load_failed");
    }
    round_result = do_round(&lg, &hk, &gui, sys_root, statefile, interval_s, ++round_no, "start");

    if (!once) {
        double last_round = um_monotonic();
        double hot_deadline = 0;
        while (!stop_requested()) {
            int woke = 0;
            double now;
            const char *wake = NULL;

            if (g_reload_hooks) {
                g_reload_hooks = 0;
                if (!no_hooks) {
                    um_hooks_shutdown(&hk, 1000);
                    memset(&hk, 0, sizeof hk);
                    if (um_hooks_load(&hk, hooks_path) == 0)
                        emit_meta(&lg, "info", "hooks-reloaded");
                }
            }
            um_hooks_reap(&hk);
            um_gui_reap(&gui);
#ifndef _WIN32
            woke = um_hot_wait(UM_SLEEP_TICK_MS);
#else
            if (gui.wake_event)
                woke = WaitForSingleObject((HANDLE)gui.wake_event, UM_SLEEP_TICK_MS) == WAIT_OBJECT_0;
            else
                Sleep(UM_SLEEP_TICK_MS);
#endif
            if (stop_requested()) break;
            now = um_monotonic();
            if (woke && hot_deadline == 0) hot_deadline = now + UM_HOT_SETTLE_MS / 1000.0;
            if (hot_deadline > 0 && now >= hot_deadline) {
                hot_deadline = 0;
                wake = "hot";
            } else if (now - last_round >= interval_s) {
                hot_deadline = 0;
                wake = "tick";
            }
            if (wake) {
                do_round(&lg, &hk, &gui, sys_root, statefile, interval_s, ++round_no, wake);
                last_round = now;
            }
        }
    }

    emit_meta(&lg, "stop", once ? (round_result == 0 ? "once-complete" : "scan-failed")
                                : stop_reason());
    um_hooks_shutdown(&hk, once ? (int)(UM_HOOK_TIMEOUT_S * 1000) : 10000);
    um_gui_shutdown(&gui);
#ifndef _WIN32
    um_hot_close();
#endif
    um_log_close(&lg);
    um_single_instance_release();
    return once && round_result != 0 ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide)
{
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    int i, result = 1;
    if (!argv) return 1;
    for (i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(argv[i] = malloc((size_t)n))) break;
        um_wide_to_utf8(wide[i], argv[i], (size_t)n);
    }
    if (i == argc) result = run(argc, argv);
    while (i > 0) free(argv[--i]);
    free(argv);
    return result;
}
#else
int main(int argc, char **argv)
{
    um_set_executable(argv[0]);
    return run(argc, argv);
}
#endif
