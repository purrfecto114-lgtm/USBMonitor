/* lock.c — single-instance guard (flock on POSIX, exclusive file handle on Windows). */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
static HANDLE lock_file = INVALID_HANDLE_VALUE;

int um_single_instance_acquire(const char *lock_path)
{
    wchar_t path[640];
    um_utf8_to_wide(lock_path, path, sizeof path / sizeof *path);
    lock_file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock_file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION ? 1 : -1;
    return 0;
}

void um_single_instance_release(void)
{
    if (lock_file != INVALID_HANDLE_VALUE) CloseHandle(lock_file);
    lock_file = INVALID_HANDLE_VALUE;
}

#else /* POSIX */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

static int g_fd = -1;

int um_single_instance_acquire(const char *lock_path)
{
    g_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_fd < 0) return -1;
    if (flock(g_fd, LOCK_EX | LOCK_NB) != 0) {
        int busy = errno == EWOULDBLOCK;
        close(g_fd);
        g_fd = -1;
        return busy ? 1 : -1;
    }
    {
        char pid[32];
        int n = snprintf(pid, sizeof pid, "%d\n", (int)getpid());
        if (n > 0) { (void)!write(g_fd, pid, (size_t)n); }
        (void)!ftruncate(g_fd, (off_t)strlen(pid));
    }
    return 0;
}

void um_single_instance_release(void)
{
    if (g_fd >= 0) {
        flock(g_fd, LOCK_UN);
        close(g_fd);
        g_fd = -1;
    }
}
#endif
