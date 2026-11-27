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
    snprintf(m->subtitle, sizeof m->subtitle, "%s", "3 个存储设备 · 3 个卷");
    snprintf(m->subtitle_all, sizeof m->subtitle_all, "%s", "3 个设备 · 3 个存储设备");
    snprintf(m->summary, sizeof m->summary, "%s", "3 个存储设备已就绪 · 可安全打开");
    snprintf(m->summary_all, sizeof m->summary_all, "%s", "设备概览");
    snprintf(m->count, sizeof m->count, "%s", "3 个");
    snprintf(m->count_all, sizeof m->count_all, "%s", "3 个");
    m->n_storage = 3;
    m->n_volumes = 3;
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
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能看到最后一条进度 */
    um_theme       theme;
    um_toast_win  *w;

    printf("== 1. 主题解析（auto → 系统深色 / 浅色）==\n");
    um_theme_resolve(&theme, "auto", 1);
    CHECK(strcmp(theme.name, "dark") == 0 && theme.panel == 0x111923);
    um_theme_resolve(&theme, "light", 1);
    CHECK(strcmp(theme.name, "light") == 0 && theme.panel == 0xfafcff);

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
        CHECK(um_toast_measure_height(&m, &s) == 406);     /* 3 行 */
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
        /* 右上角 ✖：24×24 命中区，中心 (412,29)。 */
        h = um_toast_hit_test(&m, &s, UM_UI_WIDTH, 230, 412, 29);
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
    w = um_toast_win_new(&m, &theme, 1, 0, 0, on_action, NULL);
    CHECK(w != NULL);
    if (!w) { printf("fatal: no window\n"); return 1; }
    um_toast_win_show(w);
    CHECK(shim_visible(TEST_HWND) == 1);

    g_last_act = -1;
    shim_send(TEST_HWND, WM_LBUTTONUP, 0,
              (LPARAM)((440 - UM_UI_PAD - 59) | ((230 - UM_UI_PAD - 21) << 16)));
    CHECK(g_last_act == UM_ACT_OPEN);

    /* 2.6.2：右上角 ✖ 取代左下角"关闭"按钮。 */
    g_last_act = -1;
    shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(412 | (29 << 16)));
    CHECK(g_last_act == UM_ACT_CLOSE);

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

    /* 2.6.1: 交互完成后倒计时重置为完整时长（而非停在暂停值） */
    {
        const um_toast_state *rst = um_toast_win_state(w);
        CHECK(rst->remaining_ms == rst->ttl_ms);
        shim_send(TEST_HWND, WM_MOUSEHOVER, 0, 0);
        shim_send(TEST_HWND, WM_TIMER, 1, 0);
        CHECK(rst->remaining_ms == rst->ttl_ms);   /* hover 暂停语义保留 */
        shim_send(TEST_HWND, WM_MOUSELEAVE, 0, 0);
        CHECK(rst->remaining_ms == rst->ttl_ms);   /* 离开后从满时长倒数 */
    }

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
        CHECK(strstr(b, "不可访问") != NULL);
        /* 1.1.1 原文：拔出时可能尚未分配盘符 */
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
        shim_svg("out-toast-expanded.svg", 440, 406, "#101418");
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
            shim_svg("out-toast-light-status.svg", 440, 406, "#dfe4ec");
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
                shim_svg("out-toast-mixed-devices.svg", 440, h > 0 ? h : 406,
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
        mm.rows[0].kind = UM_KIND_HID_PEN;
        mm.rows[0].ejectable = 0;
        mm.rows[0].openable = 0;
        mm.rows[0].pct = -1;
        mm.n_rows = 1;

        w2 = um_toast_win_new(&mm, &theme, 1, 0, 0, on_action, NULL);
        CHECK(w2 != NULL);
        g_last_act = -1;
        /* 点行：不应触发打开（无盘符设备没有「打开」语义） */
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(200 | (100 << 16)));
        CHECK(g_last_act != UM_ACT_OPEN_ROW);
        um_toast_win_destroy(w2);
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

        w2 = um_toast_win_new(&mm, &theme, 1, 0, 0, on_action, NULL);
        CHECK(w2 != NULL);
        um_toast_win_show(w2);
        CHECK(um_toast_win_is_visible(w2) == 1);
        CHECK(um_toast_win_height(w2) == 230);          /* 折叠 */

        /* 折叠态滚轮无效 */
        shim_send(TEST_HWND, WM_MOUSEWHEEL,
                  ((WPARAM)(uint16_t)-120 << 16), 0);
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
                      ((WPARAM)(uint16_t)-120 << 16), 0);
            shim_send(TEST_HWND, WM_MOUSEWHEEL,
                      ((WPARAM)(uint16_t)-120 << 16), 0);
            CHECK(st->scroll_px == 2 * UM_UI_ROW_H);
            /* Two wheel detents reveal the third whole row. */
            CHECK(um_toast_hit_test(&mm, st, 440, 430, 60, 100)
                  == UM_HIT_ROW + 2);
            /* 滚过头的部分被夹住 */
            for (i = 0; i < 30; i++)
                shim_send(TEST_HWND, WM_MOUSEWHEEL,
                          ((WPARAM)(uint16_t)-120 << 16), 0);
            CHECK(st->scroll_px == maxs);
            /* 反向滚回 0（同样夹住） */
            for (i = 0; i < 30; i++)
                shim_send(TEST_HWND, WM_MOUSEWHEEL,
                          ((WPARAM)120 << 16), 0);
            CHECK(st->scroll_px == 0);
            CHECK(um_toast_win_is_expanded(w2) == 1);
        }
        um_toast_win_destroy(w2);
    }

    /* ---- 2.5.0: 0-row notification = the panel kernel, at any DPI ---- */

    printf("== 16. 0-row info box: natural height, no dead 57px band ==\n");
    {
        um_toast_model m0;
        um_toast_state s;
        memset(&m0, 0, sizeof m0);
        snprintf(m0.headline, sizeof m0.headline, "%s", "\xe4\xb8\x8d\xe5\x8f\xa3\xe8\xa7\xa3");
        snprintf(m0.subtitle, sizeof m0.subtitle, "%s", "Kingston DataTraveler (E:)");
        m0.accent_kind = 3;
        m0.n_rows = 0;
        um_toast_state_init(&s, 10000);
        CHECK(um_toast_measure_height(&m0, &s) == 148);
        snprintf(m0.status, sizeof m0.status, "%s", "x");
        CHECK(um_toast_measure_height(&m0, &s) == 166);
        m0.status[0] = 0;

        /* A row-bearing panel keeps the 205 floor (no regression). */
        CHECK(um_toast_measure_height(&m, &s) >= 205);

        /* Recommended width: clamped to [300, UM_UI_WIDTH], grows with text. */
        {
            um_toast_model m1 = m0;
            char longline[UM_UI_MAX_TEXT];
            int w_min = um_toast_suggest_width(&m0);
            int w_max;
            memset(longline, 'W', sizeof longline - 1);
            longline[sizeof longline - 1] = 0;
            snprintf(m1.summary, sizeof m1.summary, "%s", longline);
            w_max = um_toast_suggest_width(&m1);
            CHECK(w_min >= 300 && w_min <= UM_UI_WIDTH);
            CHECK(w_max == UM_UI_WIDTH);
            CHECK(w_max > w_min);
        }
    }

    printf("== 17. 0-row info box: narrow width, click anywhere closes ==\n");
    {
        um_toast_model m0;
        um_toast_win *wn;
        int gx, gy, gw, gh;
        memset(&m0, 0, sizeof m0);
        snprintf(m0.headline, sizeof m0.headline, "%s", "USB");
        snprintf(m0.subtitle, sizeof m0.subtitle, "%s", "SanDisk Ultra (Q:\\)");
        m0.accent_kind = 3;
        wn = um_toast_win_new(&m0, &theme, 1, 360, 0, on_action, NULL);
        CHECK(wn != NULL);
        um_toast_win_show(wn);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 360 && gh == 148);
        /* No expand/close/open buttons on a 0-row model: every point misses,
         * and a miss means "close" (2.4.0 text-toast semantics). */
        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(40 | (110 << 16)));
        CHECK(g_last_act == UM_ACT_CLOSE);
        um_toast_win_destroy(wn);
    }

    printf("== 18. work-area clamp: <=70%% wide / <=85%% tall, no hardcoded px ==\n");
    {
        um_toast_win *wc = um_toast_win_new(&m, &theme, 1, 0, 0,
                                            on_action, NULL);
        int gx, gy, gw, gh;
        CHECK(wc != NULL);
        um_toast_win_show(wc);
        shim_set_workarea(500, 400);        /* 440x230 does not fit */
        um_toast_win_show(wc);              /* recomputes fit + re-anchors */
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw <= 350 && gh <= 340);      /* 0.70*500 / 0.85*400 */
        CHECK(gw < 440 && gh < 230);
        CHECK(gx >= 0 && gy >= 0);
        shim_set_workarea(1920, 1040);
        um_toast_win_show(wc);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 440 && gh == 230);      /* back to design size */
        CHECK(gx == 1920 - 440 - 18 && gy == 1040 - 230 - 18);
        um_toast_win_destroy(wc);
    }

    printf("== 19. high DPI: 144dpi scales everything, WM_DPICHANGED live ==\n");
    {
        um_toast_win *wd;
        int gx, gy, gw, gh;
        shim_set_dpi(144);        /* sampled by GetDeviceCaps in _new */
        wd = um_toast_win_new(&m, &theme, 1, 0, 0, on_action, NULL);
        CHECK(wd != NULL);
        um_toast_win_show(wd);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 660 && gh == 345);      /* 440x230 x 144/96 */
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(shim_font_log_px(0) == 24);   /* title 16px x 1.5 */
        /* dragged onto a 100%-scale monitor: system sends WM_DPICHANGED */
        shim_send(TEST_HWND, WM_DPICHANGED, (WPARAM)((96 << 16) | 96), 0);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 440 && gh == 230);
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(shim_font_log_px(0) == 16);
        /* 75% scale is a legal low dpi and must no longer be clamped to 96 */
        shim_send(TEST_HWND, WM_DPICHANGED, (WPARAM)((72 << 16) | 72), 0);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw == 330 && gh == 173);      /* 440x230 x 72/96 */
        um_toast_win_destroy(wd);
        shim_reset();
    }

    printf("== 20. slot offset: move_slot stacks by the scaled height ==\n");
    {
        um_toast_win *ws = um_toast_win_new(&m, &theme, 1, 0, 0,
                                            on_action, NULL);
        int gx0, gy0, gx1, gy1, gw, gh;
        um_toast_win_show(ws);
        shim_window_geometry(&gx0, &gy0, &gw, &gh);
        um_toast_win_move_slot(ws, 1);
        shim_window_geometry(&gx1, &gy1, &gw, &gh);
        CHECK(gx1 == gx0);
        CHECK(gy1 == gy0 - (230 + 12));
        um_toast_win_move_slot(ws, 0);      /* slot 0 = no-op, geometry untouched */
        {
            int gx2, gy2, gw2, gh2;
            shim_window_geometry(&gx2, &gy2, &gw2, &gh2);
            CHECK(gx2 == gx1 && gy2 == gy1 && gw2 == gw && gh2 == gh);
        }
        um_toast_win_destroy(ws);
    }

    printf("== 21. hit testing survives fit (physical coords -> design space) ==\n");
    {
        um_toast_win *wf;
        int gx, gy, gw, gh;
        shim_set_workarea(500, 400);
        wf = um_toast_win_new(&m, &theme, 1, 0, 0, on_action, NULL);
        CHECK(wf != NULL);
        um_toast_win_show(wf);
        shim_window_geometry(&gx, &gy, &gw, &gh);
        CHECK(gw < 440);                    /* clamped */
        /* Click the bottom-right corner of the SHRUNK window: still the
         * "open" button, because to_logical divides by the same factor. */
        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0,
                  (LPARAM)((gw - UM_UI_PAD - 59) | ((gh - UM_UI_PAD - 21) << 16)));
        CHECK(g_last_act == UM_ACT_OPEN);
        um_toast_win_destroy(wf);
        shim_set_workarea(1920, 1040);
    }

    um_toast_win_destroy(w);
    printf("== Keyboard: expand, scroll focused rows, close ==\n");
    {
        um_toast_model mm;
        um_toast_win *kw;
        int i;
        make_model(&mm);
        for (i = 3; i < 9; i++) mm.rows[i] = mm.rows[2];
        mm.n_rows = 9;
        kw = um_toast_win_new(&mm, &theme, 1, 0, 0, on_action, NULL);
        CHECK(kw != NULL);
        if (kw) {
            um_toast_win_show(kw);
            shim_send(TEST_HWND, WM_KEYDOWN, VK_TAB, 0);
            CHECK(um_toast_win_state(kw)->focus_row == 0);
            shim_send(TEST_HWND, WM_KEYDOWN, VK_TAB, 0);
            CHECK(um_toast_win_state(kw)->focus_btn == UM_HIT_EXPAND);
            shim_send(TEST_HWND, WM_KEYDOWN, VK_RETURN, 0);
            CHECK(um_toast_win_is_expanded(kw));
            for (i = 0; i < 7; i++) shim_send(TEST_HWND, WM_KEYDOWN, VK_DOWN, 0);
            CHECK(um_toast_win_state(kw)->focus_row == 6);
            CHECK(um_toast_win_state(kw)->scroll_px > 0);
            shim_send(TEST_HWND, WM_KEYDOWN, VK_RETURN, 0);
            CHECK(g_last_act == UM_ACT_OPEN_ROW && g_last_row == 6);
            shim_send(TEST_HWND, WM_CLOSE, 0, 0);
            CHECK(g_last_act == UM_ACT_CLOSE);
            um_toast_win_destroy(kw);
        }
    }

    /* ---- 2.6.2: 默认隐藏非存储设备 / 触摸 / 扫描视觉 / 亮色 ---- */

    printf("== 22. 视图过滤：默认只见存储设备，可见序号映射回模型行 ==\n");
    {
        um_toast_model mf;
        um_toast_state sf;
        make_model(&mf);
        memset(&mf.rows[3], 0, sizeof mf.rows[3]);
        snprintf(mf.rows[3].title, sizeof mf.rows[3].title, "%s", "USB 拓展坞 / 集线器");
        mf.rows[3].kind = UM_KIND_DOCK_HUB;
        memset(&mf.rows[4], 0, sizeof mf.rows[4]);
        snprintf(mf.rows[4].title, sizeof mf.rows[4].title, "%s", "Wacom 智能笔");
        mf.rows[4].kind = UM_KIND_HID_PEN;
        mf.n_rows = 5;
        snprintf(mf.subtitle_all, sizeof mf.subtitle_all, "%s", "5 个设备 · 3 个存储设备");
        snprintf(mf.count_all, sizeof mf.count_all, "%s", "5 个");

        um_toast_state_init(&sf, 10000);
        CHECK(um_toast_row_is_storage(&mf.rows[0]) == 1);
        CHECK(um_toast_row_is_storage(&mf.rows[3]) == 0);
        CHECK(um_toast_has_other(&mf) == 1);
        CHECK(um_toast_visible_count(&mf, &sf) == 3);
        CHECK(um_toast_visible_row(&mf, &sf, 2) == 2);
        CHECK(um_toast_visible_row(&mf, &sf, 3) == -1);
        CHECK(um_toast_first_openable_row(&mf, &sf) == 0);
        CHECK(um_toast_hit_test(&mf, &sf, 440, 230, 412, 29) == UM_HIT_CLOSE);
        CHECK(um_toast_hit_test(&mf, &sf, 440, 230, 170, 193)
              == UM_HIT_SHOW_OTHERS);

        sf.show_others = 1;
        CHECK(um_toast_visible_count(&mf, &sf) == 5);
        CHECK(um_toast_visible_row(&mf, &sf, 3) == 3);
        CHECK(um_toast_visible_row(&mf, &sf, 4) == 4);
        sf.show_others = 0;
    }

    printf("== 23. 新布局图元：✖ / 进度条 / 打开U盘 / 打开卷 / 非存储标签 ==\n");
    {
        um_toast_model mf;
        um_toast_state sf;
        um_drawlist dl;
        um_theme th;
        int i, close_lines = 0, bars = 0, open_u = 0, open_v = 0, tags = 0;
        int light_icons = 0;

        make_model(&mf);
        um_toast_state_init(&sf, 10000);
        um_theme_resolve(&th, "dark", 1);
        um_toast_layout(&mf, &sf, &th, 440, 230, &dl);
        for (i = 0; i < dl.n; i++) {
            const um_draw *d = &dl.items[i];
            if (d->kind == UM_D_LINE && d->hit == UM_HIT_NONE) close_lines++;
            if (d->kind == UM_D_RECT && d->y == 227 && d->h == 3) bars++;
            if (d->kind == UM_D_TEXT && strcmp(d->text, "打开U盘") == 0) open_u++;
        }
        CHECK(close_lines == 2);          /* ✖ 两笔 */
        CHECK(bars >= 1);                 /* 底部 3px 剩余时间条 */
        CHECK(open_u == 1);
        CHECK(open_v == 0);               /* 折叠态没有行内"打开卷" */

        sf.expanded = 1;
        um_toast_layout(&mf, &sf, &th, 440, 406, &dl);
        for (i = 0; i < dl.n; i++) {
            const um_draw *d = &dl.items[i];
            if (d->kind == UM_D_TEXT && strcmp(d->text, "打开卷") == 0) open_v++;
        }
        CHECK(open_v == 3);
        CHECK(um_toast_hit_test(&mf, &sf, 440, 406, 382, 108)
              == UM_HIT_ROW_OPEN + 0);

        /* 亮色主题：图标图元必须带 theme_light，后端据此换浅色壳体。 */
        um_theme_resolve(&th, "light", 1);
        um_toast_layout(&mf, &sf, &th, 440, 406, &dl);
        for (i = 0; i < dl.n; i++)
            if (dl.items[i].kind == UM_D_ICON && dl.items[i].theme_light)
                light_icons++;
        CHECK(light_icons >= 1);
        (void)tags;
    }

    printf("== 24. 显示其他设备：改状态 + 自动展开 + 拨回折叠 ==\n");
    {
        um_toast_model mf;
        um_toast_win *w2;
        make_model(&mf);
        memset(&mf.rows[3], 0, sizeof mf.rows[3]);
        mf.rows[3].kind = UM_KIND_DOCK_HUB;
        memset(&mf.rows[4], 0, sizeof mf.rows[4]);
        mf.rows[4].kind = UM_KIND_HID_PEN;
        mf.n_rows = 5;
        snprintf(mf.subtitle_all, sizeof mf.subtitle_all, "%s", "5 个设备 · 3 个存储设备");
        snprintf(mf.count_all, sizeof mf.count_all, "%s", "5 个");

        w2 = um_toast_win_new(&mf, &theme, 1, 0, 0, on_action, NULL);
        CHECK(w2 != NULL);
        um_toast_win_show(w2);
        CHECK(um_toast_win_height(w2) == 230);

        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(170 | (193 << 16)));
        CHECK(g_last_act == UM_ACT_TOGGLE_OTHERS);
        CHECK(um_toast_win_state(w2)->show_others == 1);
        CHECK(um_toast_win_is_expanded(w2) == 1);
        CHECK(um_toast_win_height(w2) == 430);

        /* 展开后按钮区随窗口下移：用当前高度算 y，不能再用折叠态的 193。 */
        g_last_act = -1;
        shim_send(TEST_HWND, WM_LBUTTONUP, 0,
                  (LPARAM)(170 | ((um_toast_win_height(w2) - 37) << 16)));
        CHECK(g_last_act == UM_ACT_TOGGLE_OTHERS);
        CHECK(um_toast_win_state(w2)->show_others == 0);
        /* 隐藏其他设备后仍有 3 个存储设备 → 保持展开（原型 C → B）。 */
        CHECK(um_toast_visible_count(&mf, um_toast_win_state(w2)) == 3);
        CHECK(um_toast_win_height(w2) == 406);
        um_toast_win_destroy(w2);
    }

    printf("== 25. 触摸：轻点 / 拖动滚动 / 长按菜单 / 鼠标提升抑制 ==\n");
    {
        um_toast_model mf;
        um_toast_win *w2;
        int i, before;
        memset(&mf, 0, sizeof mf);
        snprintf(mf.headline, sizeof mf.headline, "%s", "USB 设备监控");
        for (i = 0; i < 10; i++) {
            mf.rows[i].kind = UM_KIND_USB_STORAGE;
            mf.rows[i].openable = 1;
            mf.rows[i].pct = -1;
            snprintf(mf.rows[i].title, sizeof mf.rows[i].title, "设备 %d", i);
            snprintf(mf.rows[i].letters[0], 4, "%c:", 'E' + i);
            mf.rows[i].n_letters = 1;
        }
        mf.n_rows = 10;
        w2 = um_toast_win_new(&mf, &theme, 1, 0, 0, on_action, NULL);
        CHECK(w2 != NULL);
        um_toast_win_show(w2);

        /* 轻点底部"打开U盘"= 鼠标点击。 */
        shim_touch_reset();
        shim_touch_push(TOUCHEVENTF_DOWN, 36500, 19300, 1);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        shim_touch_push(TOUCHEVENTF_UP, 36500, 19300, 1);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        CHECK(g_last_act == UM_ACT_OPEN);

        /* 长按行 → 行菜单（不会在抬手时再触发一次打开）。 */
        shim_set_next_menu_result(4);
        g_last_act = -1;
        shim_touch_reset();
        shim_touch_push(TOUCHEVENTF_DOWN, 20000, 10000, 2);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        shim_send(TEST_HWND, WM_TIMER, 3, 0);
        shim_touch_push(TOUCHEVENTF_UP, 20000, 10000, 2);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        CHECK(g_last_act == UM_ACT_EJECT);

        /* 展开态上下拖动 = 滚动，抬手不误触打开。 */
        um_toast_win_toggle_expand(w2);
        g_last_act = -1;
        shim_touch_reset();
        shim_touch_push(TOUCHEVENTF_DOWN, 20000, 20000, 3);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        shim_touch_push(TOUCHEVENTF_MOVE, 20000, 8000, 3);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        shim_touch_push(TOUCHEVENTF_UP, 20000, 8000, 3);
        shim_send(TEST_HWND, WM_TOUCH, 1, 1);
        CHECK(um_toast_win_state(w2)->scroll_px == 120);
        CHECK(g_last_act == -1);

        /* 系统把触摸提升成鼠标时不能双触发。 */
        g_last_act = -1;
        before = g_n_acts;
        shim_set_extra_info(0xFF515700ULL);
        shim_send(TEST_HWND, WM_LBUTTONUP, 0, (LPARAM)(200 | (100 << 16)));
        CHECK(g_n_acts == before);
        shim_set_extra_info(0);
        um_toast_win_destroy(w2);
    }

    printf("== 26. 扫描视觉：8 点旋转 + 相位推进 ==\n");
    {
        um_toast_model ms;
        um_toast_win *ws;
        memset(&ms, 0, sizeof ms);
        snprintf(ms.headline, sizeof ms.headline, "%s", "正在扫描设备");
        snprintf(ms.subtitle, sizeof ms.subtitle, "%s", "USB 设备监控");
        snprintf(ms.count, sizeof ms.count, "…");
        snprintf(ms.summary, sizeof ms.summary, "%s", "正在枚举存储设备与卷…");
        ms.scanning = 1;
        ws = um_toast_win_new(&ms, &theme, 1, 0, 0, on_action, NULL);
        CHECK(ws != NULL);
        um_toast_win_show(ws);
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(g_ndraws > 8);              /* 8 个点 + 底环 + 边框 … */
        shim_send(TEST_HWND, WM_TIMER, 1, 0);
        CHECK(um_toast_win_state(ws)->spinner_phase == 1);
        um_toast_win_set_scanning(ws, 0);
        shim_reset();
        shim_paint(TEST_HWND);
        CHECK(g_ndraws > 8);              /* 恢复正常图标后仍能画 */
        um_toast_win_destroy(ws);
    }

    printf("== 27. 单一存储设备：不显示展开，仍有 ✖ 与进度条 ==\n");
    {
        um_toast_model m1;
        um_toast_state s1;
        um_drawlist dl;
        um_theme th;
        int i, expand_hits = 0;
        make_model(&m1);
        m1.n_rows = 1;
        m1.n_storage = 1;
        um_toast_state_init(&s1, 10000);
        um_theme_resolve(&th, "dark", 1);
        um_toast_layout(&m1, &s1, &th, 440, 230, &dl);
        for (i = 0; i < dl.n; i++) if (dl.items[i].hit == UM_HIT_EXPAND) expand_hits++;
        CHECK(expand_hits == 0);
        CHECK(um_toast_hit_test(&m1, &s1, 440, 230, 412, 29) == UM_HIT_CLOSE);
        CHECK(um_toast_hit_test(&m1, &s1, 440, 230, 365, 193) == UM_HIT_OPEN);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
