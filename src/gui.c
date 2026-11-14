/* gui.c — Linux popup helper process management.
 *
 * POSIX: the daemon itself NEVER links X11.  Toasts are rendered by a
 * helper binary, usbmon-toast, discovered next to the daemon executable
 * (or on PATH).  Each popup is a fork()+execv of the helper with a pure
 * argv array — same no-shell discipline as hooks — so a GUI crash, a
 * missing display or a missing X11 library can never affect monitoring.
 *
 * Children are tracked in a small table and reaped from the main loop
 * (um_gui_reap), so no zombies accumulate and no global SIGCHLD games
 * are needed (hook.c's per-pid waitpid is unaffected).
 */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char g_helper[512];    /* resolved path to usbmon-toast */

static int find_helper(void)
{
    const char *exe = um_executable();
    const char *slash = strrchr(exe, '/');
    char candidate[512];
    if (slash) {
        int n = snprintf(candidate, sizeof candidate, "%.*s/usbmon-toast",
                         (int)(slash - exe), exe);
        if (n > 0 && (size_t)n < sizeof candidate &&
            um_find_executable(candidate, g_helper, sizeof g_helper)) return 1;
    }
    return um_find_executable("usbmon-toast", g_helper, sizeof g_helper);
}

int um_gui_init(um_gui *g)
{
    g->enabled = 0;
    if (g->requested != 1) return 0;

    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0])
        return 0;                       /* headless: no X session */

    if (!find_helper())
        return 0;                       /* helper not installed: GUI off */

    memset(g->toasts, 0, sizeof g->toasts);
    g->enabled = 1;
    return 1;
}

/* Build the argv array for the helper.  Everything is a fixed token from
 * device fields — no shell ever sees this, so no quoting concerns. */
static void spawn_toast(um_gui *g, const um_device *dev, int is_add)
{
    char ttl[16], slot[16], size[32], model[UM_MODEL_MAX], mount[UM_MOUNT_MAX];
    char parts[UM_MAX_PARTITIONS * (UM_NAME_MAX + 2)];
    char *argv[20];
    int argc = 0, i, free_slot = -1;
    pid_t pid;
    int devnull;

    for (i = 0; i < UM_GUI_MAX_TOASTS; i++)
        if (g->toasts[i] == 0) { free_slot = i; break; }
    if (free_slot < 0)
        return;                          /* queue full: silently skip a toast */

    snprintf(ttl,  sizeof ttl,  "%d",  g->toast_ttl);
    snprintf(slot, sizeof slot, "%d", free_slot);
    snprintf(size, sizeof size, "%llu", dev->size_bytes);

    parts[0] = '\0';
    for (i = 0; i < dev->partition_count && i < UM_MAX_PARTITIONS; i++) {
        if (i) strncat(parts, ",", sizeof parts - strlen(parts) - 1);
        strncat(parts, dev->partitions[i], sizeof parts - strlen(parts) - 1);
    }

    argv[argc++] = g_helper;
    argv[argc++] = is_add ? "--add" : "--remove";
    argv[argc++] = (char *)"--ttl";    argv[argc++] = ttl;
    argv[argc++] = (char *)"--slot";   argv[argc++] = slot;
    if (dev->key[0]) {
        argv[argc++] = (char *)"--key";   argv[argc++] = (char *)dev->key;
    }
    um_display_text(dev->model, model, sizeof model);
    if (model[0]) {
        argv[argc++] = (char *)"--model"; argv[argc++] = model;
    }
    if (is_add && dev->size_bytes) {
        argv[argc++] = (char *)"--size";  argv[argc++] = size;
    }
    um_display_text(dev->mount, mount, sizeof mount);
    if (mount[0]) {
        argv[argc++] = (char *)"--mount"; argv[argc++] = mount;
    }
    {
        const char *serial = (g->raw_serial && dev->serial[0])
                             ? dev->serial : dev->serial_fp;
        if (serial[0]) {
            argv[argc++] = (char *)"--serial";
            argv[argc++] = (char *)serial;
        }
    }
    if (parts[0]) {
        argv[argc++] = (char *)"--parts"; argv[argc++] = parts;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* child: silence stdio, become its own process group (so a
         * Ctrl-C on the daemon terminal does not yank the toast away),
         * then exec the helper. */
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        setpgid(0, 0);
        execv(g_helper, argv);
        _exit(127);
    }
    g->toasts[free_slot] = (int)pid;
}

void um_gui_show_add(um_gui *g, const um_device *dev)
{
    if (!g->enabled || !dev) return;
    spawn_toast(g, dev, 1);
}

void um_gui_show_remove(um_gui *g, const um_device *dev)
{
    if (!g->enabled || !dev) return;
    spawn_toast(g, dev, 0);
}

void um_gui_reap(um_gui *g)
{
    int i;
    if (!g->enabled) return;
    for (i = 0; i < UM_GUI_MAX_TOASTS; i++) {
        if (g->toasts[i] > 0) {
            int status;
            pid_t r = waitpid((pid_t)g->toasts[i], &status, WNOHANG);
            if (r == (pid_t)g->toasts[i] || (r < 0 && errno == ECHILD))
                g->toasts[i] = 0;       /* exited (or reaped elsewhere) */
        }
    }
}

void um_gui_shutdown(um_gui *g)
{
    int i, waited_ms = 0;
    if (!g->enabled) return;
    /* Toasts self-dismiss within ttl; give stragglers a short grace
     * period, then kill what is left so daemon exit stays responsive. */
    while (waited_ms < 400) {
        um_gui_reap(g);
        {
            int live = 0;
            for (i = 0; i < UM_GUI_MAX_TOASTS; i++) if (g->toasts[i] > 0) live++;
            if (!live) break;
        }
        {
            const struct timespec ts = { 0, 25 * 1000000L };
            nanosleep(&ts, NULL);
        }
        waited_ms += 25;
    }
    for (i = 0; i < UM_GUI_MAX_TOASTS; i++) {
        if (g->toasts[i] > 0) {
            kill((pid_t)g->toasts[i], SIGKILL);
            {
                int status;
                waitpid((pid_t)g->toasts[i], &status, 0);
            }
            g->toasts[i] = 0;
        }
    }
}
