/* scan_win32.c — enumerate external storage via Win32 (compiled only on Windows).
 *
 * Same API strategy as the original Python tool, minus the interpreter:
 *   GetLogicalDrives -> GetDriveTypeW -> CreateFileW(\\.\X:)
 *   -> IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS  (letter -> physical disk)
 *   -> \\.\PhysicalDriveN + IOCTL_STORAGE_QUERY_PROPERTY (bus type / serial)
 * Devices are keyed by physical disk number ("disk3") so that multi-partition
 * sticks appear once, with all letters listed.  When the extent query fails
 * (smart-pen LFB / HID edge cases) we fall back to querying the volume handle
 * itself and key by drive letter — mirroring the original's two-tier logic.
 */
#ifdef _WIN32

#include "usbmon.h"

#include <windows.h>
#include <string.h>
#include <stdlib.h>   /* _TRUNCATE (secure-CRT constant) */

#include <winioctl.h>   /* minimal winioctl; fallbacks below */

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif
#ifndef FILE_DEVICE_VOLUME
#define FILE_DEVICE_VOLUME 0x00000056
#endif
#ifndef IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
#define IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS \
    CTL_CODE(FILE_DEVICE_VOLUME, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* ---- storage query structures (avoid header-version drift) -------------- */
typedef enum _STORAGE_QUERY_TYPE2 { PQT_STANDARD = 0 } STORAGE_QUERY_TYPE2;
typedef enum _STORAGE_PROPERTY_ID2 { SPID_DEVICE = 0 } STORAGE_PROPERTY_ID2;

typedef struct _SPQ2 {
    STORAGE_PROPERTY_ID2  PropertyId;
    STORAGE_QUERY_TYPE2   QueryType;
    BYTE Additional[1];
} SPQ2;

typedef struct _SDD2 {
    DWORD Version;
    DWORD Size;
    BYTE  DeviceType;
    BYTE  DeviceTypeModifier;
    BOOLEAN RemovableMedia;
    BOOLEAN CommandQueueing;
    DWORD VendorIdOffset;
    DWORD ProductIdOffset;
    DWORD ProductRevisionOffset;
    DWORD SerialNumberOffset;
    DWORD BusType;                 /* STORAGE_BUS_TYPE */
    DWORD RawPropertiesLength;
    BYTE  RawDeviceProperties[1];
} SDD2;

typedef struct _DISK_EXTENT2 {
    DWORD DiskNumber;
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER ExtentLength;
} DISK_EXTENT2;

typedef struct _VDE2 {
    DWORD NumberOfDiskExtents;
    DISK_EXTENT2 Extents[1];
} VDE2;

typedef struct _SDH2 {          /* STORAGE_DESCRIPTOR_HEADER */
    DWORD Version;
    DWORD Size;
} SDH2;

#define UM_OPEN_FLAGS (FILE_SHARE_READ | FILE_SHARE_WRITE)

#ifndef IOCTL_STORAGE_QUERY_PROPERTY
#define IOCTL_STORAGE_BASE FILE_DEVICE_MASS_STORAGE
#define IOCTL_STORAGE_QUERY_PROPERTY \
    CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FILE_DEVICE_MASS_STORAGE
#define FILE_DEVICE_MASS_STORAGE 0x0000002d
#endif

static HANDLE open_device(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, 0, UM_OPEN_FLAGS, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return h;
}

static void wide_to_utf8(const wchar_t *in, char *out, size_t out_n)
{
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)out_n, NULL, NULL);
}

/* Copy a descriptor string located at byte `off` inside `buf`/`size`.
 * Driver data is not guaranteed NUL-terminated: copy bounded. */
static void copy_bounded(const BYTE *buf, DWORD size, DWORD off,
                         char *out, size_t out_n)
{
    const char *s;
    size_t avail, l = 0;
    out[0] = '\0';
    if (off < sizeof(SDD2) || off >= size) return;
    s = (const char *)buf + off;
    avail = (size_t)(size - off);
    while (l < avail && s[l] && l < out_n - 1) l++;
    memcpy(out, s, l);
    out[l] = '\0';
}

/* Query STORAGE_DEVICE_DESCRIPTOR on an already-open handle.
 * Fills bus_type (BusTypeUsb=7, Sd=12? use >=7 set), removable, serial,
 * vendor/product strings when offsets are present. Returns 0 on success. */
static int query_storage(HANDLE h, DWORD *bus_type, int *removable,
                         char *serial, size_t serial_n,
                         char *vendor, size_t vendor_n,
                         char *product, size_t product_n)
{
    SPQ2 q;
    SDH2 hdr;
    DWORD br = 0;
    SDD2 *desc;
    BYTE buf[4096];
    DWORD want;

    memset(&q, 0, sizeof q);
    q.PropertyId = SPID_DEVICE;
    q.QueryType = PQT_STANDARD;

    /* size probe: the driver requires a full 8-byte
     * STORAGE_DESCRIPTOR_HEADER (Version + Size), not a bare DWORD */
    memset(&hdr, 0, sizeof hdr);
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof q, &hdr, sizeof hdr, &br, NULL))
        return -1;
    if (hdr.Size < sizeof(SDD2) || hdr.Size > sizeof buf) return -1;
    want = hdr.Size;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof q, buf, want, &br, NULL))
        return -1;
    if (br < sizeof(SDD2)) return -1;

    desc = (SDD2 *)buf;
    if (bus_type)  *bus_type = desc->BusType;
    if (removable) *removable = desc->RemovableMedia ? 1 : 0;

    copy_bounded(buf, br, desc->SerialNumberOffset, serial, serial_n);
    copy_bounded(buf, br, desc->VendorIdOffset, vendor, vendor_n);
    copy_bounded(buf, br, desc->ProductIdOffset, product, product_n);
    return 0;
}

static int extent_disk_number(wchar_t letter, DWORD *disk_no)
{
    wchar_t vol[16];
    HANDLE h;
    BYTE buf[1024];
    DWORD br = 0;
    VDE2 *ext;

    _snwprintf_s(vol, 16, _TRUNCATE, L"\\\\.\\%c:", letter);
    h = open_device(vol);
    if (!h) return -1;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         NULL, 0, buf, sizeof buf, &br, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (br < sizeof(VDE2)) return -1;      /* validate bytes returned */
    ext = (VDE2 *)buf;
    if (ext->NumberOfDiskExtents < 1) return -1;
    *disk_no = ext->Extents[0].DiskNumber;
    return 0;
}

static int is_external_bus(DWORD bus)
{
    /* BusTypeUsb=7, BusTypeSd=12? — cover USB + SD + MMC readers. */
    return bus == 7 || bus == 12 || bus == 13;
}

int um_scan_platform(um_snapshot *snap, const char *sys_root)
{
    (void)sys_root;  /* unused on Windows */
    DWORD mask;
    wchar_t letter;
    int i;

    /* first pass: group letters by physical disk number */
    struct { DWORD disk; int has; wchar_t letters[8]; int nletters;
             char label[128]; ULONGLONG total, free; } disks[UM_MAX_DEV];
    int ndisk = 0;

    snap->count = 0;
    mask = GetLogicalDrives();
    if (!mask) return -1;

    memset(disks, 0, sizeof disks);   /* label/total read before first write */

    for (letter = L'A'; letter <= L'Z'; letter++) {
        wchar_t rootw[8];
        DWORD dtype;
        DWORD disk_no;
        int di = -1, k;

        if (!(mask & (1UL << (letter - L'A')))) continue;
        _snwprintf_s(rootw, 8, _TRUNCATE, L"%c:\\", letter);
        dtype = GetDriveTypeW(rootw);
        if (dtype != DRIVE_REMOVABLE && dtype != DRIVE_FIXED) continue;

        if (extent_disk_number(letter, &disk_no) == 0) {
            for (k = 0; k < ndisk; k++)
                if (disks[k].has && disks[k].disk == disk_no) { di = k; break; }
            if (di < 0 && ndisk < UM_MAX_DEV) {
                di = ndisk++;
                disks[di].has = 1;
                disks[di].disk = disk_no;
                disks[di].nletters = 0;
            }
        } else {
            di = -1;  /* no extent: letter-keyed fallback below */
        }

        {
            /* volume info (label / capacity) */
            wchar_t vlabel[64], fs[64];
            DWORD serial = 0, maxlen = 0, flags = 0;
            ULONGLONG total = 0, freeb = 0;

            if (GetVolumeInformationW(rootw, vlabel, 64, &serial, &maxlen,
                                      &flags, fs, 64)) {
                GetDiskFreeSpaceExW(rootw, NULL,
                                    (ULARGE_INTEGER *)&total, (ULARGE_INTEGER *)&freeb);
            } else {
                vlabel[0] = L'\0';
            }

            if (di >= 0) {
                if (disks[di].nletters < 8)
                    disks[di].letters[disks[di].nletters++] = letter;
                if (!disks[di].label[0] && vlabel[0])
                    wide_to_utf8(vlabel, disks[di].label, sizeof disks[di].label);
                if (total > disks[di].total) disks[di].total = total;
                disks[di].free = freeb;  /* last letter wins; partitions differ slightly */
            } else {
                /* fallback: letter itself as the unit, decided by volume query */
                wchar_t vol[16];
                HANDLE h;
                _snwprintf_s(vol, 16, _TRUNCATE, L"\\\\.\\%c:", letter);
                h = open_device(vol);
                if (h) {
                    DWORD bus = 0; int rem = 0;
                    char serials[64], vendor[64], product[64];
                    if (query_storage(h, &bus, &rem, serials, sizeof serials,
                                      vendor, sizeof vendor, product, sizeof product) == 0
                        && (is_external_bus(bus) || rem)
                        && snap->count < UM_MAX_DEV) {
                        um_device *dv = &snap->devs[snap->count];
                        char key[16], label8[128];
                        memset(dv, 0, sizeof *dv);
                        snprintf(key, sizeof key, "%c:", (char)letter);
                        um_copy_str(dv->key, sizeof dv->key, key);
                        wide_to_utf8(vlabel, label8, sizeof label8);
                        if (vendor[0] && product[0])
                            snprintf(dv->model, sizeof dv->model, "%s %s", vendor, product);
                        else
                            um_copy_str(dv->model, sizeof dv->model, label8);
                        um_copy_str(dv->serial, sizeof dv->serial, serials);
                        um_fingerprint(dv->serial[0] ? dv->serial : dv->key,
                                       dv->serial_fp, sizeof dv->serial_fp);
                        um_copy_str(dv->bus, sizeof dv->bus,
                                    is_external_bus(bus) ? "usb" : "removable");
                        dv->size_bytes = total;
                        dv->fs_total = total;
                        dv->fs_free = freeb;
                        snprintf(dv->display, sizeof dv->display, "%s (%s:)",
                                 dv->model[0] ? dv->model : "USB storage", key);
                        snprintf(dv->extra, sizeof dv->extra, "%c:", (char)letter);
                        if (dtype == DRIVE_REMOVABLE) dv->removable = 1;
                        if (snap->count < UM_MAX_DEV) snap->count++;
                    }
                    CloseHandle(h);
                }
            }
        }
    }

    /* second pass: physical-disk units */
    for (i = 0; i < ndisk; i++) {
        wchar_t phys[32];
        HANDLE h;
        _snwprintf_s(phys, 32, _TRUNCATE, L"\\\\.\\PhysicalDrive%lu", disks[i].disk);
        h = open_device(phys);
        if (!h) continue;
        {
            DWORD bus = 0; int rem = 0;
            char serial[64], vendor[64], product[64];
            if (query_storage(h, &bus, &rem, serial, sizeof serial,
                              vendor, sizeof vendor, product, sizeof product) == 0
                && (is_external_bus(bus) || rem)
                && snap->count < UM_MAX_DEV) {
                um_device *dv = &snap->devs[snap->count];
                int li;
                memset(dv, 0, sizeof *dv);
                snprintf(dv->key, sizeof dv->key, "disk%lu", disks[i].disk);
                if (vendor[0] && product[0])
                    snprintf(dv->model, sizeof dv->model, "%s %s", vendor, product);
                else
                    um_copy_str(dv->model, sizeof dv->model, disks[i].label);
                um_copy_str(dv->serial, sizeof dv->serial, serial);
                um_fingerprint(dv->serial[0] ? dv->serial : dv->key,
                               dv->serial_fp, sizeof dv->serial_fp);
                um_copy_str(dv->bus, sizeof dv->bus,
                            is_external_bus(bus) ? "usb" : "removable");
                dv->removable = rem;
                dv->size_bytes = disks[i].total;
                dv->fs_total = disks[i].total;
                dv->fs_free = disks[i].free;

                dv->extra[0] = '\0';
                for (li = 0; li < disks[i].nletters && li < 8; li++) {
                    char one[8];
                    snprintf(one, sizeof one, "%s%c:", li ? " " : "",
                             (char)disks[i].letters[li]);
                    strncat(dv->extra, one, sizeof dv->extra - strlen(dv->extra) - 1);
                    if (dv->partition_count < UM_MAX_PARTITIONS) {
                        snprintf(dv->partitions[dv->partition_count],
                                 sizeof dv->partitions[0], "%c:", (char)disks[i].letters[li]);
                        dv->partition_count++;
                    }
                }
                snprintf(dv->display, sizeof dv->display, "%s (%s)",
                         dv->model[0] ? dv->model : "USB storage", dv->extra);
                if (disks[i].label[0] && dv->partition_count)
                    snprintf(dv->mount, sizeof dv->mount, "%c:\\",
                             (char)disks[i].letters[0]);
                if (snap->count < UM_MAX_DEV) snap->count++;
            }
        }
        CloseHandle(h);
    }
    return 0;
}

#endif /* _WIN32 */
