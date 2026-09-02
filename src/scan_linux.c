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
    *out = strtoll(buf, NULL, 10);
    return 0;
}

/* True when /sys/block/<dev>/device resolves through a USB hub. */
static int device_is_usb(const char *root, const char *dev)
{
    char path[1024], resolved[4096];
    ssize_t n;
    snprintf(path, sizeof path, "%s/block/%s/device", root, dev);
    n = readlink(path, resolved, sizeof resolved - 1);
    if (n < 0) {
        /* No symlink: try realpath (works for directories). */
        if (!realpath(path, resolved)) return 0;
    } else {
        resolved[n] = '\0';
    }
    return strstr(resolved, "/usb") != NULL;
}

static void read_trim(const char *root, const char *dev, const char *leaf,
                      char *out, size_t out_n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/block/%s/device/%s", root, dev, leaf);
    if (um_read_file_str(path, out, out_n) < 0) out[0] = '\0';
}

/* Find the first mount point of `dev` or any of its partitions. */
static void find_mount(const char *dev, char *out, size_t out_n,
                       unsigned long long *total, unsigned long long *freeb)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    char want[128];
    out[0] = '\0';
    *total = *freeb = 0;
    if (!f) return;

    snprintf(want, sizeof want, "/dev/%s", dev);
    while (fgets(line, sizeof line, f)) {
        char src[512], mnt[512], fs[64];
        if (sscanf(line, "%511s %511s %63s", src, mnt, fs) != 3) continue;
        int match = strcmp(src, want) == 0;
        if (!match) {
            /* partition: /dev/sdb1 vs dev sdb; /dev/mmcblk0p1 vs dev mmcblk0 */
            size_t wl = strlen(want);
            if (strncmp(src, want, wl) == 0) {
                const char *sfx = src + wl;
                if (isdigit((unsigned char)sfx[0])) match = 1;             /* sdb1   */
                else if (sfx[0] == 'p' && isdigit((unsigned char)sfx[1]))  /* mmcblk0p1 */
                    match = 1;
            }
        }
        if (match) {
            /* decode octal escapes (\040 for space) in the mount point */
            const char *p = mnt;
            char *w = out;
            while (*p && (size_t)(w - out) < out_n - 1) {
                if (p[0] == '\\' && p[1] >= '0' && p[1] <= '7' &&
                    p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
                    *w++ = (char)((p[1]-'0')*64 + (p[2]-'0')*8 + (p[3]-'0'));
                    p += 4;
                } else {
                    *w++ = *p++;
                }
            }
            *w = '\0';
            struct statvfs vfs;
            if (statvfs(out, &vfs) == 0) {
                *total = (unsigned long long)vfs.f_frsize * vfs.f_blocks;
                *freeb = (unsigned long long)vfs.f_frsize * vfs.f_bavail;
            }
            break;
        }
    }
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

    char root[512];
    char blockdir[1024];
    DIR *d;
    struct dirent *ent;

    snap->count = 0;
    if (!sys_root || !*sys_root) sys_root = "/sys";
    snprintf(root, sizeof root, "%s", sys_root);
    snprintf(blockdir, sizeof blockdir, "%s/block", root);

    d = opendir(blockdir);
    if (!d) return -1;

    while ((ent = readdir(d)) != NULL) {
        const char *dev = ent->d_name;
        long long removable = 0, sectors = 0;
        int is_usb;
        um_device *dv;

        if (dev[0] == '.') continue;
        if (str_prefix_in(dev, skip_prefixes)) continue;
        if (snap->count >= UM_MAX_DEV) break;

        read_int_file(root, dev, "removable", &removable);
        if (read_int_file(root, dev, "size", &sectors) != 0) continue; /* gone */

        is_usb = device_is_usb(root, dev);
        if (!is_usb && !(removable == 1 && str_prefix_in(dev, removable_prefixes)))
            continue;

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
            /* trim trailing spaces SCSI descriptors are famous for */
            {
                size_t l = strlen(model);
                while (l > 0 && model[l-1] == ' ') model[--l] = '\0';
                l = strlen(vendor);
                while (l > 0 && vendor[l-1] == ' ') vendor[--l] = '\0';
            }
            if (vendor[0] && model[0])
                snprintf(dv->model, sizeof dv->model, "%s %s", vendor, model);
            else
                um_copy_str(dv->model, sizeof dv->model, model);
            um_copy_str(dv->serial, sizeof dv->serial, serial);
            snprintf(dv->display, sizeof dv->display, "%.60s (%.40s)",
                     dv->model[0] ? dv->model : "USB storage", dev);
        }
        um_fingerprint(dv->serial[0] ? dv->serial : dv->key, dv->serial_fp,
                       sizeof dv->serial_fp);

        /* partitions */
        {
            DIR *pd;
            struct dirent *pe;
            char pdir[1024];
            snprintf(pdir, sizeof pdir, "%s/block/%s", root, dev);
            pd = opendir(pdir);
            if (pd) {
                size_t dl = strlen(dev);
                while ((pe = readdir(pd)) != NULL) {
                    if (strncmp(pe->d_name, dev, dl) == 0 && dv->partition_count < UM_MAX_PARTITIONS &&
                    (isdigit((unsigned char)pe->d_name[dl]) ||                       /* sdb1        */
                     (pe->d_name[dl] == 'p' && isdigit((unsigned char)pe->d_name[dl + 1]))) /* mmcblk0p1 */
                    ) {
                        um_copy_str(dv->partitions[dv->partition_count],
                                    sizeof dv->partitions[0], pe->d_name);
                        dv->partition_count++;
                    }
                }
                closedir(pd);
            }
        }

        find_mount(dev, dv->mount, sizeof dv->mount, &dv->fs_total, &dv->fs_free);
        snap->count++;
    }
    closedir(d);
    return 0;
}
