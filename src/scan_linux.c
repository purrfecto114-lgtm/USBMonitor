/* scan_linux.c — enumerate external storage via sysfs.
 *
 * A device is reported when it is a block device reachable through USB,
 * or a removable "sd" / "mmcblk" unit (covers built-in SD readers, which
 * sit on the MMC bus).  loop, ram, zram, dm-, md, sr, nbd and pmem disks
 * are always skipped.
 *
 * sys_root allows tests to point the scanner at a fake /sys tree.
 */
#define _XOPEN_SOURCE 700
#include "usbmon.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdlib.h>
#include <unistd.h>

static int read_int_file(const char *root, const char *dev, const char *leaf, long long *out)
{
    char path[1024], buf[64];
    snprintf(path, sizeof path, "%s/block/%s/%s", root, dev, leaf);
    if (um_read_file_str(path, buf, sizeof buf) < 0) return -1;
    {
        char *end;
        errno = 0;
        *out = strtoll(buf, &end, 10);
        return (errno || end == buf || *end || *out < 0) ? -1 : 0;
    }
}

/* Walk canonical sysfs parents and inspect their subsystem links. */
static int usb_parent(const char *root, const char *dev, char *serial, size_t n)
{
    char path[PATH_MAX], resolved[PATH_MAX];
    const size_t root_len = strlen(root);

    snprintf(path, sizeof path, "%s/block/%s", root, dev);
    if (!realpath(path, resolved)) return 0;
    if (strncmp(resolved, root, root_len) ||
        strncmp(resolved + root_len, "/devices/", 9)) {
        /* Older mock trees and the legacy sysfs layout expose a device link. */
        snprintf(path, sizeof path, "%s/block/%s/device", root, dev);
        if (!realpath(path, resolved)) return 0;
    }
    if (strncmp(resolved, root, root_len) ||
        strncmp(resolved + root_len, "/devices/", 9)) return 0;

    while (strlen(resolved) > root_len + 8) {
        char link[PATH_MAX], *base;
        int written = snprintf(path, sizeof path, "%s/subsystem", resolved);
        ssize_t len;
        if (written < 0 || (size_t)written >= sizeof path) return 0;
        len = readlink(path, link, sizeof link - 1);
        if (len > 0 && (size_t)len < sizeof link - 1) {
            link[len] = '\0';
            base = strrchr(link, '/');
            if (!strcmp(base ? base + 1 : link, "usb")) {
                written = snprintf(path, sizeof path, "%s/serial", resolved);
                if (written > 0 && (size_t)written < sizeof path)
                    um_read_file_str(path, serial, n);
                return 1;
            }
        }
        base = strrchr(resolved, '/');
        *base = '\0';
    }
    return 0;
}

static int is_partition(const char *disk, const char *name)
{
    size_t len = strlen(disk);
    const char *suffix;
    if (strncmp(disk, name, len)) return 0;
    suffix = name + len;
    if (*suffix == 'p') suffix++;
    return *suffix && suffix[strspn(suffix, "0123456789")] == '\0';
}

static void read_trim(const char *root, const char *dev, const char *leaf,
                      char *out, size_t out_n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/block/%s/device/%s", root, dev, leaf);
    if (um_read_file_str(path, out, out_n) < 0) out[0] = '\0';
}

/* Find the first mount point of `dev` or any of its partitions. */
static void find_mount(um_device *dev)
{
    FILE *f = fopen("/proc/self/mounts", "r");
    char *line = NULL;
    size_t capacity = 0;
    if (!f) return;

    while (getline(&line, &capacity, f) >= 0) {
        char src[512], mnt[PATH_MAX];
        const char *p;
        size_t used = 0;
        struct statvfs vfs;
        int i;
        if (sscanf(line, "%511s %4095s", src, mnt) != 2 || strncmp(src, "/dev/", 5))
            continue;
        for (i = 0; i < dev->partition_count; i++) {
            if (!strcmp(src + 5, dev->partitions[i])) break;
        }
        if (strcmp(src + 5, dev->key) && i == dev->partition_count) continue;
        for (p = mnt; *p && used < sizeof dev->mount - 1; p++) {
            if (p[0] == '\\' && strspn(p + 1, "01234567") >= 3) {
                dev->mount[used++] = (char)((p[1]-'0') * 64 + (p[2]-'0') * 8 + p[3]-'0');
                p += 3;
            } else {
                dev->mount[used++] = *p;
            }
        }
        if (*p) {   /* A truncated path cannot be handed to a hook. */
            dev->mount[0] = '\0';
            continue;
        }
        dev->mount[used] = '\0';
        if (statvfs(dev->mount, &vfs) == 0) {
            dev->fs_total = (unsigned long long)vfs.f_frsize * vfs.f_blocks;
            dev->fs_free = (unsigned long long)vfs.f_frsize * vfs.f_bavail;
        }
        break;
    }
    free(line);
    fclose(f);
}

static int str_prefix_in(const char *s, const char *const *prefixes)
{
    int i;
    for (i = 0; prefixes[i]; i++)
        if (strncmp(s, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

int um_scan_platform(um_snapshot *snap, const char *sys_root)
{
    static const char *const skip_prefixes[] = {
        "loop", "ram", "zram", "dm-", "md", "sr", "nbd", "pmem", "hd", NULL
    };
    static const char *const removable_prefixes[] = { "sd", "mmcblk", NULL };

    char root[512], canonical[PATH_MAX];
    char blockdir[1024];
    DIR *d;
    struct dirent *ent;

    snap->count = 0;
    if (!sys_root || !*sys_root) sys_root = "/sys";
    if (!realpath(sys_root, canonical) || strlen(canonical) >= sizeof root) return -1;
    um_copy_str(root, sizeof root, canonical);
    snprintf(blockdir, sizeof blockdir, "%s/block", root);

    d = opendir(blockdir);
    if (!d) return -1;

    for (;;) {
        const char *dev;
        char usb_serial[UM_SERIAL_MAX] = "";
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            int failed = errno != 0;
            closedir(d);
            return failed ? -1 : 0;
        }
        dev = ent->d_name;
        long long removable = 0, sectors = 0;
        int is_usb;
        um_device *dv;

        if (dev[0] == '.' || strlen(dev) >= UM_KEY_MAX) continue;
        if (str_prefix_in(dev, skip_prefixes)) continue;

        if (read_int_file(root, dev, "removable", &removable) < 0) removable = 0;
        if (read_int_file(root, dev, "size", &sectors) != 0) continue; /* gone */

        is_usb = usb_parent(root, dev, usb_serial, sizeof usb_serial);
        if (!is_usb && !(removable == 1 && str_prefix_in(dev, removable_prefixes)))
            continue;

        if (snap->count >= UM_MAX_DEV || (unsigned long long)sectors > ULLONG_MAX / 512) {
            closedir(d);
            return -1;
        }
        dv = &snap->devs[snap->count];
        memset(dv, 0, sizeof *dv);
        um_copy_str(dv->key, sizeof dv->key, dev);
        dv->removable = (removable == 1);
        dv->size_bytes = (unsigned long long)sectors * 512ULL;
        um_copy_str(dv->bus, sizeof dv->bus, is_usb ? "usb" : "removable");

        {
            char vendor[64] = "", model[64] = "", serial[64] = "";
            read_trim(root, dev, "vendor", vendor, sizeof vendor);
            read_trim(root, dev, "model", model, sizeof model);
            read_trim(root, dev, "serial", serial, sizeof serial);
            if (vendor[0] && model[0])
                snprintf(dv->model, sizeof dv->model, "%.30s %.32s", vendor, model);
            else
                um_copy_str(dv->model, sizeof dv->model, model);
            um_copy_str(dv->serial, sizeof dv->serial, serial[0] ? serial : usb_serial);
            snprintf(dv->display, sizeof dv->display, "%.60s (%.40s)",
                     dv->model[0] ? dv->model : "USB storage", dev);
        }
        um_fingerprint(dv->serial[0] ? dv->serial : dv->key, dv->serial_fp,
                       sizeof dv->serial_fp);
        if (dv->serial[0]) um_copy_str(dv->identity, sizeof dv->identity, dv->serial_fp);

        /* partitions */
        {
            DIR *pd;
            struct dirent *pe;
            char pdir[1024];
            snprintf(pdir, sizeof pdir, "%s/block/%s", root, dev);
            pd = opendir(pdir);
            if (pd) {
                while ((pe = readdir(pd)) != NULL) {
                    if (dv->partition_count < UM_MAX_PARTITIONS &&
                        is_partition(dev, pe->d_name)) {
                        um_copy_str(dv->partitions[dv->partition_count],
                                    sizeof dv->partitions[0], pe->d_name);
                        dv->partition_count++;
                    }
                }
                closedir(pd);
            }
        }

        find_mount(dv);
        snap->count++;
    }
}
