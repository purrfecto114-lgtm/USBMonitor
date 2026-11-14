/* Project one Windows inventory into storage events and desktop rows. */
#include "um_enum.h"

#include <string.h>

#ifndef _WIN32
#include "win32_shim.h"
#else
#include <windows.h>
#include <winioctl.h>
#endif

static const char *bus_name(const um_evidence *e)
{
    if (e->tags & UM_TAG_USB) return "usb";
    if (e->bus_type == BusTypeSd) return "sd";
    if (e->bus_type == BusTypeMmc) return "mmc";
    return "removable";
}

void um_enum_snapshot(const um_evidence *evs, int n, um_snapshot *snap)
{
    int i;
    snap->count = 0;
    for (i = 0; i < n && snap->count < UM_MAX_DEV; i++) {
        const um_evidence *e = &evs[i];
        um_device *d;
        int letter;
        size_t used = 0;
        if (!(e->tags & UM_TAG_STORAGE) || e->disk_number < 0 || e->media_absent) continue;
        d = &snap->devs[snap->count++];
        memset(d, 0, sizeof *d);
        snprintf(d->key, sizeof d->key, "disk%d", e->disk_number);
        um_copy_str(d->bus, sizeof d->bus, bus_name(e));
        um_copy_str(d->model, sizeof d->model, e->model);
        um_copy_str(d->serial, sizeof d->serial, e->serial);
        um_fingerprint(e->instance_id, d->identity, sizeof d->identity);
        um_fingerprint(e->serial[0] ? e->serial : d->key, d->serial_fp, sizeof d->serial_fp);
        um_copy_str(d->mount, sizeof d->mount, e->mount);
        d->size_bytes = e->size;
        d->fs_total = e->fs_total;
        d->fs_free = e->fs_free;
        d->removable = e->removable;
        for (letter = 0; letter < 26; letter++) {
            if (!(e->letters & (1U << letter))) continue;
            if (d->partition_count < UM_MAX_PARTITIONS)
                snprintf(d->partitions[d->partition_count++], UM_NAME_MAX, "%c:", 'A' + letter);
            used += (size_t)snprintf(d->extra + used, sizeof d->extra - used,
                                     "%s%c:", used ? " " : "", 'A' + letter);
        }
        snprintf(d->display, sizeof d->display, "%s (disk%d)",
                 e->model[0] ? e->model : "External storage", e->disk_number);
    }
}

int um_scan_windows(um_snapshot *snap, um_evidence *out, int max)
{
    int n = um_enum_collect(out, max);
    if (n >= 0) um_enum_snapshot(out, n, snap);
    return n;
}

#ifdef _WIN32
int um_scan_platform(um_snapshot *snap, const char *sys_root)
{
    um_evidence evidence[UM_EVID_MAX];
    (void)sys_root;
    return um_scan_windows(snap, evidence, UM_EVID_MAX) < 0 ? -1 : 0;
}
#endif

static const char *kind_name(um_dev_kind kind)
{
    switch (kind) {
    case UM_KIND_USB_STORAGE:       return "可移动磁盘";
    case UM_KIND_STORAGE_NO_LETTER: return "外接存储设备";
    case UM_KIND_EMPTY_SLOT:        return "读卡器";
    case UM_KIND_LOCKED_VOLUME:     return "不可访问的卷";
    case UM_KIND_DOCK_HUB:          return "USB 拓展坞 / 集线器";
    case UM_KIND_NET_ADAPTER:       return "USB 网络设备";
    case UM_KIND_HID_PEN:           return "USB HID 设备";
    default:                        return "USB 设备";
    }
}

int um_enum_to_model(const um_evidence *evs, int n, um_toast_model *out)
{
    int i, volumes = 0;
    memset(out, 0, sizeof *out);
    for (i = 0; i < n && out->n_rows < UM_UI_MAX_ROWS; i++) {
        const um_evidence *e = &evs[i];
        um_volume *row;
        char letters[26 * 5] = "";
        int k;
        size_t used = 0;
        if (e->kind == UM_KIND_UNKNOWN) continue;
        row = &out->rows[out->n_rows++];
        row->kind = e->kind;
        row->disk_number = e->disk_number;
        row->openable = e->accessible_letters != 0;
        row->ejectable = e->ejectable;
        row->pct = -1;
        um_copy_str(row->instance_id, sizeof row->instance_id, e->instance_id);
        for (k = 0; k < 26; k++) {
            unsigned bit = 1U << k;
            if (!(e->letters & bit)) continue;
            if ((e->accessible_letters & bit) && !row->path[0])
                snprintf(row->path, sizeof row->path, "%c:\\", 'A' + k);
            snprintf(row->letters[row->n_letters++], sizeof row->letters[0], "%c:", 'A' + k);
            used += (size_t)snprintf(letters + used, sizeof letters - used,
                                     "%s%c:", used ? "、" : "", 'A' + k);
        }
        row->accessible_letters = e->accessible_letters;
        if (e->model[0])
            um_copy_str(row->title, sizeof row->title, e->model);
        else if (used)
            snprintf(row->title, sizeof row->title, "%.58s %s", letters, kind_name(e->kind));
        else
            um_copy_str(row->title, sizeof row->title, kind_name(e->kind));

        if (e->kind == UM_KIND_LOCKED_VOLUME)
            snprintf(row->subtitle, sizeof row->subtitle, "%.100s · 卷当前不可访问", letters);
        else if (row->n_letters > 1)
            snprintf(row->subtitle, sizeof row->subtitle, "%.112s · %d 个盘符",
                     letters, row->n_letters);
        else if (row->n_letters == 1)
            snprintf(row->subtitle, sizeof row->subtitle, "%s · %.120s", kind_name(e->kind), letters);
        else if (e->mount[0])
            snprintf(row->subtitle, sizeof row->subtitle, "%.100s · 目录挂载", e->mount);
        else
            um_ui_device_notice(e->kind, row->subtitle, sizeof row->subtitle);

        if (e->fs_total && e->fs_free <= e->fs_total) {
            row->total = e->fs_total;
            row->free = e->fs_free;
            row->used = row->total - row->free;
            row->pct = (int)(100.0 * (double)row->used / (double)row->total);
            um_ui_capacity_line(row, row->capacity, sizeof row->capacity);
        } else if (e->size) {
            char size[32];
            um_ui_format_bytes(e->size, size, sizeof size);
            snprintf(row->capacity, sizeof row->capacity, "设备容量 %s", size);
        }
        volumes += e->volumes;
    }
    out->overview = 1;
    snprintf(out->headline, sizeof out->headline, "USB 设备监控");
    snprintf(out->subtitle, sizeof out->subtitle, "%d 个设备 · %d 个卷", out->n_rows, volumes);
    snprintf(out->count, sizeof out->count, "%d 个", out->n_rows);
    snprintf(out->summary, sizeof out->summary, "%s",
             out->n_rows ? "设备概览" : "当前没有检测到外接设备");
    return out->n_rows;
}
