/* Linux: kernel uevents in production, inotify for a mock sysfs tree. */
#define _GNU_SOURCE
#include "usbmon.h"

#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int hot_fd = -1;
static int mock_watch = -1;
static char mock_block[1024];
static const uint32_t mock_mask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
                                  IN_DELETE_SELF | IN_MOVE_SELF;

/* The kernel sends an action@path header followed by NUL-delimited fields. */
int um_hot_block_event(const char *data, size_t len)
{
    int block = 0, change = 0;
    while (len) {
        const char *end = memchr(data, '\0', len);
        size_t field;
        if (!end) return 0;
        field = (size_t)(end - data);
        if (field == sizeof "SUBSYSTEM=block" - 1 && !strcmp(data, "SUBSYSTEM=block"))
            block = 1;
        if (!strcmp(data, "ACTION=add") || !strcmp(data, "ACTION=remove") ||
            !strcmp(data, "ACTION=change"))
            change = 1;
        data = end + 1;
        len -= field + 1;
    }
    return block && change;
}

int um_hot_init(const char *sys_root)
{
    um_hot_close();
    if (sys_root && *sys_root && strcmp(sys_root, "/sys") && strcmp(sys_root, "/sys/")) {
        int n = snprintf(mock_block, sizeof mock_block, "%s/block", sys_root);
        if (n < 0 || (size_t)n >= sizeof mock_block) return -1;
        hot_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (hot_fd >= 0) mock_watch = inotify_add_watch(hot_fd, mock_block, mock_mask);
    } else {
        struct sockaddr_nl address = {0};
        address.nl_family = AF_NETLINK;
        address.nl_groups = 1;  /* Kernel kobject notifications, before udev processing. */
        hot_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        NETLINK_KOBJECT_UEVENT);
        if (hot_fd >= 0 && bind(hot_fd, (struct sockaddr *)&address, sizeof address) != 0)
            um_hot_close();
    }
    return hot_fd;
}

void um_hot_close(void)
{
    if (hot_fd >= 0) close(hot_fd);
    hot_fd = -1;
    mock_watch = -1;
    mock_block[0] = '\0';
}

static int read_mock(void)
{
    union { struct inotify_event align; char bytes[4096]; } buffer;
    ssize_t n = read(hot_fd, buffer.bytes, sizeof buffer.bytes);
    size_t offset = 0;
    if (n <= 0) return 0;
    while (offset + sizeof(struct inotify_event) <= (size_t)n) {
        const struct inotify_event *ev = (const void *)(buffer.bytes + offset);
        if (ev->wd == mock_watch && (ev->mask & (IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF))) {
            if (ev->mask & IN_MOVE_SELF) inotify_rm_watch(hot_fd, mock_watch);
            mock_watch = -1;
        }
        offset += sizeof *ev + ev->len;
    }
    return 1;  /* Including overflow and watch invalidation, which need a rescan. */
}

static int read_kernel(void)
{
    int changed = 0, batch;
    for (batch = 0; batch < 64; batch++) {
        struct sockaddr_nl from = {0};
        socklen_t from_len = sizeof from;
        char data[8192];
        ssize_t n = recvfrom(hot_fd, data, sizeof data, MSG_TRUNC,
                             (struct sockaddr *)&from, &from_len);
        if (n < 0) return changed || errno == ENOBUFS;
        if (from_len == sizeof from && from.nl_family == AF_NETLINK &&
            from.nl_pid == 0 && from.nl_groups == 1 &&
            ((size_t)n > sizeof data || um_hot_block_event(data, (size_t)n)))
            changed = 1;
    }
    return changed;
}

int um_hot_wait(int timeout_ms)
{
    struct pollfd fd = { hot_fd, POLLIN, 0 };
    if (hot_fd < 0) {
        poll(NULL, 0, timeout_ms);
        return 0;
    }
    if (mock_block[0] && mock_watch < 0) {
        mock_watch = inotify_add_watch(hot_fd, mock_block, mock_mask);
        if (mock_watch >= 0) return 1;
    }
    if (poll(&fd, 1, timeout_ms) <= 0) return 0;
    if (fd.revents & (POLLHUP | POLLNVAL)) {
        um_hot_close();
        return 1;
    }
    return mock_block[0] ? read_mock() : read_kernel();
}
