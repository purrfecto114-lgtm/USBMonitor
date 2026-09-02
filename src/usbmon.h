/* usbmon — native USB storage monitor daemon (C99)
 *
 * A lightweight, cross-platform replacement for the Python/PySide6
 * "USBMonitor" tool.  Design goals:
 *   - zero runtime dependencies for the daemon itself (no interpreter,
 *     no GUI toolkit, no X11 linkage — the optional popup GUI lives in
 *     a tiny helper binary, usbmon-toast, spawned only when a display
 *     is available)
 *   - one scan round per hour by default ("1h 一轮") — the guaranteed
 *     cadence; plug/unplug wake the daemon instantly (inotify on Linux,
 *     WM_DEVICECHANGE on Windows), so popups appear in real time
 *   - JSONL structured event log, serial/label redaction by default
 *   - opt-in hooks executed as argv arrays (never through a shell),
 *     with the same BatBadBut/CmdHijack guards as the Python original
 *   - no tray, no webpage, no background service beyond one process
 */
#ifndef USBMON_H
#define USBMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#define UM_VERSION "2.0.1"

/* ---- limits (defensive caps, same spirit as the original normalizers) ---- */
#define UM_MAX_DEV          64      /* devices per snapshot            */
#define UM_MAX_PARTITIONS   16      /* partitions reported per device  */
#define UM_MAX_HOOKS        20      /* hooks rules                     */
#define UM_HOOK_MAX_ARGS    32      /* argv tokens per hook command    */
#define UM_HOOK_ARG_MAX     2048    /* bytes per argv token            */
#define UM_HOOK_PATTERN_MAX 260
#define UM_HOOK_MAX_PATTERNS 20
#define UM_NAME_MAX         64
#define UM_KEY_MAX          64
#define UM_MODEL_MAX        64
#define UM_SERIAL_MAX       64
#define UM_MOUNT_MAX        128
#define UM_REAP_MAX         16      /* concurrent hook children        */
#define UM_HOOK_TIMEOUT_S   60.0
#define UM_LOG_ROTATE_BYTES (1024 * 1024)
#define UM_LOG_KEEP         3
#define UM_SLEEP_TICK_MS    200
#define UM_HOT_SETTLE_MS    700     /* debounce between event wake and round */
#define UM_GUI_TOAST_TTL_S  12      /* popup auto-dismiss                      */
#define UM_GUI_MAX_TOASTS   8       /* tracked helper children (reap table)    */
#define UM_GUI_SLOTS        4       /* on-screen stacking slots (bottom-right) */

/* ---------------------------------------------------------------- devices */

typedef struct {
    char key[UM_KEY_MAX];       /* stable id: "sdb" (linux) / "disk3" (win)  */
    char display[UM_NAME_MAX * 2];
    char model[UM_MODEL_MAX];
    char serial[UM_SERIAL_MAX];     /* raw serial (may be empty)            */
    char serial_fp[24];             /* "sha256:xxxxxxxxxxxx" redacted form  */
    char bus[16];                   /* "usb" / "removable" / "sd" / "mmc"   */
    int  removable;
    unsigned long long size_bytes;  /* device capacity                      */
    int  partition_count;
    char partitions[UM_MAX_PARTITIONS][UM_NAME_MAX];
    char mount[UM_MOUNT_MAX];       /* first mount point ("" if none)       */
    unsigned long long fs_total, fs_free; /* statvfs values when mounted     */
    char extra[UM_MOUNT_MAX];       /* win: "E: F:" letters; linux: ""      */
} um_device;

typedef struct {
    int count;
    um_device devs[UM_MAX_DEV];
} um_snapshot;

/* Platform scanner.  Returns 0 on success (even with zero devices). */
int um_scan_platform(um_snapshot *snap, const char *sys_root);

/* ------------------------------------------------------------- util.c ---- */
void um_now_utc_iso(char *buf, size_t n);
double um_monotonic(void);
int   um_glob_match(const char *pattern, const char *text, int fold_case);
void  um_fingerprint(const char *value, char *out, size_t out_n);
int   um_mkdir_p(const char *path);
int   um_read_file_str(const char *path, char *buf, size_t n);
void  um_copy_str(char *dst, size_t n, const char *src);
void  um_human_size(unsigned long long bytes, char *out, size_t n);
const char *um_state_dir(char *buf, size_t n);
const char *um_config_dir(char *buf, size_t n);
#ifdef _WIN32
void um_utf8_to_wide(const char *in, wchar_t *out, size_t out_n);
void um_wide_to_utf8(const wchar_t *in, char *out, size_t out_n);
#endif

/* ----------------------------------------------------------- logjson.c --- */
typedef struct {
    FILE *fp;
    char  path[512];
    int   raw;        /* 1 = log serial/label verbatim                    */
    int   verbose;    /* 1 = mirror human-readable lines to stderr        */
} um_logger;

int  um_log_open(um_logger *lg, const char *path, int raw, int verbose);
void um_log_close(um_logger *lg);
/* Rotation happens inside. `line` is one complete JSON object, no '\n'. */
void um_log_line(um_logger *lg, const char *line);
void um_json_escape(const char *in, char *out, size_t out_n);

/* --------------------------------------------------------------- json.c -- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    char *str;                     /* J_STR */
    double num;                    /* J_NUM */
    int    b;                      /* J_BOOL */
    struct jval **items; int n;    /* J_ARR  */
    char **keys; struct jval **vals; int nk; /* J_OBJ */
} jval;

jval *json_parse(const char *text);   /* NULL on error */
void  json_free(jval *v);
const jval *json_get(const jval *obj, const char *key);
const char *json_str(const jval *v, const char *fallback);

/* ---------------------------------------------------------------- hook.c -- */
typedef struct {
    char name[UM_NAME_MAX];
    char match_keys[UM_HOOK_MAX_PATTERNS][UM_HOOK_PATTERN_MAX];
    int  n_keys;
    char match_models[UM_HOOK_MAX_PATTERNS][UM_HOOK_PATTERN_MAX];
    int  n_models;
    char command[UM_HOOK_ARG_MAX];          /* argv[0]                       */
    char argv[UM_HOOK_MAX_ARGS - 1][UM_HOOK_ARG_MAX];
    int  argc;                              /* total incl. command           */
    double debounce_s;
    int  enabled;
} um_hook;

typedef struct {
    int       count;
    um_hook   hooks[UM_MAX_HOOKS];
    int       hooks_disabled;               /* --no-hooks                    */
    char      path[512];
    /* debounce + reaper runtime state */
    struct { char key[UM_KEY_MAX + UM_NAME_MAX]; double at; } fired[256];
    int fired_n;
    struct {
#ifdef _WIN32
        void *h;                    /* child HANDLE (safer than PID) */
#else
        int pid;
#endif
        double deadline; char hook[UM_NAME_MAX];
    } reap[UM_REAP_MAX];
    int reap_n;
} um_hooks;

int  um_hooks_load(um_hooks *hk, const char *path);
void um_hooks_dispatch(um_hooks *hk, const um_device *dev, const char *path_hint);
void um_hooks_reap(um_hooks *hk);
/* max_wait_ms: grace period before killing stragglers on shutdown. */
void um_hooks_shutdown(um_hooks *hk, int max_wait_ms);

/* ---------------------------------------------------------------- lock.c -- */
int  um_single_instance_acquire(const char *lock_path);
void um_single_instance_release(void);

/* ------------------------------------------------------------- hotpath.c -- */
/* Event-driven wakeup: watch <sys_root>/block for create/delete so a plug
 * triggers a round immediately instead of waiting for the hourly tick.
 * POSIX/inotify implementation; Windows uses WM_DEVICECHANGE in gui_win32.c.
 * Returns an fd (-1 = unavailable).  um_hot_wait blocks up to timeout_ms
 * and returns 1 when a device change was observed (events drained).      */
#ifndef _WIN32
int  um_hot_init(const char *sys_root);
void um_hot_close(void);
int  um_hot_wait(int timeout_ms);
#endif

/* ---------------------------------------------------------------- gui.c ---- */
/* Popup GUI: when a USB device is inserted/removed a small toast window
 * appears (bottom-right).  POSIX renders via the usbmon-toast helper
 * (fork+exec, argv array — never a shell); Windows renders in-process
 * (gui_win32.c GUI thread).  The daemon itself never links X11.        */
typedef struct {
    int  requested;      /* CLI: -1 auto, 0 off, 1 on                          */
    int  enabled;        /* runtime: GUI actually active                       */
    int  raw_serial;     /* mirror --log-raw for display                       */
    int  toast_ttl;      /* auto-dismiss seconds                               */
    int  slot_seq;       /* stacking slot counter                              */
    int  warned;         /* emitted "unavailable" notice once                  */
#ifndef _WIN32
    int  toasts[UM_GUI_MAX_TOASTS];   /* helper pids, 0 = empty             */
#else
    void *gui_thread;    /* HANDLE                                            */
    void *wake_event;    /* HANDLE set by WM_DEVICECHANGE                     */
    unsigned long gui_tid; /* PostThreadMessage target                        */
#endif
} um_gui;

int  um_gui_init(um_gui *g);            /* resolves helper/display; sets enabled */
void um_gui_show_add(um_gui *g, const um_device *dev);
void um_gui_show_remove(um_gui *g, const um_device *dev);
void um_gui_reap(um_gui *g);            /* POSIX: reap finished helper children  */
void um_gui_shutdown(um_gui *g);        /* brief wait, then forget helpers       */

/* -------------------------------------------------------------- main.c ---- */
/* implemented in main.c: snapshot diff + round loop */
int  um_snapshot_find(const um_snapshot *s, const char *key);

#endif /* USBMON_H */
