/* lock.c — single-instance guard (flock on POSIX, named mutex on Windows). */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
static HANDLE g_mutex = NULL;

int um_single_instance_acquire(const char *lock_path)
{
    wchar_t wpath[512];
    (void)lock_path;
    MultiByteToWideChar(CP_UTF8, 0, "Local\\usbmon-singleton", -1, wpath, 512);
    g_mutex = CreateMutexW(NULL, TRUE, wpath);
    if (!g_mutex) return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return -1;
    }
    return 0;
}

void um_single_instance_release(void)
{
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL; }
}

#else /* POSIX */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

static int g_fd = -1;

int um_single_instance_acquire(const char *lock_path)
{
    g_fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (g_fd < 0) return -1;
    if (flock(g_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
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
