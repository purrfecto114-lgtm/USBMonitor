/* ui_test.c — 在 Linux 上驱动 src/um_toast_win32.c（真·生产代码）的交互测试。
 *
 * 通过 tests/win32_shim.h 提供的 Win32 仿真层，直接给窗口过程投递真实的
 * 消息（WM_PAINT / WM_LBUTTONUP / WM_RBUTTONUP / WM_KEYDOWN / WM_TIMER /
 * WM_MOUSEHOVER / WM_MOUSELEAVE），断言状态机与回调；同时把绘制指令导出
 * 为 SVG，供人工审阅视觉还原度。
 *
 * 章节 8-13 是 TDD 的「红」：先写断言，再实现 um_toast_ui.c 里的对应能力。
 */
#include "um_toast_win32.h"
#include "win32_shim.h"

#include <stdio.h>
#include <string.h>

#define TEST_HWND ((HWND)0x1234)

static int g_fails, g_checks;
static int g_last_act = -1, g_last_row = -99, g_n_acts;

#define CHECK(cond) do {                                              \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
        g_fails++;                                                    \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    } else {                                                          \
        printf("  ok   %s\n", #cond);                                 \
    }                                                                 \
} while (0)

static void on_action(um_toast_action a, int row, void *u)
{
    (void)u;
    g_last_act = (int)a;
    g_last_row = row;
    g_n_acts++;
}

static void fill_volume(um_volume *v, const char *path, const char *title,
                        const char *sub, unsigned long long total,
                        unsigned long long used, unsigned long long freeb)
{
    memset(v, 0, sizeof *v);
    snprintf(v->path, sizeof v->path, "%s", path);
    snprintf(v->title, sizeof v->title, "%s", title);
    snprintf(v->subtitle, sizeof v->subtitle, "%s", sub);
    v->total = total; v->used = used; v->free = freeb;
    v->pct = (total > 0) ? (int)((used * 100) / total) : -1;
    v->disk_number = 0;
    v->tags = UM_TAG_USB | UM_TAG_STORAGE | UM_TAG_VOLUME;
    v->kind = UM_KIND_USB_STORAGE;
    v->ejectable = 1;
    v->openable = 1;
    v->n_letters = 1;
    snprintf(v->letters[0], sizeof v->letters[0], "%.2s", path);
}

static void make_model(um_toast_model *m)
{
    memset(m, 0, sizeof *m);
    snprintf(m->headline, sizeof m->headline, "%s", "USB 设备监控");
    snprintf(m->subtitle, sizeof m->subtitle, "%s", "2 个设备 · 3 个卷");
    snprintf(m->summary, sizeof m->summary, "%s", "USB 已连接：E:、F:、G:");
    snprintf(m->count, sizeof m->count, "%s", "3 个");
    m->accent_kind = 1;                      /* ok / 绿 */
    fill_volume(&m->rows[0], "E:\\", "Kingston DataTraveler 3.0 (E:)",
                "可移动磁盘 · E:\\", 62668800000ULL, 42600000000ULL,
                20068800000ULL);
    fill_volume(&m->rows[1], "F:\\", "Kingston DataTraveler 3.0 (F:)",
                "可移动磁盘 · F:\\", 62668800000ULL, 12000000000ULL,
                50668800000ULL);
    fill_volume(&m->rows[2], "G:\\", "SanDisk Ultra (G:)",
                "可移动磁盘 · G:\\", 32000000000ULL, 8000000000ULL,
                24000000000ULL);
    m->n_rows = 3;
}

int main(void)
{
    um_toast_model m;
    um_theme       theme;
    um_toast_win  *w;

    printf("== 1. 主题解析（auto → 系统深色 / 浅色）==\n");
    um_theme_resolve(&theme, "auto", 1);
    CHECK(strcmp(theme.name, "dark") == 0 && theme.panel == 0x202630);
    um_theme_resolve(&theme, "light", 1);
    CHECK(strcmp(theme.name, "light") == 0 && theme.panel == 0xfbfcff);

    printf("== 2. 文案工具（对齐 core.py）==\n");
    {
        char b[64];
        um_ui_format_bytes(62668800000ULL, b, sizeof b);
        CHECK(strcmp(b, "58.4 GB") == 0);
        um_ui_countdown(9500, b, sizeof b);
        CHECK(strcmp(b, "10 秒后自动关闭") == 0);
        um_ui_countdown(0, b, sizeof b);
        CHECK(strcmp(b, "即将关闭") == 0);
        um_ui_countdown(120000, b, sizeof b);
        CHECK(strcmp(b, "2 分钟后自动关闭") == 0);
    }

    printf("== 3. 高度测量：折叠 230 / 展开随行数增长并夹到 430（1.1.1 同规则）==\n");
    make_model(&m);
    {
        um_toast_state s;
        um_toast_state_init(&s, 10000);
        s.expanded = 0;
        CHECK(um_toast_measure_height(&m, &s) == 230);
        s.expanded = 1;
        CHECK(um_toast_measure_height(&m, &s) == 334);     /* 3 行 */
        m.n_rows = 8;
        CHECK(um_toast_measure_height(&m, &s) == 430);     /* 夹到上限 */
        s.expanded = 0;
        CHECK(um_toast_measure_height(&m, &s) == 230);     /* 折叠恒定 */
        m.n_rows = 3;
    }

    printf("== 4. 命中测试：看得见就点得到 ==\n");
    {
        um_toast_state s;
        um_ui_hit h;
        um_toast_state_init(&s, 10000);
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230,
                              UM_UI_WIDTH - UM_UI_PAD - 59, 230 - UM_UI_PAD - 21);
        CHECK(h == UM_HIT_OPEN);
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230, 16 + 98 + 45,
                              230 - UM_UI_PAD - 21);
        CHECK(h == UM_HIT_CLOSE);
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230, 16 + 45,
                              230 - UM_UI_PAD - 21);
        CHECK(h == UM_HIT_EXPAND);
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230, 200, 100);
        CHECK(h == UM_HIT_ROW + 0);
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230, 5, 5);
        CHECK(h == UM_HIT_NONE);
    }

    printf("== 5. 倒计时状态机（tick / hover 暂停 / 恢复）==\n");
    {
        um_toast_state s;
        um_toast_state_init(&s, 10000);
        s.visible = 1;
        CHECK(um_toast_tick(&s, 3000) == 0 && s.remaining_ms == 7000);
        um_toast_pause(&s);
        CHECK(um_toast_tick(&s, 5000) == 0 && s.remaining_ms == 7000);
        um_toast_resume(&s);
        CHECK(um_toast_tick(&s, 7000) == 1);
    }

    printf("== 6. 通过真实窗口过程驱动交互（Win32 消息）==\n");
    um_theme_resolve(&theme, "dark", 1);
    shim_window(NULL, 440, 230);
    CHECK(um_toast_win_init(NULL) == 1);
    w = um_toast_win_new(&m, &theme, 1, on_action, NULL);
    CHECK(w != NULL);
    if (!w) { printf("fatal: no window\n"); return 1; }
    um_toast_win_show(w);
    CHECK(shim_visible(TEST_HWND) == 1);

    g_last_act = -1;
    shim_send(TEST_HWND, WM_LBUTTONUP, 0,
              (LPARAM)((440 - UM_UI_PAD - 59) | ((230 - UM_UI_PAD - 21) << 16)));
    CHECK(g_last_act == UM_ACT_OPEN);

    g_last_act = -1; g_last_row = -99;
    shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(200 | (100 << 16)));
    CHECK(g_last_act == UM_ACT_OPEN_ROW && g_last_row == 0);

    g_last_act = -1;
    shim_send(TEST_HWND, WM_LBUTTONUP, 0,
              (LPARAM)((16 + 45) | ((230 - UM_UI_PAD - 21) << 16)));
    CHECK(g_last_act == UM_ACT_TOGGLE_EXPAND);

    g_last_act = -1;
    shim_send(TEST_HWND, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(g_last_act == UM_ACT_CLOSE);

    g_last_act = -1;
    shim_send(TEST_HWND, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(g_last_act == UM_ACT_OPEN);

    shim_send(TEST_HWND, WM_MOUSEHOVER, 0, 0);
    {
        int before = g_n_acts;
        shim_send(TEST_HWND, WM_TIMER, 1, 0);
        shim_send(TEST_HWND, WM_TIMER, 1, 0);
        CHECK(g_n_acts == before);
        shim_send(TEST_HWND, WM_MOUSELEAVE, 0, 0);
    }

    g_last_act = -1; g_last_row = -99;
    shim_set_next_menu_result(4);
    shim_send(TEST_HWND, WM_RBUTTONUP, 0, (LPARAM)(200 | (100 << 16)));
    CHECK(g_last_act == UM_ACT_EJECT && g_last_row == 0);

    /* ------------------------------------------------------------------ */
    /* 以下为 TDD 新增断言（红 → 绿）                                      */
    /* ------------------------------------------------------------------ */

    printf("== 8. UTF-8 → UTF-16 编码（乱码根因：原来逐字节 cast）==\n");
    {
        uint16_t out[64];
        int n = um_ui_utf8_to_utf16("USB 设备监控", out, 64);
        CHECK(n == 8);                       /* 不是 16（逐字节） */
        CHECK(out[0] == 0x0055 && out[1] == 0x0053 && out[2] == 0x0042);
        CHECK(out[3] == 0x0020);             /* 空格 */
        CHECK(out[4] == 0x8BBE);             /* 设 —— 关键：不是 0x00E8 */
        CHECK(out[5] == 0x5907);             /* 备 */
        CHECK(out[6] == 0x76D1);             /* 监 */
        CHECK(out[7] == 0x63A7);             /* 控 */
        /* 代理对（emoji U+1F600）必须产出 2 个码元，不是乱码 */
        n = um_ui_utf8_to_utf16("\xF0\x9F\x98\x80", out, 64);
        CHECK(n == 2 && out[0] == 0xD83D && out[1] == 0xDE00);
        /* 缓冲区不足时截断但不越界 */
        n = um_ui_utf8_to_utf16("设备", out, 1);
        CHECK(n == 0);
        n = um_ui_utf8_to_utf16("", out, 64);
        CHECK(n == 0 && out[0] == 0);

        /* 非法序列一律 U+FFFD（不再静默吞掉或吐半解码乱码）。
         * 红绿验证：旧实现对 C0 80 产出内嵌 NUL（截断宽字符串）、对 E8 41
         * 产出控制符 U+0008、对 ED A0 80 吐裸代理 D800。 */
        n = um_ui_utf8_to_utf16("\xC0\x80", out, 64);          /* 过长 NUL */
        CHECK(n == 1 && out[0] == 0xFFFD);
        n = um_ui_utf8_to_utf16("\xE8" "A", out, 64);         /* 截断 + A 重新处理 */
        CHECK(n == 2 && out[0] == 0xFFFD && out[1] == 0x0041);
        n = um_ui_utf8_to_utf16("\xED\xA0\x80", out, 64);    /* 孤代理 D800 */
        CHECK(n == 1 && out[0] == 0xFFFD);
        n = um_ui_utf8_to_utf16("\xF5\x80\x80\x80", out, 64); /* > 10FFFF */
        CHECK(n == 1 && out[0] == 0xFFFD);
        n = um_ui_utf8_to_utf16("\xFE", out, 64);              /* 非法引导 */
        CHECK(n == 1 && out[0] == 0xFFFD);
        n = um_ui_utf8_to_utf16("\x80" "A", out, 64);         /* 孤续字节 */
        CHECK(n == 2 && out[0] == 0xFFFD && out[1] == 0x0041);
        n = um_ui_utf8_to_utf16("\xE8", out, 64);             /* 末尾截断（续字节读到 NUL） */
        CHECK(n == 1 && out[0] == 0xFFFD);
    }

    printf("== 9. 设备分类决策树（拓展坞/网卡/智能笔/无盘符/误报防御）==\n");
    {
        unsigned usb_stor = UM_TAG_USB | UM_TAG_STORAGE;
        CHECK(um_ui_classify(usb_stor | UM_TAG_VOLUME, 1, 1, 1, 1, 1)
              == UM_KIND_USB_STORAGE);
        /* 未分配盘符：有 USB+Storage 但没有盘符 */
        CHECK(um_ui_classify(usb_stor, 0, 1, 1, 1, 1)
              == UM_KIND_STORAGE_NO_LETTER);
        /* 智能笔 / HID：无 Storage 标签 */
        CHECK(um_ui_classify(UM_TAG_USB | UM_TAG_HID, 0, 0, 0, 1, 1)
              == UM_KIND_HID_PEN);
        /* 智能笔带配置盘（复合设备）：Storage 优先，保留多标签 */
        CHECK(um_ui_classify(UM_TAG_USB | UM_TAG_HID | UM_TAG_STORAGE, 1, 1, 1, 1, 1)
              == UM_KIND_USB_STORAGE);
        /* USB 无线网卡 */
        CHECK(um_ui_classify(UM_TAG_USB | UM_TAG_NET, 0, 0, 0, 1, 1)
              == UM_KIND_NET_ADAPTER);
        /* 拓展坞 / 集线器 */
        CHECK(um_ui_classify(UM_TAG_USB | UM_TAG_HUB, 0, 0, 0, 1, 1)
              == UM_KIND_DOCK_HUB);
        /* 读卡器空槽：有 Storage 但无介质 —— 不算已插入 */
        CHECK(um_ui_classify(usb_stor, 0, 1, 1, 0, 1) == UM_KIND_EMPTY_SLOT);
        /* BitLocker 未解锁 */
        CHECK(um_ui_classify(usb_stor | UM_TAG_VOLUME, 1, 1, 1, 1, 0)
              == UM_KIND_LOCKED_VOLUME);
        /* 虚拟光驱 / iSCSI / VHD：根本没有 USB 祖先，且 BusType 非 USB
         * —— 不纳入 USB 存储（注意 tags 里不能带 UM_TAG_USB） */
        CHECK(um_ui_classify(UM_TAG_STORAGE | UM_TAG_VOLUME, 1, 0, 1, 1, 1)
              == UM_KIND_UNKNOWN);
        /* 只有 USB 标签、没有任何功能接口 */
        CHECK(um_ui_classify(UM_TAG_USB, 0, 0, 0, 1, 1) == UM_KIND_UNKNOWN);
    }

    printf("== 10. 能力判定：谁能打开、谁能安全弹出 ==\n");
    {
        CHECK(um_ui_can_open(UM_KIND_USB_STORAGE) == 1);
        CHECK(um_ui_can_open(UM_KIND_STORAGE_NO_LETTER) == 0);  /* 无盘符打不开 */
        CHECK(um_ui_can_open(UM_KIND_EMPTY_SLOT) == 0);
        CHECK(um_ui_can_open(UM_KIND_LOCKED_VOLUME) == 0);      /* 未解锁打不开 */
        CHECK(um_ui_can_open(UM_KIND_HID_PEN) == 0);
        CHECK(um_ui_can_open(UM_KIND_DOCK_HUB) == 0);
        CHECK(um_ui_can_open(UM_KIND_NET_ADAPTER) == 0);

        CHECK(um_ui_can_eject(UM_KIND_USB_STORAGE, 1) == 1);
        CHECK(um_ui_can_eject(UM_KIND_STORAGE_NO_LETTER, 1) == 1); /* 存储可弹出 */
        CHECK(um_ui_can_eject(UM_KIND_USB_STORAGE, 0) == 0);        /* 固定盘禁弹 */
        CHECK(um_ui_can_eject(UM_KIND_HID_PEN, 1) == 0);            /* 笔不弹 */
        CHECK(um_ui_can_eject(UM_KIND_NET_ADAPTER, 1) == 0);        /* 网卡不弹 */
        CHECK(um_ui_can_eject(UM_KIND_DOCK_HUB, 1) == 0);           /* 拓展坞不弹 */
        CHECK(um_ui_can_eject(UM_KIND_EMPTY_SLOT, 1) == 0);         /* 空槽不弹 */
        CHECK(um_ui_can_eject(UM_KIND_UNKNOWN, 1) == 0);
    }

    printf("== 11. 多分区 U 盘聚合（DiskNumber 为键，不是盘符）==\n");
    {
        um_volume vols[8];
        um_device_row rows[8];
        int n;
        memset(vols, 0, sizeof vols);
        /* 同一个 U 盘（disk 2）上的 3 个分区 */
        for (n = 0; n < 3; n++) {
            vols[n].disk_number = 2;
            vols[n].tags = UM_TAG_USB | UM_TAG_STORAGE | UM_TAG_VOLUME;
            vols[n].kind = UM_KIND_USB_STORAGE;
            vols[n].total = 62668800000ULL;
            vols[n].free = 20000000000ULL;
            vols[n].pct = 60;
            snprintf(vols[n].letters[vols[n].n_letters++], 4, "%c:",
                     (char)('E' + n));
        }
        /* 另一台独立 U 盘（disk 5） */
        vols[3].disk_number = 5;
        vols[3].tags = UM_TAG_USB | UM_TAG_STORAGE | UM_TAG_VOLUME;
        vols[3].kind = UM_KIND_USB_STORAGE;
        vols[3].total = 32000000000ULL;
        vols[3].free = 24000000000ULL;
        vols[3].pct = 25;
        snprintf(vols[3].letters[vols[3].n_letters++], 4, "H:");

        n = um_ui_group_by_disk(vols, 4, rows, 8);
        CHECK(n == 2);                       /* 3 个分区 + 1 台 = 2 个设备 */
        CHECK(rows[0].n_letters == 3);
        CHECK(strcmp(rows[0].letters[0], "E:") == 0);
        CHECK(strcmp(rows[0].letters[2], "G:") == 0);
        CHECK(rows[0].disk_number == 2);
        CHECK(rows[1].n_letters == 1);
        CHECK(strcmp(rows[1].letters[0], "H:") == 0);
        /* 多行副标题应含分区数 */
        {
            char sub[160];
            um_ui_row_subtitle(&rows[0], sub, sizeof sub);
            CHECK(strstr(sub, "3 个分区") != NULL);
            CHECK(strstr(sub, "E:") != NULL && strstr(sub, "G:") != NULL);
        }
        {
            char sub[160];
            um_ui_row_subtitle(&rows[1], sub, sizeof sub);
            CHECK(strstr(sub, "3 个分区") == NULL);   /* 单分区不说分区数 */
        }
    }

    printf("== 12. 未分配盘符 / 非存储设备的提示文案 ==\n");
    {
        char b[256];
        um_ui_device_notice(UM_KIND_STORAGE_NO_LETTER, b, sizeof b);
        CHECK(strstr(b, "未分配盘符") != NULL);
        um_ui_device_notice(UM_KIND_HID_PEN, b, sizeof b);
        CHECK(strstr(b, "不支持打开") != NULL || strstr(b, "无存储") != NULL);
        um_ui_device_notice(UM_KIND_NET_ADAPTER, b, sizeof b);
        CHECK(strstr(b, "无存储") != NULL);
        um_ui_device_notice(UM_KIND_DOCK_HUB, b, sizeof b);
        CHECK(strstr(b, "拓展坞") != NULL || strstr(b, "集线器") != NULL);
        um_ui_device_notice(UM_KIND_EMPTY_SLOT, b, sizeof b);
        CHECK(strstr(b, "未插入") != NULL || strstr(b, "无介质") != NULL);
        um_ui_device_notice(UM_KIND_LOCKED_VOLUME, b, sizeof b);
        CHECK(strstr(b, "解锁") != NULL);
        /* 1.1.1 原文：拔出时可能尚未分配盘符 */
        um_ui_remove_notice(b, sizeof b);
        CHECK(strstr(b, "可能尚未分配盘符") != NULL);
    }

    printf("== 7. 视觉快照（SVG）==\n");
    {
        um_toast_win *w2 = w;
        shim_set_next_menu_result(0);
        if (um_toast_win_is_expanded(w2)) um_toast_win_toggle_expand(w2);
        shim_reset();
        shim_paint(TEST_HWND);
        shim_svg("out-toast-collapsed.svg", 440, 230, "#101418");
        printf("  -> out-toast-collapsed.svg (%d 图元)\n", g_ndraws);
        CHECK(g_ndraws > 20);

        um_toast_win_toggle_expand(w2);
        CHECK(um_toast_win_is_expanded(w2) == 1);
        shim_reset();
        shim_paint(TEST_HWND);
        shim_svg("out-toast-expanded.svg", 440, 334, "#101418");
        printf("  -> out-toast-expanded.svg (%d 图元)\n", g_ndraws);
        CHECK(g_ndraws > 20);

        {
            um_toast_model m2 = m;
            um_theme lt;
            um_theme_resolve(&lt, "light", 1);
            snprintf(m2.status, sizeof m2.status, "%s", "正在安全弹出 E:…");
            m2.accent_kind = 2;
            um_toast_win_set_theme(w2, &lt);
            um_toast_win_update(w2, &m2);
            shim_reset();
            shim_paint(TEST_HWND);
            shim_svg("out-toast-light-status.svg", 440, 334, "#dfe4ec");
            printf("  -> out-toast-light-status.svg (%d 图元)\n", g_ndraws);
            CHECK(g_ndraws > 20);
        }
        /* 新增：多分区 + 非存储设备混合的视觉快照 */
        {
            um_toast_model m3;
            um_theme dk;
            um_theme_resolve(&dk, "dark", 1);
            memset(&m3, 0, sizeof m3);
            snprintf(m3.headline, sizeof m3.headline, "%s", "USB 设备监控");
            snprintf(m3.subtitle, sizeof m3.subtitle, "%s", "3 个设备 · 4 个卷");
            snprintf(m3.summary, sizeof m3.summary, "%s",
                     "USB 已连接：E:、F:、G:（同一设备）");
            m3.accent_kind = 1;
            /* 行 1：多分区 U 盘 */
            fill_volume(&m3.rows[0], "E:\\", "Kingston DataTraveler 3.0",
                        "可移动磁盘 · E:、F:、G:（3 个分区）",
                        62668800000ULL, 42600000000ULL, 20068800000ULL);
            m3.rows[0].n_letters = 3;
            snprintf(m3.rows[0].letters[1], 4, "F:");
            snprintf(m3.rows[0].letters[2], 4, "G:");
            /* 行 2：未分配盘符 */
            memset(&m3.rows[1], 0, sizeof m3.rows[1]);
            snprintf(m3.rows[1].title, sizeof m3.rows[1].title, "%s",
                     "Generic USB Storage");
            snprintf(m3.rows[1].subtitle, sizeof m3.rows[1].subtitle, "%s",
                     "USB 存储设备 · 未分配盘符");
            m3.rows[1].tags = UM_TAG_USB | UM_TAG_STORAGE;
            m3.rows[1].kind = UM_KIND_STORAGE_NO_LETTER;
            m3.rows[1].ejectable = 1;
            m3.rows[1].openable = 0;
            m3.rows[1].pct = -1;
            /* 行 3：智能笔 */
            memset(&m3.rows[2], 0, sizeof m3.rows[2]);
            snprintf(m3.rows[2].title, sizeof m3.rows[2].title, "%s",
                     "Wacom 智能笔");
            snprintf(m3.rows[2].subtitle, sizeof m3.rows[2].subtitle, "%s",
                     "USB HID 设备 · 无存储");
            m3.rows[2].tags = UM_TAG_USB | UM_TAG_HID;
            m3.rows[2].kind = UM_KIND_HID_PEN;
            m3.rows[2].ejectable = 0;
            m3.rows[2].openable = 0;
            m3.rows[2].pct = -1;
            m3.n_rows = 3;
            um_toast_win_set_theme(w2, &dk);
            um_toast_win_update(w2, &m3);
            shim_reset();
            shim_paint(TEST_HWND);
            {
                int h = um_toast_win_height(w2);
                shim_svg("out-toast-mixed-devices.svg", 440, h > 0 ? h : 334,
                         "#101418");
                printf("  -> out-toast-mixed-devices.svg (%d 图元)\n", g_ndraws);
            }
            CHECK(g_ndraws > 20);
        }
    }

    printf("== 13. 非存储设备不产生「打开/弹出」动作 ==\n");
    {
        um_toast_model mm;
        um_toast_win *w2;
        memset(&mm, 0, sizeof mm);
        snprintf(mm.headline, sizeof mm.headline, "%s", "USB 设备已连接");
        snprintf(mm.summary, sizeof mm.summary, "%s", "此设备无存储，无法打开。");
        mm.accent_kind = 2;
        /* 智能笔：不可打开、不可弹出 */
        memset(&mm.rows[0], 0, sizeof mm.rows[0]);
        snprintf(mm.rows[0].title, sizeof mm.rows[0].title, "%s", "Wacom 智能笔");
        snprintf(mm.rows[0].subtitle, sizeof mm.rows[0].subtitle, "%s",
                 "USB HID 设备 · 无存储");
        mm.rows[0].tags = UM_TAG_USB | UM_TAG_HID;
        mm.rows[0].kind = UM_KIND_HID_PEN;
        mm.rows[0].ejectable = 0;
        mm.rows[0].openable = 0;
        mm.rows[0].pct = -1;
        mm.n_rows = 1;

        w2 = um_toast_win_new(&mm, &theme, 1, on_action, NULL);
        CHECK(w2 != NULL);
        g_last_act = -1;
        /* 点行：不应触发打开（无盘符设备没有「打开」语义） */
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(200 | (100 << 16)));
        CHECK(g_last_act != UM_ACT_OPEN_ROW);
        um_toast_win_destroy(w2);
    }

    printf("== 14. group_by_disk：disk_number<0 不可归并（回归：笔+网卡+拓展坞并成一行）==\n");
    {
        um_volume vols[4];
        um_device_row rows[4];
        int n;
        memset(vols, 0, sizeof vols);
        snprintf(vols[0].title, sizeof vols[0].title, "%s", "Wacom 智能笔");
        vols[0].kind = UM_KIND_HID_PEN;  vols[0].disk_number = -1;
        snprintf(vols[1].title, sizeof vols[1].title, "%s", "USB 网卡");
        vols[1].kind = UM_KIND_NET_ADAPTER; vols[1].disk_number = -1;
        snprintf(vols[2].title, sizeof vols[2].title, "%s", "拓展坞");
        vols[2].kind = UM_KIND_DOCK_HUB;  vols[2].disk_number = -1;
        n = um_ui_group_by_disk(vols, 3, rows, 4);
        CHECK(n == 3);                      /* 不是 1（旧实现会并成一行幽灵设备） */
        CHECK(strcmp(rows[0].title, "Wacom 智能笔") == 0);
        CHECK(strcmp(rows[1].title, "USB 网卡") == 0);
        CHECK(strcmp(rows[2].title, "拓展坞") == 0);
        /* 同盘号仍然正常归并 */
        vols[0].disk_number = 7; vols[1].disk_number = 7;
        snprintf(vols[0].letters[0], 4, "E:");
        snprintf(vols[1].letters[0], 4, "F:");
        vols[0].n_letters = 1; vols[1].n_letters = 1;
        vols[0].kind = UM_KIND_USB_STORAGE;
        n = um_ui_group_by_disk(vols, 3, rows, 4);
        CHECK(n == 2);
    }

    printf("== 15. 滚轮滚动与展开几何（回归：展开不重设窗口、无滚轮时第5行后不可见）==\n");
    {
        um_toast_model mm;
        um_toast_win *w2;
        int i, gw, gh, gx, gy;

        memset(&mm, 0, sizeof mm);
        snprintf(mm.headline, sizeof mm.headline, "%s", "USB 设备监控");
        mm.accent_kind = 1;
        for (i = 0; i < 10; i++) {
            mm.rows[i].kind = UM_KIND_USB_STORAGE;
            mm.rows[i].openable = 1;
            mm.rows[i].ejectable = 1;
            mm.rows[i].pct = -1;
            snprintf(mm.rows[i].title, sizeof mm.rows[i].title, "设备 %d", i);
            snprintf(mm.rows[i].letters[0], 4, "%c:", 'E' + i);
            mm.rows[i].n_letters = 1;
        }
        mm.n_rows = 10;

        w2 = um_toast_win_new(&mm, &theme, 1, on_action, NULL);
        CHECK(w2 != NULL);
        um_toast_win_show(w2);
        CHECK(um_toast_win_is_visible(w2) == 1);
        CHECK(um_toast_win_height(w2) == 230);          /* 折叠 */

        /* 折叠态滚轮无效 */
        shim_send(TEST_HWND, WM_MOUSEWHEEL,
                  (WPARAM)((unsigned short)(-120) << 16), 0);
        CHECK(um_toast_win_is_expanded(w2) == 0);

        um_toast_win_toggle_expand(w2);
        CHECK(um_toast_win_height(w2) == 430);          /* 10 行夹到上限 */
        /* 展开后窗口真实尺寸+锚点都变了（旧实现只改 w->height 不改窗口） */
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 440 && gh == 430);
        CHECK(gx == 1920 - 440 - 18 && gy == 1040 - 430 - 18);

        /* 滚轮向下（负 delta）：内容上移，scroll_px 增大并夹到上界 */
        {
            const um_toast_state *st = um_toast_win_state(w2);
            int maxs = um_toast_max_scroll(&mm, st, um_toast_win_height(w2));
            CHECK(st->scroll_px == 0);
            shim_send(TEST_HWND, WM_MOUSEWHEEL,
                      (WPARAM)((unsigned short)(-120) << 16), 0);
            shim_send(TEST_HWND, WM_MOUSEWHEEL,
                      (WPARAM)((unsigned short)(-120) << 16), 0);
            CHECK(st->scroll_px == 240);
            /* 命中区随滚动移动：y=100 原属行 0/1（行顶 90/152）；滚 240px 后
             * 首个可见行是行 4（ry = 90+4*62-240 = 98），y=100 命中行 4。 */
            CHECK(um_toast_hit_test(&mm, st, 440, 430, 60, 100)
                  == UM_HIT_ROW + 4);
            /* 滚过头的部分被夹住 */
            for (i = 0; i < 30; i++)
                shim_send(TEST_HWND, WM_MOUSEWHEEL,
                          (WPARAM)((unsigned short)(-120) << 16), 0);
            CHECK(st->scroll_px == maxs);
            /* 反向滚回 0（同样夹住） */
            for (i = 0; i < 30; i++)
                shim_send(TEST_HWND, WM_MOUSEWHEEL,
                          (WPARAM)((unsigned short)(120) << 16), 0);
            CHECK(st->scroll_px == 0);
            CHECK(um_toast_win_is_expanded(w2) == 1);
        }
        um_toast_win_destroy(w2);
    }

    um_toast_win_destroy(w);
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
