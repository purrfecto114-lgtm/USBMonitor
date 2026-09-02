/* main.c — CLI, snapshot diff, round loop, GUI + hot-path wakeups.
 *
 *   usbmon                  daemon, one scan round per hour (default);
 *                           plug/unplug wake it instantly (inotify /
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
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- signals */

#ifndef _WIN32
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload_hooks = 0;
#else
static volatile int g_stop = 0;          /* set by the console ctrl handler */
static volatile int g_reload_hooks = 0;  /* Windows has no SIGHUP: never set */
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
#else
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT ||
        type == CTRL_LOGOFF_EVENT) g_stop = 1;
    return TRUE;
}
static void install_signals(void) { SetConsoleCtrlHandler(on_ctrl, TRUE); }
static void sleep_ms(int ms) { Sleep(ms); }   /* POSIX sleeps in um_hot_wait */
#endif

/* ---------------------------------------------------------------- helpers */

int um_snapshot_find(const um_snapshot *s, const char *key)
{
    int i;
    for (i = 0; i < s->count; i++)
        if (strcmp(s->devs[i].key, key) == 0) return i;
    return -1;
}

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
    char e_model[UM_MODEL_MAX * 8], e_key[UM_KEY_MAX * 4], e_mount[UM_MOUNT_MAX * 4];
    char e_serial[128];
    char parts[UM_MAX_PARTITIONS * (UM_NAME_MAX * 4 + 4)];
    char e_bus[32];
    int i;

    um_json_escape(d->model, e_model, sizeof e_model);
    um_json_escape(d->key, e_key, sizeof e_key);
    um_json_escape(d->mount, e_mount, sizeof e_mount);
    um_json_escape(d->bus, e_bus, sizeof e_bus);
    um_json_escape(lg->raw && d->serial[0] ? d->serial : d->serial_fp,
                   e_serial, sizeof e_serial);

    parts[0] = '\0';
    for (i = 0; i < d->partition_count; i++) {
        char ep[UM_NAME_MAX * 4 + 8];
        um_json_escape(d->partitions[i], ep + 1, sizeof ep - 8);
        ep[0] = '"';
        strcat(ep, "\"");
        if (i) strcat(parts, ",");
        strcat(parts, ep);
    }

    {
        char ts[40];
        char full[6144];
        um_now_utc_iso(ts, sizeof ts);
        if (baseline)
            snprintf(full, sizeof full,
                "{\"ts\":\"%s\",\"ev\":\"add\",\"baseline\":1,\"key\":\"%s\",\"bus\":\"%s\",\"model\":\"%s\","
                "\"serial\":\"%s\",\"size_bytes\":%llu,\"partitions\":[%s],"
                "\"mount\":\"%s\",\"fs_total_bytes\":%llu,\"fs_free_bytes\":%llu}",
                ts, e_key, e_bus, e_model, e_serial,
                d->size_bytes, parts, e_mount, d->fs_total, d->fs_free);
        else
            snprintf(full, sizeof full,
                "{\"ts\":\"%s\",\"ev\":\"add\",\"key\":\"%s\",\"bus\":\"%s\",\"model\":\"%s\","
                "\"serial\":\"%s\",\"size_bytes\":%llu,\"partitions\":[%s],"
                "\"mount\":\"%s\",\"fs_total_bytes\":%llu,\"fs_free_bytes\":%llu}",
                ts, e_key, e_bus, e_model, e_serial,
                d->size_bytes, parts, e_mount, d->fs_total, d->fs_free);
        um_log_line(lg, full);
    }
}

static void emit_remove(um_logger *lg, const um_device *d)
{
    char e_key[UM_KEY_MAX * 4];
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

/* --------------------------------------------------------------- state --- */

/* Persist the last seen device keys so `--once` runs (cron mode) can diff
 * across invocations.  Only keys are stored: that is all the diff needs.
 * File format: one key per line, plain text, atomically replaced. */
static void state_path(char *buf, size_t n)
{
    char dir[512];
    um_state_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(buf, n, "%s\\last-snapshot.txt", dir);
#else
    snprintf(buf, n, "%s/last-snapshot.txt", dir);
#endif
}

static void state_save(const um_snapshot *s)
{
    char path[600], tmp[640];
    FILE *f;
    int i;
    state_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return;
    for (i = 0; i < s->count; i++) fprintf(f, "%s\n", s->devs[i].key);
    fclose(f);
    (void)rename(tmp, path);
}

static int state_load(um_snapshot *s)
{
    char path[600];
    FILE *f;
    char line[256];
    state_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) return 0;
    s->count = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (!l || s->count >= UM_MAX_DEV) continue;
        memset(&s->devs[s->count], 0, sizeof s->devs[0]);
        um_copy_str(s->devs[s->count].key, sizeof s->devs[s->count].key, line);
        s->count++;
    }
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- rounds -- */

static um_snapshot g_prev;
static int g_have_prev = 0;

static int do_round(um_logger *lg, um_hooks *hk, um_gui *gui, const char *sys_root,
                    int interval_s, int round_no, int is_list, const char *wake)
{
    um_snapshot cur;
    int i;
    double t0 = um_monotonic();
    int added = 0, removed = 0;

    memset(&cur, 0, sizeof cur);
    if (um_scan_platform(&cur, sys_root) != 0) {
        emit_meta(lg, "error", "scan_failed");
        return -1;
    }

    if (g_have_prev) {
        for (i = 0; i < cur.count; i++) {
            if (um_snapshot_find(&g_prev, cur.devs[i].key) < 0) {
                emit_add(lg, &cur.devs[i], 0);
                added++;
                if (!is_list) {   /* --list is read-only: never fire user hooks */
                    char hint[UM_MOUNT_MAX];
                    path_hint_for(&cur.devs[i], hint, sizeof hint);
                    um_hooks_dispatch(hk, &cur.devs[i], hint);
                    um_gui_show_add(gui, &cur.devs[i]);
                }
            }
        }
        for (i = 0; i < g_prev.count; i++) {
            if (um_snapshot_find(&cur, g_prev.devs[i].key) < 0) {
                emit_remove(lg, &g_prev.devs[i]);
                removed++;
                if (!is_list)
                    um_gui_show_remove(gui, &g_prev.devs[i]);
            }
        }
    } else {
        /* first round: baseline — every present device is an "add" event
         * (auditors want to know what was already plugged in at start).
         * Baseline adds are flagged and do NOT pop up a toast: a desktop
         * session start should not be greeted by a pile of windows.   */
        for (i = 0; i < cur.count; i++) {
            emit_add(lg, &cur.devs[i], 1);
            added++;
            if (!is_list) {   /* --list is read-only: never fire user hooks */
                char hint[UM_MOUNT_MAX];
                path_hint_for(&cur.devs[i], hint, sizeof hint);
                um_hooks_dispatch(hk, &cur.devs[i], hint);
            }
        }
    }

    {
        double ms = (um_monotonic() - t0) * 1000.0;
        emit_round(lg, round_no, cur.count, interval_s, ms, wake);
        if (is_list) {
            printf("%-8s %-9s %-24s %10s  %s\n", "KEY", "BUS", "MODEL", "SIZE", "MOUNT");
            for (i = 0; i < cur.count; i++) {
                char sz[32];
                um_human_size(cur.devs[i].size_bytes, sz, sizeof sz);
                printf("%-8s %-9s %-24s %10s  %s\n",
                       cur.devs[i].key, cur.devs[i].bus, cur.devs[i].model, sz,
                       cur.devs[i].mount[0] ? cur.devs[i].mount : "-");
            }
            if (cur.count == 0)
                printf("(no external storage devices)\n");
        }
    }
    (void)removed;

    g_prev = cur;
    g_have_prev = 1;
    return added;
}

/* ---------------------------------------------------------------- usage --- */

static void usage(FILE *out)
{
    fprintf(out,
"usbmon " UM_VERSION " — native USB storage monitor (no webpage, no tray)\n"
"\n"
"Usage: usbmon [OPTIONS]\n"
"  (default)          run as daemon, one scan round per hour (1h 一轮);\n"
"                     plug/unplug events wake it instantly (toast pops up)\n"
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
"  --sys-root PATH    sysfs root override (Linux, for tests)\n"
"  --verbose          mirror event lines to stderr in human form\n"
"  --version          print version\n"
"  --help             this help\n"
"\n"
"Events are JSONL: add / remove / round / start / stop.\n"
"Hooks run as argv arrays, never through a shell (see README).\n");
}

/* ---------------------------------------------------------------- main ---- */

int main(int argc, char **argv)
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
    char statebuf[512], confbuf[512], lockbuf[600];
    char defaultlog[600];
    int round_no = 0;

    gui.requested = -1;          /* auto */
    gui.toast_ttl = UM_GUI_TOAST_TTL_S;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--version")) { printf("usbmon " UM_VERSION "\n"); return 0; }
        else if (!strcmp(a, "--once")) once = 1;
        else if (!strcmp(a, "--list")) list_only = 1;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "--log-raw")) raw = 1;
        else if (!strcmp(a, "--no-hooks")) no_hooks = 1;
        else if (!strcmp(a, "--gui")) gui.requested = 1;
        else if (!strcmp(a, "--no-gui")) gui.requested = 0;
        else if (!strcmp(a, "--no-hotpath")) no_hotpath = 1;
        else if (!strcmp(a, "--baseline")) force_baseline = 1;
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
            fprintf(stderr, "usbmon: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    /* default paths */
    if (!log_path) {
        um_state_dir(statebuf, sizeof statebuf);
#ifdef _WIN32
        if (list_only) snprintf(defaultlog, sizeof defaultlog, "NUL");
        else snprintf(defaultlog, sizeof defaultlog, "%s\\events.jsonl", statebuf);
#else
        if (list_only) snprintf(defaultlog, sizeof defaultlog, "/dev/null");
        else snprintf(defaultlog, sizeof defaultlog, "%s/events.jsonl", statebuf);
#endif
        log_path = defaultlog;
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
        fprintf(stderr, "usbmon: cannot open log %s: %s\n", log_path, strerror(errno));
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

    /* GUI: auto-on for the daemon when a display exists, off for --once
     * (cron context) and always off for --list (read-only). */
    if (gui.requested == -1)
        gui.requested = (!once && !list_only) ? 1 : 0;
    if (list_only) gui.requested = 0;
    gui.raw_serial = raw;
    um_gui_init(&gui);
    if (verbose) {
        if (gui.requested == 1 && !gui.enabled)
            fprintf(stderr, "usbmon: GUI unavailable (no DISPLAY or no "
                            "usbmon-toast helper next to the daemon) — "
                            "headless mode\n");
        else if (gui.enabled)
            fprintf(stderr, "usbmon: GUI on — toast pops up on insert/remove\n");
    }

    if (list_only) {
        (void)do_round(&lg, &hk, &gui, sys_root, interval_s, 0, 1, "list");
        um_log_close(&lg);
        return 0;
    }

    /* single instance only for daemon mode (--once may run beside it) */
    if (!once) {
        um_state_dir(statebuf, sizeof statebuf);
        snprintf(lockbuf, sizeof lockbuf,
#ifdef _WIN32
                 "%s\\usbmon.lock", statebuf);
#else
                 "%s/usbmon.lock", statebuf);
#endif
        if (um_single_instance_acquire(lockbuf) != 0) {
            fprintf(stderr, "usbmon: another instance is already running\n");
            emit_meta(&lg, "error", "already_running");
            um_log_close(&lg);
            return 3;
        }
    }

    install_signals();
    {
        char detail[256];
        snprintf(detail, sizeof detail, "usbmon %s interval=%ds hooks=%d gui=%d hotpath=%d",
                 UM_VERSION, interval_s, no_hooks ? 0 : hk.count,
                 gui.enabled, !no_hotpath);
        emit_meta(&lg, "start", detail);
    }

    /* hot path: watch <sys_root>/block so a plug wakes us within ms
     * (POSIX).  Windows gets the same effect from WM_DEVICECHANGE on
     * the GUI thread.  Failure is silent: pure interval mode remains. */
#ifndef _WIN32
    if (!once && !no_hotpath) {
        if (um_hot_init(sys_root) < 0 && verbose)
            fprintf(stderr, "usbmon: hot path unavailable (inotify watch on "
                            "%s/block failed) — interval-only mode\n",
                            sys_root && *sys_root ? sys_root : "/sys");
    }
#else
    (void)sys_root;
#endif

    /* Diff against the persisted state unless a baseline is requested:
     * a daemon restart (or a --once cron run before it) must not re-emit
     * add events / re-fire hooks for devices that never moved.  */
    if (!force_baseline && state_load(&g_prev)) g_have_prev = 1;

    do_round(&lg, &hk, &gui, sys_root, interval_s, ++round_no, 0, "start");
    if (!once) state_save(&g_prev);

    if (once) {
        state_save(&g_prev);
        emit_meta(&lg, "stop", "once-complete");
        /* --once: let freshly spawned hooks finish (full timeout budget) */
        um_hooks_shutdown(&hk, (int)(UM_HOOK_TIMEOUT_S * 1000));
        um_log_close(&lg);
        return 0;
    }

    {
        double last_round = 0;      /* monotonic ts of the last round */
        int    hot_pending = 0;
        double hot_deadline = 0;

        while (!g_stop) {
            int woke = 0;

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

            /* block on the inotify fd (or plain sleep) until next tick */
#ifndef _WIN32
            woke = um_hot_wait(UM_SLEEP_TICK_MS);
#else
            if (gui.enabled && gui.wake_event) {
                DWORD wr = WaitForSingleObject((HANDLE)gui.wake_event,
                                               UM_SLEEP_TICK_MS);
                if (wr == WAIT_OBJECT_0) woke = 1;
            } else {
                sleep_ms(UM_SLEEP_TICK_MS);
            }
#endif
            if (woke && !no_hotpath) {
                if (!hot_pending) {
                    hot_pending = 1;
                    hot_deadline = um_monotonic() +
                                   (double)UM_HOT_SETTLE_MS / 1000.0;
                }
            }

            {
                double now = um_monotonic();
                if (last_round == 0) last_round = now;
                if (hot_pending && now >= hot_deadline) {
                    hot_pending = 0;
                    last_round = now;
                    do_round(&lg, &hk, &gui, sys_root, interval_s,
                             ++round_no, 0, "hot");
                    state_save(&g_prev);
                } else if (now - last_round >= (double)interval_s) {
                    last_round = now;
                    do_round(&lg, &hk, &gui, sys_root, interval_s,
                             ++round_no, 0, "tick");
                    state_save(&g_prev);
                }
            }
        }
    }

    um_hooks_reap(&hk);
    emit_meta(&lg, "stop", "signal");
    um_hooks_shutdown(&hk, 10000);   /* responsive exit, then kill stragglers */
    um_gui_shutdown(&gui);           /* toasts: brief grace, then kill */
#ifndef _WIN32
    um_hot_close();
#endif
    um_log_close(&lg);
    um_single_instance_release();
    return 0;
}
