/* Last successful inventory. A key may be followed by a private fingerprint. */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#endif

int um_snapshot_match(const um_snapshot *snap, const um_device *dev)
{
    int i;
    for (i = 0; i < snap->count; i++) {
        const um_device *prev = &snap->devs[i];
        if (!strcmp(prev->key, dev->key) &&
            (!prev->identity[0] || !dev->identity[0] ||
             !strcmp(prev->identity, dev->identity))) return 1;
    }
    return 0;
}

int um_state_load(const char *path, um_snapshot *snap)
{
    char line[UM_KEY_MAX + 32];
    FILE *f = um_fopen(path, "rb");
    snap->count = 0;
    if (!f) return errno == ENOENT ? 0 : -1;

    while (fgets(line, sizeof line, f)) {
        char *id, *end = strpbrk(line, "\r\n");
        um_device *dev;
        if (!end && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        if (end) *end = '\0';
        id = strchr(line, '\t');
        if (id) *id++ = '\0';
        if (!line[0] || strlen(line) >= UM_KEY_MAX || snap->count >= UM_MAX_DEV)
            continue;
        dev = &snap->devs[snap->count];
        memset(dev, 0, sizeof *dev);
        um_copy_str(dev->key, sizeof dev->key, line);
        if (id && strlen(id) == 19 && !strncmp(id, "sha256:", 7) &&
            strspn(id + 7, "0123456789abcdef") == 12)
            um_copy_str(dev->identity, sizeof dev->identity, id);
        if (!um_snapshot_match(snap, dev)) snap->count++;
    }
    if (ferror(f)) {
        fclose(f);
        snap->count = 0;
        return -1;
    }
    fclose(f);
    return 1;
}

int um_state_save(const char *path, const um_snapshot *snap)
{
    char tmp[640];
    FILE *f;
    int i, ok = 1;
#ifdef _WIN32
    wchar_t src[640], dst[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = um_fopen(tmp, "wb");
    if (!f) return -1;
#else
    int fd;
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", path);
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return -1;
    }
#endif
    for (i = 0; i < snap->count; i++) {
        const um_device *dev = &snap->devs[i];
        if (fprintf(f, "%s%s%s\n", dev->key,
                    dev->identity[0] ? "\t" : "", dev->identity) < 0) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
#ifdef _WIN32
    um_utf8_to_wide(tmp, src, sizeof src / sizeof *src);
    um_utf8_to_wide(path, dst, sizeof dst / sizeof *dst);
    if (ok && MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) return 0;
    DeleteFileW(src);
#else
    if (ok && rename(tmp, path) == 0) return 0;
    unlink(tmp);
#endif
    return -1;
}
