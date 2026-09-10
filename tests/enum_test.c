/* enum_test.c — 采集层端到端测试（在 Linux 上驱动 src/um_enum.c 生产代码）
 *
 * 用 tests/win32_shim.c 提供的假"设备世界"，让真实的采集逻辑跑一遍，
 * 断言它给出的证据与判定。
 *
 * 覆盖的场景正是 main 分支 scan_win32.c 的盲区：
 *   - 多分区 U 盘（3 个盘符 → 1 台设备）
 *   - 未分配盘符的 USB 存储（GetLogicalDrives 完全看不到）
 *   - 智能笔 / HID
 *   - USB 无线网卡
 *   - 拓展坞 / 集线器
 *   - 虚拟光驱（非 USB 总线，必须被排除）
 */
#include "um_enum.h"
#include "win32_shim.h"

#include <stdio.h>
#include <string.h>

static int g_fails, g_checks;

#define CHECK(cond) do {                                              \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
        g_fails++;                                                    \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    } else {                                                          \
        printf("  ok   %s\n", #cond);                                 \
    }                                                                 \
} while (0)

/* 3 分区 U 盘（disk 2）+ 无盘符存储（disk 5）+ 智能笔 + 网卡 + 拓展坞
 * 注意：同一物理设备的多个接口共享 instance 前缀（去掉最后一段） */
static const shim_dev k_world[] = {
    /* --- Kingston 3 分区 U 盘：1 个 disk 接口 + 3 个卷 --- */
    { "USB\\VID_0951&PID_1666\\001B1C",
      "USB\\VID_0951&PID_1666\\001B1C\\disk",
      IF_DISK,   2, BUS_TYPE_USB, 1, 1, 1, "", NULL },
    { "USB\\VID_0951&PID_1666\\001B1C\\volume", "\\\\?\\Volume{k-3p-e}",
      IF_VOLUME, 2, BUS_TYPE_USB, 1, 1, 1, "E:", NULL },
    { "USB\\VID_0951&PID_1666\\001B1C\\volume", "\\\\?\\Volume{k-3p-f}",
      IF_VOLUME, 2, BUS_TYPE_USB, 1, 1, 1, "F:", NULL },
    { "USB\\VID_0951&PID_1666\\001B1C\\volume", "\\\\?\\Volume{k-3p-g}",
      IF_VOLUME, 2, BUS_TYPE_USB, 1, 1, 1, "G:", NULL },

    /* --- 未分配盘符的 USB 存储（main 的盲区） --- */
    { "USB\\VID_0781&PID_5583\\4C53",
      "USB\\VID_0781&PID_5583\\4C53\\disk",
      IF_DISK,   5, BUS_TYPE_USB, 1, 1, 1, "", NULL },
    { "USB\\VID_0781&PID_5583\\4C53\\volume", "\\\\?\\Volume{no-letter}",
      IF_VOLUME, 5, BUS_TYPE_USB, 1, 1, 1, "", NULL },

    /* --- Wacom 智能笔：HID 子节点，父节点是 USB 设备。
     *     父节点 iface=0（不可枚举），只用于 CM_Get_Parent 反查。 --- */
    { "USB\\VID_056A&PID_0376\\PEN", "USB\\VID_056A&PID_0376\\PEN",
      0, -1, BUS_TYPE_UNKNOWN, 0, 0, 1, "", NULL },
    { "HID\\VID_056A&PID_0376\\PEN", "HID\\VID_056A&PID_0376\\PEN\\hid",
      IF_HID, -1, BUS_TYPE_UNKNOWN, 0, 0, 1, "", "USB\\VID_056A&PID_0376\\PEN" },

    /* --- USB 无线网卡 --- */
    { "USB\\VID_0BDA&PID_8179\\WIFI", "USB\\VID_0BDA&PID_8179\\WIFI\\net",
      IF_NET, -1, BUS_TYPE_UNKNOWN, 0, 0, 1, "", NULL },

    /* --- 拓展坞 / 集线器 --- */
    { "USB\\VID_17EF&PID_3062\\DOCK", "USB\\VID_17EF&PID_3062\\DOCK\\hub",
      IF_HUB, -1, BUS_TYPE_UNKNOWN, 0, 0, 1, "", NULL },
};

static um_evidence *find_by_disk(um_evidence *evs, int n, int disk)
{
    int i;
    for (i = 0; i < n; i++)
        if (evs[i].disk_number == disk) return &evs[i];
    return NULL;
}

static um_evidence *find_by_kind(um_evidence *evs, int n, um_dev_kind k)
{
    int i;
    for (i = 0; i < n; i++)
        if (evs[i].kind == k) return &evs[i];
    return NULL;
}

int main(void)
{
    um_evidence evs[UM_EVID_MAX];
    int n;

    shim_set_world(k_world, (int)(sizeof k_world / sizeof k_world[0]));
    n = um_enum_collect(evs, UM_EVID_MAX);

    printf("== 1. 采集到 %d 台设备 ==\n", n);
    CHECK(n == 5);   /* 3分区U盘(1) + 无盘符(1) + 笔 + 网卡 + 拓展坞 */

    printf("== 2. 多分区 U 盘：3 个盘符聚合成 1 台（不是 3 台）==\n");
    {
        um_evidence *e = find_by_disk(evs, n, 2);
        CHECK(e != NULL);
        if (e) {
            CHECK(e->n_letters == 3);
            CHECK(strcmp(e->letters[0], "E:") == 0);
            CHECK(strcmp(e->letters[1], "F:") == 0);
            CHECK(strcmp(e->letters[2], "G:") == 0);
            CHECK(e->kind == UM_KIND_USB_STORAGE);
            CHECK(e->bus_is_usb == 1);
            CHECK(e->removable == 1);
            CHECK(um_ui_can_eject(e->kind, e->removable) == 1);
        }
    }

    printf("== 3. 未分配盘符的 USB 存储（main 扫描不到，这里要能看见）==\n");
    {
        um_evidence *e = find_by_disk(evs, n, 5);
        CHECK(e != NULL);
        if (e) {
            CHECK(e->n_letters == 0);
            CHECK(e->kind == UM_KIND_STORAGE_NO_LETTER);
            CHECK(e->bus_is_usb == 1);
            /* 无盘符不能"打开"，但可以安全弹出 */
            CHECK(um_ui_can_open(e->kind) == 0);
            CHECK(um_ui_can_eject(e->kind, e->removable) == 1);
        }
    }

    printf("== 4. 智能笔：识别为 HID，不是未分配盘符的 U 盘 ==\n");
    {
        um_evidence *e = find_by_kind(evs, n, UM_KIND_HID_PEN);
        CHECK(e != NULL);
        if (e) {
            CHECK((e->tags & UM_TAG_HID) != 0);
            CHECK((e->tags & UM_TAG_STORAGE) == 0);
            CHECK(um_ui_can_open(e->kind) == 0);
            CHECK(um_ui_can_eject(e->kind, e->removable) == 0);
        }
    }

    printf("== 5. USB 无线网卡 ==\n");
    {
        um_evidence *e = find_by_kind(evs, n, UM_KIND_NET_ADAPTER);
        CHECK(e != NULL);
        if (e) {
            CHECK((e->tags & UM_TAG_NET) != 0);
            CHECK(um_ui_can_open(e->kind) == 0);
            CHECK(um_ui_can_eject(e->kind, e->removable) == 0);
        }
    }

    printf("== 6. 拓展坞 / 集线器 ==\n");
    {
        um_evidence *e = find_by_kind(evs, n, UM_KIND_DOCK_HUB);
        CHECK(e != NULL);
        if (e) {
            CHECK((e->tags & UM_TAG_HUB) != 0);
            CHECK(um_ui_can_eject(e->kind, e->removable) == 0);
        }
    }

    printf("== 7. 虚拟光驱（非 USB 总线）必须被排除，不能算 USB 存储 ==\n");
    {
        const shim_dev cdrom[] = {
            { "SCSI\\CDROM&VEN_VIRTUAL\\dvd", "SCSI\\CDROM&VEN_VIRTUAL\\dvd\\disk",
              IF_DISK, 9, BUS_TYPE_SATA, 0, 1, 1, "D:", NULL },
            { "SCSI\\CDROM&VEN_VIRTUAL\\volume", "\\\\?\\Volume{cdrom}",
              IF_VOLUME, 9, BUS_TYPE_SATA, 0, 1, 1, "D:", NULL },
        };
        int m;
        shim_set_world(cdrom, 2);
        m = um_enum_collect(evs, UM_EVID_MAX);
        CHECK(m >= 1);
        if (m >= 1) {
            CHECK(evs[0].kind != UM_KIND_USB_STORAGE);
            CHECK(um_ui_can_eject(evs[0].kind, evs[0].removable) == 0);
        }
    }

    printf("== 8. 归并键提取：同一设备不同接口共享 instance ==\n");
    {
        char inst[128];
        um_enum_instance_id("USB\\VID_0951&PID_1666\\001B1C\\disk", inst, sizeof inst);
        CHECK(strcmp(inst, "USB\\VID_0951&PID_1666\\001B1C") == 0);
        um_enum_instance_id("USB\\VID_0951&PID_1666\\001B1C\\volume", inst, sizeof inst);
        CHECK(strcmp(inst, "USB\\VID_0951&PID_1666\\001B1C") == 0);
        um_enum_instance_id("", inst, sizeof inst);
        CHECK(inst[0] == '\0');
        um_enum_instance_id("no-separator", inst, sizeof inst);
        CHECK(strcmp(inst, "no-separator") == 0);
    }

    printf("== 9. 证据 → UI 模型（端到端打通）==\n");
    {
        um_evidence e2[UM_EVID_MAX];
        um_toast_model m;
        int cnt;
        shim_set_world(k_world, (int)(sizeof k_world / sizeof k_world[0]));
        cnt = um_enum_collect(e2, UM_EVID_MAX);
        {
            int rows = um_enum_to_model(e2, cnt, &m);
            CHECK(rows == 5);            /* 虚拟光驱等 UNKNOWN 不进 UI */
            CHECK(m.n_rows == 5);
            /* 多分区那一行 */
            {
                int i, found = 0;
                for (i = 0; i < m.n_rows; i++) {
                    if (m.rows[i].n_letters == 3) {
                        found = 1;
                        CHECK(strstr(m.rows[i].subtitle, "3 个分区") != NULL);
                        CHECK(m.rows[i].openable == 1);
                        CHECK(m.rows[i].ejectable == 1);
                    }
                }
                CHECK(found);
            }
            /* 智能笔那一行：不可打开、不可弹出 */
            {
                int i, found = 0;
                for (i = 0; i < m.n_rows; i++) {
                    if (m.rows[i].kind == UM_KIND_HID_PEN) {
                        found = 1;
                        CHECK(m.rows[i].openable == 0);
                        CHECK(m.rows[i].ejectable == 0);
                        CHECK(strstr(m.rows[i].subtitle, "无存储") != NULL ||
                              strstr(m.rows[i].subtitle, "不支持打开") != NULL);
                    }
                }
                CHECK(found);
            }
            /* 无盘符那一行 */
            {
                int i, found = 0;
                for (i = 0; i < m.n_rows; i++) {
                    if (m.rows[i].kind == UM_KIND_STORAGE_NO_LETTER) {
                        found = 1;
                        CHECK(m.rows[i].openable == 0);
                        CHECK(m.rows[i].ejectable == 1);
                        CHECK(strstr(m.rows[i].subtitle, "未分配盘符") != NULL);
                    }
                }
                CHECK(found);
            }
            CHECK(strstr(m.subtitle, "个设备") != NULL);
        }
    }

    printf("== 10. 采集返回值必须装夹到 max（回归：20 个接口、max=16 越界读）==\n");
    {
        /* 普通桌面键鼠就是十几个 HID 接口：返回未装夹的原始计数会让
         * 调用方按 n 遍历 out[]，读出 out[16..19] 的越界内存。 */
        static shim_dev many[20];
        um_evidence evs[16];
        um_toast_model m;
        int i, n;

        for (i = 0; i < 20; i++) {
            static char paths[20][96];
            snprintf(paths[i], sizeof paths[i],
                     "USB\\VID_045E&PID_00%02d\\SER%03d\\hid", i, i);
            many[i].instance_id = paths[i];   /* shim 合并键由路径截断而来 */
            many[i].device_path = paths[i];
            many[i].iface = IF_HID;
            many[i].disk_number = -1;
            many[i].media_present = 1;
            many[i].letters = "";
        }
        shim_set_world(many, 20);

        n = um_enum_collect(evs, 16);
        CHECK(n >= 0 && n <= 16);            /* 核心断言：绝不超 max */
        if (n > 0) {
            /* 装夹后的 n 遍历也必须安全（ASan 下验证越界读） */
            int rows = um_enum_to_model(evs, n, &m);
            CHECK(rows >= 0 && rows <= UM_UI_MAX_ROWS);
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
