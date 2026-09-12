/* um_enum.c — 设备证据采集层实现。 */

#include "um_enum.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#  include <windows.h>
#  include <winioctl.h>   /* STORAGE_* / IOCTL_* / VOLUME_DISK_EXTENTS */
#  include <setupapi.h>
#  include <cfgmgr32.h>   /* CM_Locate_DevNodeW / CM_Get_Parent */
#else
#  include "win32_shim.h"
#endif

#ifdef _WIN32
/* 接口类 GUID（Microsoft Learn 文档值，自 Win2000 起稳定）。本地定义而非
 * 引头文件：GUID_DEVINTERFACE_HID 在 hidclass.h、_USB_HUB 在 usbiodef.h、
 * _NET 在 netiodef.h，且 DEFINE_GUID 依赖 INITGUID/链接序，各工具链行为
 * 不一；直接写字面值零依赖、可移植，也不必再链 -luuid。 */
static const GUID UM_GUID_DEVINTERFACE_DISK =
  {0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
static const GUID UM_GUID_DEVINTERFACE_HID =
  {0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
static const GUID UM_GUID_DEVINTERFACE_NET =
  {0xcac88484,0x7515,0x4c03,{0x82,0xe6,0x71,0xa8,0x7a,0xb8,0x62,0x9b}};
static const GUID UM_GUID_DEVINTERFACE_USB_HUB =
  {0xf18a0e88,0xc30c,0x11d0,{0x88,0x15,0x00,0xa0,0xc9,0x08,0xbe,0xdc}};
/* STORAGE_BUS_TYPE 枚举值（ntddstor.h：BusTypeUnknown=0、BusTypeUsb=7）。
 * Linux 侧 win32_shim.h 定义同名宏且数值一致，这里补齐 Windows 侧并
 * 双向 #ifndef 防重复定义。 */
#ifndef BUS_TYPE_UNKNOWN
#define BUS_TYPE_UNKNOWN 0
#endif
#ifndef BUS_TYPE_USB
#define BUS_TYPE_USB    7
#endif
#ifndef CM_LOCATE_DEVNODE_NORMAL
#define CM_LOCATE_DEVNODE_NORMAL 0x00000000UL
#endif
#endif

/* ------------------------------------------------------------- 结构体布局 --
 * 与 Win32 保持一致；Linux 侧由 shim 按同一布局填值。                    */

#ifndef _WIN32
/* 布局必须与 Win32 完全一致：Windows 的 DWORD 是 32 位，而 Linux LP64 下
 * unsigned long 是 64 位——用错会同时错掉结构体大小和 BusType 的偏移
 * （实测 sizeof 会从 40 变成 80，读取位置也偏）。故一律用 uint32_t。 */
#include <stdint.h>
typedef struct { int64_t QuadPart; } UM_LARGE_INTEGER;
typedef struct { uint32_t DiskNumber; UM_LARGE_INTEGER StartingOffset;
                 UM_LARGE_INTEGER ExtentLength; } UM_DISK_EXTENT;
typedef struct { uint32_t NumberOfDiskExtents; UM_DISK_EXTENT Extents[1]; }
        UM_VDE;
typedef struct {
    uint32_t Version, Size;
    uint8_t  DeviceType, DeviceTypeModifier, RemovableMedia, CommandQueueing;
    uint32_t VendorIdOffset, ProductIdOffset, ProductRevisionOffset,
             SerialNumberOffset;
    uint32_t BusType;                    /* 偏移 28，与 STORAGE_DEVICE_DESCRIPTOR 同 */
    uint32_t RawPropertiesLength;
    uint8_t  RawDeviceProperties[1];
} UM_SDD;
#endif

/* 归并键/字段的有界拷贝（避开 snprintf("%s") 在 -O2 -Wformat-truncation
 * 下的告警；同长度字段拷贝本就不可能截断）。 */
static void um_copy_bounded(char *dst, size_t n, const char *src)
{
    size_t l;
    if (!dst || n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

/* SDD 的最小可用长度：能读到 BusType(28..31) 与 RawPropertiesLength(32..35)
 * 即 36 字节（驱动在无 RawProperties 时可只返回 36；sizeof=40 会误拒）。 */
#define UM_SDD_MIN 36

/* ------------------------------------------------------------ 归并键提取 -- */

void um_enum_instance_id(const char *device_path, char *out, size_t n)
{
    const char *slash, *hash;
    size_t cut;

    if (!out || n == 0) return;
    out[0] = '\0';
    if (!device_path) return;

    /* 取最后一个 '\' 之前的部分：去掉接口后缀（disk / volume / hid …） */
    slash = strrchr(device_path, '\\');
    hash  = strrchr(device_path, '#');
    /* 注意：必须算"偏移量"，不能把指针直接转 size_t（原来是这么错的，
     * 结果 cut 变成一个巨大值，被 strlen 兜底 → 等于没截断） */
    if (slash && hash) cut = (size_t)((slash > hash ? slash : hash) - device_path);
    else if (slash)    cut = (size_t)(slash - device_path);
    else if (hash)     cut = (size_t)(hash  - device_path);
    else               cut = 0;
    if (cut == 0) cut = strlen(device_path);

    if (cut >= n) cut = n - 1;
    memcpy(out, device_path, cut);
    out[cut] = '\0';
}

/* -------------------------------------------------------------- 低级探测 -- */

int um_enum_volume_disk(const char *volume_path, int *disk_no)
{
#ifdef _WIN32
    wchar_t wpath[256];
    HANDLE  h;
    BYTE    buf[1024];
    DWORD   br = 0;
    VOLUME_DISK_EXTENTS *ext;

    if (!MultiByteToWideChar(CP_UTF8, 0, volume_path ? volume_path : "",
                             -1, wpath, 256))
        return -1;
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                         buf, sizeof buf, &br, NULL) || br < sizeof *ext) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    ext = (VOLUME_DISK_EXTENTS *)buf;
    if (ext->NumberOfDiskExtents < 1) return -1;
    if (disk_no) *disk_no = (int)ext->Extents[0].DiskNumber;
    return 0;
#else
    wchar_t wpath[256];
    void   *h;
    BYTE    buf[1024];
    DWORD   br = 0;
    UM_VDE *ext;
    int     k;

    for (k = 0; volume_path && volume_path[k] && k < 255; k++)
        wpath[k] = (wchar_t)(unsigned char)volume_path[k];
    wpath[k] = 0;

    h = CreateFileW(wpath, 0, 1 | 2 | 4, NULL, 3, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                         buf, sizeof buf, &br, NULL) || br < sizeof(UM_VDE)) {
        CloseHandleShim(h);
        return -1;
    }
    CloseHandleShim(h);
    ext = (UM_VDE *)buf;
    if (ext->NumberOfDiskExtents < 1) return -1;
    if (disk_no) *disk_no = (int)ext->Extents[0].DiskNumber;
    return 0;
#endif
}

int um_enum_device_disk(const char *device_path, int *disk_no)
{
#ifdef _WIN32
    wchar_t wpath[256];
    HANDLE  h;
    STORAGE_DEVICE_NUMBER sdn;
    DWORD   br = 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, device_path ? device_path : "",
                             -1, wpath, 256))
        return -1;
    h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(&sdn, 0, sizeof sdn);
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                         &sdn, sizeof sdn, &br, NULL) || br < sizeof sdn) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (disk_no) *disk_no = (int)sdn.DeviceNumber;
    return 0;
#else
    wchar_t wpath[256];
    void   *h;
    BYTE    buf[64];
    DWORD   br = 0;
    int     k;

    for (k = 0; device_path && device_path[k] && k < 255; k++)
        wpath[k] = (wchar_t)(unsigned char)device_path[k];
    wpath[k] = 0;
    h = CreateFileW(wpath, 0, 1 | 2, NULL, 3, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                         buf, sizeof buf, &br, NULL) || br < 12) {
        CloseHandleShim(h);
        return -1;
    }
    CloseHandleShim(h);
    if (disk_no) memcpy(disk_no, (char *)buf + 4, sizeof(int));
    return 0;
#endif
}

int um_enum_disk_props(int disk_number, int *bus_type, int *removable)
{
#ifdef _WIN32
    wchar_t phys[64];
    HANDLE  h;
    BYTE    buf[1024];
    DWORD   br = 0;
    STORAGE_PROPERTY_QUERY q;
    STORAGE_DEVICE_DESCRIPTOR *d;

    _snwprintf_s(phys, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%d", disk_number);
    h = CreateFileW(phys, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         buf, sizeof buf, &br, NULL) || br < UM_SDD_MIN) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    if (bus_type)  *bus_type  = (int)d->BusType;
    if (removable) *removable = d->RemovableMedia ? 1 : 0;
    return 0;
#else
    wchar_t phys[64];
    void   *h;
    BYTE    buf[1024];
    DWORD   br = 0;
    UM_SDD *d;

    snprintf((char *)phys, sizeof phys, "\\\\.\\PhysicalDrive%d", disk_number);
    {
        char tmp[64];
        int  k;
        snprintf(tmp, sizeof tmp, "\\\\.\\PhysicalDrive%d", disk_number);
        for (k = 0; tmp[k]; k++) phys[k] = (wchar_t)(unsigned char)tmp[k];
        phys[k] = 0;
    }
    h = CreateFileW(phys, 0, 1 | 2, NULL, 3, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(buf, 0, sizeof buf);
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, NULL, 0,
                         buf, sizeof buf, &br, NULL) || br < UM_SDD_MIN) {
        CloseHandleShim(h);
        return -1;
    }
    CloseHandleShim(h);
    d = (UM_SDD *)buf;
    if (bus_type)  *bus_type  = (int)d->BusType;
    if (removable) *removable = d->RemovableMedia ? 1 : 0;
    return 0;
#endif
}

/* ------------------------------------------------------ USB 祖先判定 ------
 * 为什么需要：USB 智能笔的 HID 子节点实例 ID 是 HID\VID_xxxx，无线网卡是
 * {net-guid}\xxxx，它们**不以 USB\ 开头**。只靠前缀判断会把它们漏掉或误判。
 * 正确做法是沿父链向上找 USB 设备（CM_Get_Parent），这正是判断"挂在拓展坞
 * 下"和"这是 USB 网卡而非 PCIe 网卡"的依据。
 */
static int has_usb_ancestor(const char *instance_id)
{
#ifdef _WIN32
    DEVINST cur, parent;
    wchar_t wid[MAX_DEVICE_ID_LEN];
    wchar_t idw[MAX_DEVICE_ID_LEN];
    char    id[MAX_DEVICE_ID_LEN];
    int     hops;

    /* 由实例 ID 定位 devnode。原实现传 NULL 设备 ID 且无条件 return 0
     * （占位），真机上笔/网卡/拓展坧会全部判 UNKNOWN 被丢弃。 */
    if (!instance_id || !instance_id[0]) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, instance_id, -1, idw,
                            MAX_DEVICE_ID_LEN) == 0)
        return 0;
    if (CM_Locate_DevNodeW(&cur, idw, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return 0;

    for (hops = 0; hops < 16; hops++) {
        int k;
        if (CM_Get_Device_IDW(cur, wid, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            return 0;
        for (k = 0; wid[k] && k < (int)sizeof id - 1; k++)
            id[k] = (char)wid[k];
        id[k] = 0;
        if (strncmp(id, "USB\\", 4) == 0 ||
            strncmp(id, "USBSTOR\\", 8) == 0 ||
            strncmp(id, "USBHUB\\", 7) == 0)
            return 1;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) return 0;
        if (parent == cur) return 0;
        cur = parent;
    }
    return 0;
#else
    DEVINST cur, parent;
    wchar_t wid[MAX_DEVICE_ID_LEN];
    char    id[MAX_DEVICE_ID_LEN];
    int     hops;

    cur = shim_devnode_of(instance_id);
    if (cur == 0) return 0;

    for (hops = 0; hops < 16; hops++) {
        int k;
        if (CM_Get_Device_IDW(cur, wid, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            return 0;
        for (k = 0; wid[k] && k < (int)sizeof id - 1; k++) id[k] = (char)wid[k];
        id[k] = 0;
        if (strncmp(id, "USB\\", 4) == 0 ||
            strncmp(id, "USBSTOR\\", 8) == 0 ||
            strncmp(id, "USBHUB\\", 7) == 0)
            return 1;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) return 0;
        if (parent == cur) return 0;
        cur = parent;
    }
    return 0;
#endif
}

/* ------------------------------------------------------ 接口枚举（5 类）-- */

typedef struct {
    char     inst[UM_EVID_ID_LEN];
    char     path[UM_EVID_PATH_LEN];
    unsigned tags;
    int      has_disk;
    int      disk_number;
} iface_rec;

static int add_or_merge(iface_rec *recs, int *n, int max,
                        const char *inst, const char *path, unsigned tag,
                        int disk_number)
{
    int i;

    for (i = 0; i < *n; i++) {
        if (strcmp(recs[i].inst, inst) == 0) {
            recs[i].tags |= tag;
            if (disk_number >= 0) { recs[i].has_disk = 1; recs[i].disk_number = disk_number; }
            return i;
        }
    }
    if (*n >= max) return -1;
    memset(&recs[*n], 0, sizeof recs[*n]);
    um_copy_bounded(recs[*n].inst, sizeof recs[*n].inst, inst);
    um_copy_bounded(recs[*n].path, sizeof recs[*n].path, path);
    recs[*n].tags = tag;
    recs[*n].disk_number = -1;
    if (disk_number >= 0) { recs[*n].has_disk = 1; recs[*n].disk_number = disk_number; }
    return (*n)++;
}

#ifdef _WIN32
static void enum_iface(iface_rec *recs, int *n, int max, const GUID *g, unsigned tag)
{
    /* 全部用 W 显式版本：原来裸写 SetupDiGetDeviceInterfaceDetail，在未定义
     * UNICODE 时展开成 A 版，与 PSP_DEVICE_INTERFACE_DETAIL_DATA_W 类型
     * 不兼容（cbSize 也不对，运行必 ERROR_INVALID_USER_BUFFER）。 */
    HDEVINFO set = SetupDiGetClassDevsW(g, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;

    if (set == INVALID_HANDLE_VALUE) return;
    for (i = 0; ; i++) {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = NULL;
        SP_DEVINFO_DATA did;
        DWORD need = 0;
        char path8[UM_EVID_PATH_LEN];
        char inst8[UM_EVID_ID_LEN];
        wchar_t winst[MAX_DEVICE_ID_LEN];

        memset(&ifd, 0, sizeof ifd);
        ifd.cbSize = sizeof ifd;
        if (!SetupDiEnumDeviceInterfaces(set, NULL, g, i, &ifd)) break;

        path8[0] = '\0';
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
        if (need) {
            det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
            if (!det) continue;
            memset(&did, 0, sizeof did);
            did.cbSize = sizeof did;
            det->cbSize = sizeof *det;
            if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, NULL,
                                                 &did)) {
                WideCharToMultiByte(CP_UTF8, 0, det->DevicePath, -1,
                                    path8, (int)sizeof path8, NULL, NULL);
                /* 真正的设备实例 ID（USBSTOR\DISK&VEN_... / HID\VID_...）：
                 * 接口路径是 \\?\usbstor#...#{guid} 形态，从它截断既不可靠
                 * 也不能用于 CM_Locate_DevNodeW。这里从 detail 拿到的
                 * SP_DEVINFO_DATA 反查实例 ID，失败再退回路径截断。 */
                if (SetupDiGetDeviceInstanceIdW(set, &did, winst,
                                                MAX_DEVICE_ID_LEN, NULL)) {
                    WideCharToMultiByte(CP_UTF8, 0, winst, -1,
                                        inst8, (int)sizeof inst8, NULL, NULL);
                } else {
                    um_enum_instance_id(path8, inst8, sizeof inst8);
                }
                add_or_merge(recs, n, max, inst8, path8, tag, -1);
            }
            free(det);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}
#else
static void enum_iface(iface_rec *recs, int *n, int max, int code, unsigned tag)
{
    HDEVINFO set = SetupDiGetClassDevsW((const void *)(ULONG_PTR)code, NULL, NULL, 0);
    SP_DEVICE_INTERFACE_DATA_SHIM ifd;
    DWORD i;

    if (!set) return;
    for (i = 0; ; i++) {
        char detail[512];

        memset(&ifd, 0, sizeof ifd);
        ifd.cbSize = sizeof ifd;
        if (!SetupDiEnumDeviceInterfaces(set, NULL,
                                         (const void *)(ULONG_PTR)code, i, &ifd))
            break;
        memset(detail, 0, sizeof detail);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof detail,
                                             NULL, NULL)) {
            const wchar_t *wp = (const wchar_t *)(detail + 8);
            char utf8[UM_EVID_PATH_LEN];
            char inst[UM_EVID_ID_LEN];
            int k;
            for (k = 0; wp[k] && k < (int)sizeof utf8 - 1; k++)
                utf8[k] = (char)wp[k];
            utf8[k] = 0;
            /* shim 世界用 devnode 风格路径（USB\VID_...\SERIAL\disk），
             * 截断后缀即实例 ID；与 Windows 分支的 SetupDiGetDeviceInstanceIdW
             * 等价。 */
            um_enum_instance_id(utf8, inst, sizeof inst);
            add_or_merge(recs, n, max, inst, utf8, tag, -1);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}
#endif

/* ------------------------------------------------ 卷枚举（含无盘符的卷）-- */

static void collect_volumes(void *vols_out, int *nv, int max)
{
    /* 卷记录：path / disk / letters */
    struct volrec { char path[UM_EVID_PATH_LEN]; int disk; char letters[8][4]; int n; };
    struct volrec *vols = (struct volrec *)vols_out;

#ifdef _WIN32
    wchar_t vol[MAX_PATH];
    HANDLE  h = FindFirstVolumeW(vol, MAX_PATH);

    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char   utf8[UM_EVID_PATH_LEN];
        int    disk_no = -1;
        struct volrec *v;
        wchar_t names[256];
        DWORD   used = 0;

        WideCharToMultiByte(CP_UTF8, 0, vol, -1, utf8, (int)sizeof utf8, NULL, NULL);
        if (um_enum_volume_disk(utf8, &disk_no) != 0) continue;
        if (*nv >= max) break;
        v = &vols[*nv];
        memset(v, 0, sizeof *v);
        um_copy_bounded(v->path, sizeof v->path, utf8);
        v->disk = disk_no;
        memset(names, 0, sizeof names);
        if (GetVolumePathNamesForVolumeNameW(vol, names, 256, &used)) {
            int o = 0;
            while (o < 256 && names[o] && v->n < 8) {
                /* 只把“盘符路径”（正好 3 字符：字母+:+\）当作盘符；
                 * 更长的是挂载点（如 C:\mnt\usb），取首字符会变成假盘符。 */
                if (names[o + 1] == L':' && names[o + 2] == L'\\' &&
                    names[o + 3] == L'\0') {
                    snprintf(v->letters[v->n++], 4, "%c:", (char)names[o]);
                }
                while (o < 256 && names[o]) o++;
                o++;
            }
        }
        (void)used;
        (*nv)++;
    } while (FindNextVolumeW(h, vol, MAX_PATH));
    FindVolumeClose(h);
#else
    wchar_t vol[MAX_PATH];
    void   *h = FindFirstVolumeW(vol, MAX_PATH);

    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char   utf8[UM_EVID_PATH_LEN];
        int    disk_no = -1;
        struct volrec *v;
        wchar_t names[256];

        {
            int k;
            for (k = 0; vol[k] && k < (int)sizeof utf8 - 1; k++) utf8[k] = (char)vol[k];
            utf8[k] = 0;
        }
        if (um_enum_volume_disk(utf8, &disk_no) != 0) continue;
        if (*nv >= max) break;
        v = &vols[*nv];
        memset(v, 0, sizeof *v);
        um_copy_bounded(v->path, sizeof v->path, utf8);
        v->disk = disk_no;
        memset(names, 0, sizeof names);
        if (GetVolumePathNamesForVolumeNameW(vol, names, 256, NULL)) {
            int o = 0;
            while (o < 256 && names[o] && v->n < 8) {
                if (names[o + 1] == L':' && names[o + 2] == L'\\' &&
                    names[o + 3] == L'\0') {
                    snprintf(v->letters[v->n++], 4, "%c:", (char)names[o]);
                }
                while (o < 256 && names[o]) o++;
                o++;
            }
        }
        (*nv)++;
    } while (FindNextVolumeW(h, vol, MAX_PATH));
    FindVolumeClose(h);
#endif
}

int um_enum_collect(um_evidence *out, int max)
{
    iface_rec recs[64];
    /* 卷侧记录：卷路径 + 所属物理磁盘 + 盘符 */
    struct { char path[UM_EVID_PATH_LEN]; int disk; char letters[8][4]; int n; }
        vols[64];
    int n = 0, nv = 0, i, j;

    if (!out || max <= 0) return -1;

    /* ---- 1) 功能接口 → 标签（同一设备多接口按 instance_id 合并） ---- */
#ifdef _WIN32
    enum_iface(recs, &n, 64, &UM_GUID_DEVINTERFACE_DISK,     UM_TAG_STORAGE);
    enum_iface(recs, &n, 64, &UM_GUID_DEVINTERFACE_HID,      UM_TAG_HID);
    enum_iface(recs, &n, 64, &UM_GUID_DEVINTERFACE_NET,      UM_TAG_NET);
    enum_iface(recs, &n, 64, &UM_GUID_DEVINTERFACE_USB_HUB,  UM_TAG_HUB);
#else
    enum_iface(recs, &n, 64, IF_DISK, UM_TAG_STORAGE);
    enum_iface(recs, &n, 64, IF_HID,  UM_TAG_HID);
    enum_iface(recs, &n, 64, IF_NET,  UM_TAG_NET);
    enum_iface(recs, &n, 64, IF_HUB,  UM_TAG_HUB);
#endif

    /* ---- 2) 磁盘接口 → 物理磁盘号 ----
     * 这一步必须由 IOCTL 决定，不能从 device_path 字符串猜。 */
    for (i = 0; i < n; i++) {
        int dn = -1;
        if ((recs[i].tags & UM_TAG_STORAGE) &&
            um_enum_device_disk(recs[i].path, &dn) == 0) {
            recs[i].has_disk = 1;
            recs[i].disk_number = dn;
        }
    }

    /* ---- 3) 所有卷（含无盘符）→ 物理磁盘号 + 盘符 ----
     * 这是 main 分支 scan_win32.c 缺失的关键：它从 GetLogicalDrives() 出发，
     * 只看得到已分配盘符的卷，因此无盘符存储完全不可见。 */
    collect_volumes(vols, &nv, 64);

    /* ---- 4) 按物理磁盘号把卷挂到设备上（多分区聚合） ---- */
    /* CRITICAL：n 可能达到 64（接口数），而调用方只给了 max 条空间。
     * 原来直接 return n，调用方会越界读 out[max..n]（普通桌面键鼠就是
     * 十几个 HID 接口，真机必触）。装夹后再进入循环 5。 */
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        um_evidence *e = &out[i];
        int bus = BUS_TYPE_UNKNOWN, rem = 0;
        int has_letter = 0;

        memset(e, 0, sizeof *e);
        um_copy_bounded(e->instance_id, sizeof e->instance_id, recs[i].inst);
        um_copy_bounded(e->device_path, sizeof e->device_path, recs[i].path);
        e->tags = recs[i].tags;
        e->disk_number = recs[i].disk_number;

        /* USB 身份：自身前缀 或 父链上有 USB（网卡/笔/拓展坞常靠后者） */
        if (strncmp(recs[i].inst, "USB\\", 4) == 0 ||
            strncmp(recs[i].inst, "USBSTOR\\", 8) == 0 ||
            has_usb_ancestor(recs[i].inst))
            e->tags |= UM_TAG_USB;

        if (recs[i].has_disk && recs[i].disk_number >= 0) {
            if (um_enum_disk_props(recs[i].disk_number, &bus, &rem) == 0) {
                e->bus_is_usb = (bus == BUS_TYPE_USB);
                e->removable  = rem;
            }
            /* 收集该磁盘上的所有盘符 */
            for (j = 0; j < nv; j++) {
                int k;
                if (vols[j].disk != recs[i].disk_number) continue;
                for (k = 0; k < vols[j].n && e->n_letters < 8; k++)
                    snprintf(e->letters[e->n_letters++], 4, "%s", vols[j].letters[k]);
            }
        }
        has_letter = (e->n_letters > 0);

        /* 介质与解锁：能读到盘符即视为就绪并解锁；无盘符时保守不主动访问
         * （调研建议：不要访问可能弹错误框的路径）。 */
        e->media_present = recs[i].has_disk ? 1 : 0;
        e->unlocked      = 1;

        e->kind = um_ui_classify(e->tags, has_letter, e->bus_is_usb,
                                 e->removable, e->media_present, e->unlocked);
    }

    /* ---- 5) 没有匹配磁盘接口的卷（裸卷）也别丢 ---- */
    for (j = 0; j < nv && n < max; j++) {
        um_evidence *e;
        int owned = 0, k;
        for (k = 0; k < n; k++)
            if (out[k].disk_number >= 0 && out[k].disk_number == vols[j].disk)
                { owned = 1; break; }
        if (owned) continue;
        e = &out[n];
        memset(e, 0, sizeof *e);
        um_enum_instance_id(vols[j].path, e->instance_id, sizeof e->instance_id);
        um_copy_bounded(e->device_path, sizeof e->device_path, vols[j].path);
        e->tags = UM_TAG_VOLUME;
        e->disk_number = vols[j].disk;
        e->media_present = 1;
        e->unlocked = 1;
        for (k = 0; k < vols[j].n && e->n_letters < 8; k++)
            snprintf(e->letters[e->n_letters++], 4, "%s", vols[j].letters[k]);
        e->kind = um_ui_classify(e->tags, e->n_letters > 0, 0, 0, 1, 1);
        n++;
    }
    return n;
}

/* ------------------------------------------------------ 证据 → UI 模型 ---- */

int um_enum_to_model(const um_evidence *evs, int n, um_toast_model *out)
{
    int i, rows = 0;
    int n_dev = 0, n_vol = 0;

    if (!evs || !out) return 0;
    memset(out, 0, sizeof *out);

    for (i = 0; i < n && rows < UM_UI_MAX_ROWS; i++) {
        const um_evidence *e = &evs[i];
        um_volume *v;
        char letters[UM_UI_MAX_LETTER * 4];
        int k;

        /* 证据不足的设备（虚拟光驱、未知 USB）不进 UI，避免噪音 */
        if (e->kind == UM_KIND_UNKNOWN) continue;

        v = &out->rows[rows];
        memset(v, 0, sizeof *v);
        v->tags = e->tags;
        v->kind = e->kind;
        v->disk_number = e->disk_number;
        v->removable = e->removable;
        v->openable = um_ui_can_open(e->kind);
        v->ejectable = um_ui_can_eject(e->kind, e->removable);
        v->pct = -1;
        v->n_letters = e->n_letters;
        for (k = 0; k < e->n_letters && k < UM_UI_MAX_LETTER; k++)
            um_copy_bounded(v->letters[k], sizeof v->letters[k], e->letters[k]);

        /* 标题：优先盘符列表，无盘符则用类别说明 */
        letters[0] = '\0';
        for (k = 0; k < e->n_letters; k++) {
            if (k) strncat(letters, "、", sizeof letters - strlen(letters) - 1);
            strncat(letters, e->letters[k], sizeof letters - strlen(letters) - 1);
        }
        {
            const char *nm;
            switch (e->kind) {
            case UM_KIND_USB_STORAGE:       nm = "可移动磁盘"; break;
            case UM_KIND_STORAGE_NO_LETTER: nm = "USB 存储设备"; break;
            case UM_KIND_EMPTY_SLOT:        nm = "读卡器"; break;
            case UM_KIND_LOCKED_VOLUME:     nm = "已锁定卷"; break;
            case UM_KIND_DOCK_HUB:          nm = "USB 拓展坞 / 集线器"; break;
            case UM_KIND_NET_ADAPTER:       nm = "USB 网络设备"; break;
            case UM_KIND_HID_PEN:           nm = "USB HID 设备"; break;
            default:                        nm = "USB 设备"; break;
            }
            /* 有盘符时以盘符为主标识，否则用类别名（比实例 ID 片段可读） */
            if (e->n_letters)
                snprintf(v->title, sizeof v->title, "%s %s", letters, nm);
            else
                snprintf(v->title, sizeof v->title, "%s", nm);
        }

        /* 副标题：多分区显示"N 个分区"，无盘符给具体提示 */
        if (e->n_letters > 1)
            snprintf(v->subtitle, sizeof v->subtitle, "可移动磁盘 · %s（%d 个分区）",
                     letters, e->n_letters);
        else if (e->n_letters == 1)
            snprintf(v->subtitle, sizeof v->subtitle, "可移动磁盘 · %s", letters);
        else
            um_ui_device_notice(e->kind, v->subtitle, sizeof v->subtitle);

        um_ui_capacity_line(v, v->capacity, sizeof v->capacity);

        n_dev++;
        n_vol += (e->n_letters > 0 ? e->n_letters : 0);
        rows++;
    }

    out->n_rows = rows;
    snprintf(out->headline, sizeof out->headline, "%s", "USB 设备监控");
    snprintf(out->subtitle, sizeof out->subtitle, "%d 个设备 · %d 个卷", n_dev, n_vol);
    snprintf(out->count, sizeof out->count, "%d 个", n_dev);
    out->accent_kind = 1;
    return rows;
}
