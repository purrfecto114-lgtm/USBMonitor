/* um_enum.h — 设备证据采集层（usbmon，C99）
 *
 * 这一层回答一个问题：**这台机器上到底插了什么 USB 设备**，并把每条证据
 * 填成 um_ui_classify() 能直接吃的形状。
 *
 * 为什么需要它：
 *   main 分支的 scan_win32.c 从 GetLogicalDrives() 出发枚举盘符，因此
 *   **看不见没有盘符的卷**（未分配盘符、仅挂载到目录、RAW/离线），也
 *   **完全没有 HID / NET / HUB 的概念**——拓展坞、无线网卡、智能笔一律
 *   不可见。1.1.1 对此的兜底是：收到设备事件但解析不出盘符时，猜一句
 *   "该设备可能尚未分配盘符"（app.py:1494 generic_remove）。
 *
 * 本层用两套互补的证据取代那个猜测：
 *   1. SetupAPI 按接口类枚举（DISK / VOLUME / HID / NET / HUB）→ 功能标签
 *   2. FindFirstVolume 枚举**所有**卷（含无盘符）→ 卷与物理磁盘的归属
 * 再由物理磁盘号聚合，交给 um_ui_classify() 判定。
 *
 * 与 GUI 层一样：这里是纯 C99，Windows 上链接真实 SetupAPI / cfgmgr32，
 * Linux 上由 tests/win32_shim.c 提供同名实现，因此**采集逻辑本身可以在
 * Linux 上端到端测试**（见 tests/enum_test.c）。
 */
#ifndef UM_ENUM_H
#define UM_ENUM_H

#include "um_toast_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM_EVID_MAX       16
#define UM_EVID_ID_LEN    128
#define UM_EVID_PATH_LEN  192

typedef struct {
    char        instance_id[UM_EVID_ID_LEN];   /* 归并键（同一物理设备） */
    char        device_path[UM_EVID_PATH_LEN]; /* 代表性接口路径 */
    unsigned    tags;            /* UM_TAG_* 位掩码                     */
    int         disk_number;     /* 物理磁盘号，-1 = 非存储/未知        */
    int         bus_is_usb;      /* BusType == BusTypeUsb               */
    int         removable;       /* RemovableMedia 或 CM_DEVCAP_REMOVABLE */
    int         media_present;   /* 介质就绪（读卡器空槽 = 0）          */
    int         unlocked;        /* 卷可访问（BitLocker 已解锁）        */
    int         n_letters;       /* 0 = 未分配盘符                      */
    char        letters[8][4];   /* "E:" "F:" "G:"                      */
    um_dev_kind kind;            /* 判定结果                            */
} um_evidence;

/* 采集全部 USB 设备证据。返回写入 out 的条数（<0 表示失败）。 */
int um_enum_collect(um_evidence *out, int max);

/* 从接口路径提取"设备标识"用于归并：
 *   "USB\VID_0781&PID_5583\SERIAL\disk"  → "USB\VID_0781&PID_5583\SERIAL"
 * 同一物理设备的多个功能接口共享这个前缀。纯函数，便于测试。 */
void um_enum_instance_id(const char *device_path, char *out, size_t n);

/* 把采集到的证据转成 toast 可直接显示的模型。
 * 非存储设备（网卡/笔/拓展坞）也会出现，但根据 kind 决定是否显示
 * "打开"/"安全弹出"。返回写入的行数。 */
int um_enum_to_model(const um_evidence *evs, int n, um_toast_model *out);

/* ------------------------------------------------------------------ 内部 --
 * 以下函数暴露出来是为了单元测试（Windows / shim 两条实现路径）。      */

/* 卷 → 物理磁盘号（IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）；成功返回 0。 */
int um_enum_volume_disk(const char *volume_path, int *disk_no);
/* 磁盘接口设备 → 物理磁盘号（IOCTL_STORAGE_GET_DEVICE_NUMBER）；成功返回 0。 */
int um_enum_device_disk(const char *device_path, int *disk_no);
/* 物理磁盘 → bus type / removable；成功返回 0。 */
int um_enum_disk_props(int disk_number, int *bus_type, int *removable);

#ifdef __cplusplus
}
#endif

#endif /* UM_ENUM_H */
