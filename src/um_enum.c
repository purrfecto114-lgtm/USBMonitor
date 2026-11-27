/* SetupAPI devices are the inventory. Volume GUIDs supply optional mounts. */
#include "um_enum.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#ifndef UM_WIN32_SHIM
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
static const GUID disk_class =
    {0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
static const GUID hid_class =
    {0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
static const GUID net_class =
    {0xcac88484,0x7515,0x4c03,{0x82,0xe6,0x71,0xa8,0x7a,0xb8,0x62,0x9b}};
static const GUID hub_class =
    {0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0x00,0xa0,0xc9,0x08,0xbe,0xdc}};
#define DISK_CLASS (&disk_class)
#define HID_CLASS (&hid_class)
#define NET_CLASS (&net_class)
#define HUB_CLASS (&hub_class)
#else
#include "win32_shim.h"
#define DISK_CLASS ((const GUID *)IF_DISK)
#define HID_CLASS ((const GUID *)IF_HID)
#define NET_CLASS ((const GUID *)IF_NET)
#define HUB_CLASS ((const GUID *)IF_HUB)
#endif

static int has_prefix(const wchar_t *id, const wchar_t *prefix)
{
    while (*prefix && towupper((wint_t)*id) == (wint_t)*prefix) {
        id++;
        prefix++;
    }
    return *prefix == L'\0';
}

static int usb_node(DEVINST start, DEVINST *eject)
{
    DEVINST cur = start, parent;
    int usb = 0, composite = 0, depth;
    if (eject) *eject = 0;
    for (depth = 0; depth < 32; depth++, cur = parent) {
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDW(cur, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        if (has_prefix(id, L"USB\\ROOT_HUB")) break;
        if (has_prefix(id, L"USBSTOR\\") || has_prefix(id, L"USBHUB\\"))
            usb = 1;
        if (has_prefix(id, L"USB\\VID_")) {
            const wchar_t *instance = wcschr(id + 4, L'\\');
            const wchar_t *part;
            usb = 1;
            for (part = id + 4; *part && part != instance; part++) {
                if (has_prefix(part, L"&MI_")) {
                    composite = 1;
                    break;
                }
            }
            if (!*part || part == instance) {
                DWORD caps = 0, bytes = sizeof caps;
                if (eject && !composite &&
                    CM_Get_DevNode_Registry_PropertyW(cur, CM_DRP_CAPABILITIES, NULL,
                                                     &caps, &bytes, 0) == CR_SUCCESS &&
                    bytes == sizeof caps && (caps & CM_DEVCAP_REMOVABLE) &&
                    !(caps & CM_DEVCAP_DOCKDEVICE))
                    *eject = cur;
                break;
            }
        }
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || parent == cur) break;
    }
    return usb;
}

static HANDLE open_device(const wchar_t *path)
{
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static void descriptor_text(char *out, size_t n, const BYTE *raw, DWORD got, DWORD offset)
{
    size_t len = 0;
    if (offset < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) || offset >= got)
        return;
    while (len + 1 < n && offset + len < got && raw[offset + len]) {
        BYTE c = raw[offset + len];
        out[len++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    while (len && out[len - 1] == ' ') len--;
    out[len] = '\0';
}

static void probe_disk(const wchar_t *path, um_evidence *e)
{
    HANDLE h = open_device(path);
    STORAGE_DEVICE_NUMBER number;
    STORAGE_PROPERTY_QUERY query = {0};
    union { STORAGE_DEVICE_DESCRIPTOR d; BYTE raw[4096]; } buffer;
    DISK_GEOMETRY_EX geometry;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return;

    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &number,
                        sizeof number, &got, NULL) &&
        got >= sizeof number && number.DeviceNumber <= INT_MAX)
        e->disk_number = (int)number.DeviceNumber;

    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query,
                        buffer.raw, sizeof buffer.raw, &got, NULL) &&
        got >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) &&
        got <= sizeof buffer.raw) {
        e->bus_type = buffer.d.BusType;
        e->removable = buffer.d.RemovableMedia != 0;
        if (buffer.d.Size < got) got = buffer.d.Size;
        descriptor_text(e->serial, sizeof e->serial, buffer.raw, got, buffer.d.SerialNumberOffset);
        if (!e->model[0])
            descriptor_text(e->model, sizeof e->model, buffer.raw, got, buffer.d.ProductIdOffset);
    }

    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geometry,
                        sizeof geometry, &got, NULL) &&
        got >= offsetof(DISK_GEOMETRY_EX, Data) && geometry.DiskSize.QuadPart > 0)
        e->size = (unsigned long long)geometry.DiskSize.QuadPart;

    if (!e->size && !DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, NULL, 0,
                                    &got, NULL)) {
        DWORD error = GetLastError();
        e->media_absent = error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE;
    }
    CloseHandle(h);
}

static int disk_at_volume(const wchar_t *path)
{
    HANDLE h = open_device(path);
    VOLUME_DISK_EXTENTS extents;
    DWORD got = 0;
    int disk = -1;
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents,
                        sizeof extents, &got, NULL) && got >= sizeof extents &&
        extents.NumberOfDiskExtents == 1 && extents.Extents[0].DiskNumber <= INT_MAX)
        disk = (int)extents.Extents[0].DiskNumber;
    CloseHandle(h);
    return disk;
}

int um_enum_letter_disk(char letter)
{
    wchar_t path[] = L"\\\\.\\A:";
    if (letter < 'A' || letter > 'Z') return -1;
    path[4] = (wchar_t)letter;
    return disk_at_volume(path);
}

static int collect_interfaces(um_evidence *out, int *n, int max, const GUID *cls,
                              unsigned tag)
{
    HDEVINFO set = SetupDiGetClassDevsW(cls, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    DWORD i;
    int result = -1;
    if (set == INVALID_HANDLE_VALUE) return -1;

    for (i = 0; ; i++) {
        SP_DEVICE_INTERFACE_DATA iface = {0};
        SP_DEVINFO_DATA info = {0};
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        DWORD needed = 0, error;
        wchar_t id[MAX_DEVICE_ID_LEN], friendly[128];
        DEVINST eject;
        um_evidence e = {0};
        int k;

        iface.cbSize = sizeof iface;
        if (!SetupDiEnumDeviceInterfaces(set, NULL, cls, i, &iface)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) result = 0;
            break;
        }

        SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &needed, NULL);
        if (needed < sizeof *detail) {
            error = GetLastError();
            if (error == ERROR_NO_SUCH_DEVICE || error == ERROR_DEVICE_NOT_CONNECTED ||
                error == ERROR_NO_SUCH_DEVINST) continue;
            break;
        }
        detail = malloc(needed);
        if (!detail) break;
        detail->cbSize = sizeof *detail;
        info.cbSize = sizeof info;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, NULL, &info)) {
            error = GetLastError();
            free(detail);
            if (error == ERROR_NO_SUCH_DEVICE || error == ERROR_DEVICE_NOT_CONNECTED ||
                error == ERROR_NO_SUCH_DEVINST) continue;
            break;
        }

        e.tags = tag;
        e.disk_number = -1;
        if (usb_node(info.DevInst, &eject)) e.tags |= UM_TAG_USB;
        if (tag == UM_TAG_STORAGE) {
            probe_disk(detail->DevicePath, &e);
            if (e.bus_type == BusTypeUsb) e.tags |= UM_TAG_USB;
        }
        free(detail);

        if (!(e.tags & UM_TAG_USB) && e.bus_type != BusTypeSd && e.bus_type != BusTypeMmc &&
            !(tag == UM_TAG_STORAGE && e.bus_type == 0 && e.removable && e.disk_number >= 0)) continue;
        if (CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
            um_wide_to_utf8(id, e.instance_id, sizeof e.instance_id);
        if (!e.instance_id[0]) continue;

        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME,
                                              NULL, (BYTE *)friendly, sizeof friendly, NULL) ||
            SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC,
                                              NULL, (BYTE *)friendly, sizeof friendly, NULL))
            um_wide_to_utf8(friendly, e.model, sizeof e.model);
        e.ejectable = eject != 0 && tag == UM_TAG_STORAGE && !e.media_absent;

        for (k = 0; k < *n; k++) {
            if ((e.disk_number >= 0 && e.disk_number == out[k].disk_number) ||
                !strcmp(e.instance_id, out[k].instance_id)) break;
        }
        if (k < *n) continue;
        if (*n == max) {
            /* Preserve storage completeness. Non-storage rows are best effort. */
            if (tag == UM_TAG_STORAGE) break;
            result = 0;
            break;
        }
        out[(*n)++] = e;
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

static void attach_paths(const wchar_t *volume, um_evidence *e)
{
    wchar_t local[256], *paths = local;
    DWORD needed = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume, local, 256, &needed)) {
        if (GetLastError() != ERROR_MORE_DATA || needed > 65536 || needed <= 256) return;
        paths = malloc(needed * sizeof *paths);
        if (!paths) return;
        if (!GetVolumePathNamesForVolumeNameW(volume, paths, needed, &needed)) {
            free(paths);
            return;
        }
    }
    {
        const wchar_t *path;
        for (path = paths; *path; path += wcslen(path) + 1) {
            ULARGE_INTEGER total, free_bytes;
            int letter = -1;
            char mount[UM_MOUNT_MAX];
            if (wcslen(path) == 3 && path[0] >= L'A' && path[0] <= L'Z' &&
                path[1] == L':' && path[2] == L'\\') {
                letter = (int)(path[0] - L'A');
                e->letters |= 1U << letter;
            }
            if (!GetDiskFreeSpaceExW(path, &free_bytes, &total, NULL)) continue;
            if (letter >= 0) e->accessible_letters |= 1U << letter;
            um_wide_to_utf8(path, mount, sizeof mount);
            if (mount[0] && (!e->mount[0] ||
                (letter >= 0 && (strlen(e->mount) > 3 || strcmp(mount, e->mount) < 0)))) {
                um_copy_str(e->mount, sizeof e->mount, mount);
                e->fs_total = total.QuadPart;
                e->fs_free = free_bytes.QuadPart;
            }
        }
    }
    if (paths != local) free(paths);
}

static void collect_volumes(um_evidence *out, int n)
{
    wchar_t name[MAX_PATH];
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        size_t len = wcslen(name);
        int disk, i;
        if (!len || name[len - 1] != L'\\') continue;
        name[len - 1] = L'\0'; /* Open the volume device, not its root directory. */
        disk = disk_at_volume(name);
        name[len - 1] = L'\\';
        if (disk < 0) continue;
        for (i = 0; i < n; i++) {
            if (out[i].disk_number != disk) continue;
            out[i].volumes++;
            attach_paths(name, &out[i]);
            break;
        }
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
}

int um_enum_collect(um_evidence *out, int max)
{
    int n = 0, i;
    DWORD old_mode;
    int restore;
    if (!out || max <= 0) return -1;

    restore = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    if (collect_interfaces(out, &n, max, DISK_CLASS, UM_TAG_STORAGE) ||
        collect_interfaces(out, &n, max, HID_CLASS, UM_TAG_HID) ||
        collect_interfaces(out, &n, max, NET_CLASS, UM_TAG_NET) ||
        collect_interfaces(out, &n, max, HUB_CLASS, UM_TAG_HUB)) {
        n = -1;
    } else {
        collect_volumes(out, n);
        for (i = 0; i < n; i++) {
            um_evidence *e = &out[i];
            if (e->letters) e->tags |= UM_TAG_VOLUME;
            e->kind = um_ui_classify(e->tags, e->letters != 0,
                                     (e->tags & UM_TAG_STORAGE) != 0,
                                     e->removable, !e->media_absent,
                                     !e->letters || e->accessible_letters != 0);
        }
    }
    if (restore) SetThreadErrorMode(old_mode, NULL);
    return n;
}

unsigned long um_enum_eject(const char *instance_id)
{
    wchar_t id[MAX_DEVICE_ID_LEN], veto[MAX_PATH];
    DEVINST node, eject;
    if (!instance_id || !*instance_id) return CR_NO_SUCH_DEVNODE;
    um_utf8_to_wide(instance_id, id, MAX_DEVICE_ID_LEN);
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
        !usb_node(node, &eject) || !eject) return CR_NO_SUCH_DEVNODE;
    return CM_Request_Device_EjectW(eject, NULL, veto, MAX_PATH, 0);
}
