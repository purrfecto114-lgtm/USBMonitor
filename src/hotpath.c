/* hotpath.c — event-driven wakeup for the round loop (Linux/POSIX).
 *
 * Watches <sys_root>/block with inotify.  A create/delete there means a
 * block device appeared or vanished, which is exactly when a scan round
 * is interesting — so instead of waiting for the hourly tick the daemon
 * reacts within UM_HOT_SETTLE_MS (settling lets udev finish symlinks,
 * partitions and mounts before we scan).
 *
 * The hourly interval remains the guaranteed *maximum* cadence; this file
 * only makes reaction time near-instant.  Costs nothing while idle: the
 * main loop blocks in poll() on the inotify fd instead of sleeping.
 *
 * If the watch cannot be established (very old kernel, exotic sysfs),
 * um_hot_init returns -1 and the daemon silently degrades to pure
 * interval mode — no failure path, no polling fallback storm.
 */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#ifndef _WIN32

#include <errno.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int g_ifd = -1;      /* inotify fd                       */
static int g_wd  = -1;      /* watch descriptor on <root>/block */
static char g_blockdir[1024];

int um_hot_init(const char *sys_root)
{
    const char *root = (sys_root && *sys_root) ? sys_root : "/sys";
    g_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_ifd < 0) return -1;
    snprintf(g_blockdir, sizeof g_blockdir, "%s/block", root);
    g_wd = inotify_add_watch(g_ifd, g_blockdir,
                             IN_CREATE | IN_DELETE |
                             IN_MOVED_TO | IN_MOVED_FROM);
    if (g_wd < 0) {
        close(g_ifd);
        g_ifd = -1;
        return -1;
    }
    return g_ifd;
}

void um_hot_close(void)
{
    if (g_ifd >= 0) close(g_ifd);
    g_ifd = -1;
    g_wd = -1;
}

/* Re-arm after IN_IGNORED (watched dir replaced — happens with mock
 * sysfs trees in tests; harmless no-op when the watch is still valid). */
static void rewatch(void)
{
    if (g_ifd >= 0) {
        g_wd = inotify_add_watch(g_ifd, g_blockdir,
                                 IN_CREATE | IN_DELETE |
                                 IN_MOVED_TO | IN_MOVED_FROM);
        if (g_wd < 0) {              /* directory gone for good: drop hot path */
            close(g_ifd);
            g_ifd = -1;
        }
    }
}

int um_hot_wait(int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    char buf[4096] __attribute__((aligned(8)));
    int woke = 0;

    if (g_ifd < 0) {                 /* no hot path: plain sleep */
        struct timespec ts;
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        return 0;
    }

    FD_ZERO(&rfds);
    FD_SET(g_ifd, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(g_ifd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return 0;                    /* timeout or error: treat as tick */

    /* drain all pending events, flag interesting ones */
    for (;;) {
        ssize_t n = read(g_ifd, buf, sizeof buf);
        if (n <= 0) break;           /* EAGAIN: drained */
        {
            const struct inotify_event *ev;
            char *p = buf;
            while (p + sizeof *ev <= buf + n) {
                ev = (const struct inotify_event *)p;
                if (ev->mask & IN_IGNORED) rewatch();
                if (ev->mask & (IN_CREATE | IN_DELETE | IN_MOVED_TO |
                                IN_MOVED_FROM | IN_Q_OVERFLOW))
                    woke = 1;
                p += sizeof *ev + ev->len;
            }
        }
    }
    return woke;
}

#endif /* !_WIN32 */
