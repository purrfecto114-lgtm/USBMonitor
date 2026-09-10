/* um_toast_ui.c — platform-neutral toast UI kernel (C99, no window system).
 *
 * 与 v1.1.1 的对应关系（同一套尺寸/配色/文案，只是去掉了 Qt）：
 *   Theme                -> um_theme_resolve
 *   ToastWindow.refresh  -> um_toast_layout
 *   VolumeRow            -> um_toast_layout 里的行块
 *   countdown_label      -> um_ui_countdown
 *   format_bytes         -> um_ui_format_bytes
 *   enter/leaveEvent     -> um_toast_pause / um_toast_resume
 *   keyPressEvent(Esc)   -> 后端收到 UM_HIT_CLOSE 语义（见 um_toast_win32.c）
 */
#include "um_toast_ui.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ 主题 -- */

void um_theme_resolve(um_theme *t, const char *requested, int system_dark)
{
    int dark;
    if (requested && strcmp(requested, "dark") == 0)      dark = 1;
    else if (requested && strcmp(requested, "light") == 0) dark = 0;
    else dark = system_dark ? 1 : 0;              /* "auto" 或未知值 */

    if (dark) {
        t->name = "dark";
        t->panel = 0x202630; t->panel2 = 0x2a323e;
        t->text  = 0xf7f9fc; t->muted  = 0xb5c0cf;
        t->border = 0x3a4656;
        t->accent = 0x75a7ff; t->accent_hover = 0x91b9ff;
        t->progress = 0x3b4654;
        t->shadow = 0x91000000;
    } else {
        t->name = "light";
        t->panel = 0xfbfcff; t->panel2 = 0xf2f5fa;
        t->text  = 0x111827; t->muted  = 0x687386;
        t->border = 0xd9e2ee;
        t->accent = 0x1769e0; t->accent_hover = 0x0f5ed2;
        t->progress = 0xe5ebf3;
        t->shadow = 0x370f172a;
    }
    t->ok    = 0x34c759;
    t->warn  = 0xffb020;
    t->error = 0xff5c5c;
}

static unsigned long accent_of(const um_theme *t, int kind)
{
    switch (kind) {
    case 1:  return t->ok;
    case 2:  return t->warn;
    case 3:  return t->error;
    default: return t->accent;
    }
}

/* ------------------------------------------------------------ 文案工具集 -- */

void um_ui_format_bytes(unsigned long long v, char *out, size_t n)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double size = (double)v;
    int u = 0;
    while (u < 5 && size >= 1024.0) { size /= 1024.0; u++; }
    if (u <= 1) snprintf(out, n, "%.0f %s", size, unit[u]);
    else        snprintf(out, n, "%.1f %s", size, unit[u]);
}

void um_ui_countdown(int remaining_ms, char *out, size_t n)
{
    if (remaining_ms <= 0) { snprintf(out, n, "%s", "即将关闭"); return; }
    if (remaining_ms >= 60000) {
        int minutes = (remaining_ms + 59999) / 60000;
        snprintf(out, n, "%d 分钟后自动关闭", minutes);
        return;
    }
    snprintf(out, n, "%d 秒后自动关闭", (remaining_ms + 999) / 1000);
}

void um_ui_capacity_line(const um_volume *v, char *out, size_t n)
{
    char total[32], free_s[32];
    if (v->total == 0) { snprintf(out, n, "%s", "容量未知"); return; }
    um_ui_format_bytes(v->total, total, sizeof total);
    um_ui_format_bytes(v->free, free_s, sizeof free_s);
    if (v->pct >= 0)
        snprintf(out, n, "容量 %s · 可用 %s · %d%%", total, free_s, v->pct);
    else
        snprintf(out, n, "容量 %s · 可用 %s", total, free_s);
}

/* -------------------------------------------------------------- 状态机 ---- */

void um_toast_state_init(um_toast_state *s, int ttl_ms)
{
    memset(s, 0, sizeof *s);
    s->ttl_ms = ttl_ms > 0 ? ttl_ms : 10000;
    s->remaining_ms = s->ttl_ms;
    s->hover_row = -1;
    s->focus_row = -1;
}

int um_toast_tick(um_toast_state *s, int dt_ms)
{
    if (!s->visible || s->paused) return 0;
    s->remaining_ms -= dt_ms;
    if (s->remaining_ms <= 0) { s->remaining_ms = 0; return 1; }
    return 0;
}

void um_toast_pause(um_toast_state *s)
{
    if (s->paused) return;
    s->paused = 1;                       /* 保留 remaining_ms，离开时续上 */
}

void um_toast_resume(um_toast_state *s)
{
    if (!s->paused) return;
    s->paused = 0;
    if (s->remaining_ms <= 0) s->remaining_ms = s->ttl_ms;
}

/* ------------------------------------------------------------- draw list -- */

static um_draw *push(um_drawlist *out)
{
    if (out->n >= (int)(sizeof out->items / sizeof out->items[0])) return NULL;
    memset(&out->items[out->n], 0, sizeof out->items[out->n]);
    return &out->items[out->n++];
}

/* 注：纯矩形图元未使用（当前布局只用圆角矩形），不定义 push_rect。 */
static void push_round(um_drawlist *o, int x, int y, int w, int h,
                       unsigned long c, int r, um_ui_hit hit)
{
    um_draw *d = push(o);
    if (!d) return;
    d->kind = UM_D_ROUNDRECT; d->x = x; d->y = y; d->w = w; d->h = h;
    d->color = c; d->radius = r; d->hit = hit;
}

static void push_text(um_drawlist *o, int x, int y, int w, um_font f,
                      unsigned long c, um_align a, const char *text)
{
    um_draw *d = push(o);
    if (!d) return;
    d->kind = UM_D_TEXT; d->x = x; d->y = y; d->w = w;
    d->font = f; d->color = c; d->align = a; d->clip = 1;
    snprintf(d->text, sizeof d->text, "%s", text ? text : "");
}

static void push_icon(um_drawlist *o, int x, int y, int w, int h,
                      const char *status, unsigned long badge, unsigned long fg)
{
    um_draw *d = push(o);
    if (!d) return;
    d->kind = UM_D_ICON; d->x = x; d->y = y; d->w = w; d->h = h;
    d->color = fg; d->color2 = badge;
    snprintf(d->text, sizeof d->text, "%s", status);
}

/* --------------------------------------------------------------- 布局器 --- */

static int model_has_openable(const um_toast_model *m)
{
    int i;
    for (i = 0; i < m->n_rows; i++)
        if (m->rows[i].openable) return 1;
    return 0;
}

/* 图标状态：非存储设备用各自的标识（后端据此画不同角标/配色） */
static const char *icon_status_for(um_dev_kind kind)
{
    switch (kind) {
    case UM_KIND_USB_STORAGE:        return "usb";
    case UM_KIND_STORAGE_NO_LETTER:  return "nostor";
    case UM_KIND_EMPTY_SLOT:         return "empty";
    case UM_KIND_LOCKED_VOLUME:      return "lock";
    case UM_KIND_DOCK_HUB:           return "hub";
    case UM_KIND_NET_ADAPTER:        return "net";
    case UM_KIND_HID_PEN:            return "pen";
    default:                         return "unknown";
    }
}

int um_toast_measure_height(const um_toast_model *m, const um_toast_state *s)
{
    int rows_h = 0;
    int h = UM_UI_PAD + 46 + 20 + 8 + UM_UI_BTN_H + UM_UI_PAD;

    if (m->status[0]) h += 18;
    if (m->n_rows > 0) {
        if (s->expanded) {
            int full = m->n_rows * UM_UI_ROW_H;
            rows_h = full > UM_UI_ROWS_MAX_H ? UM_UI_ROWS_MAX_H : full;
        } else {
            rows_h = UM_UI_ROWS_COLLAPSED_H;   /* 与 1.1.1 scroll minHeight 一致 */
        }
    }
    h += rows_h;
    /* 与 1.1.1 一致：min(max(自然高度, 205), 目标高度) */
    {
        int target = s->expanded ? UM_UI_EXPANDED_H : UM_UI_COLLAPSED_H;
        if (h < 205) h = 205;
        if (h > target) h = target;
    }
    return h;
}

void um_toast_layout(const um_toast_model *m, const um_toast_state *s,
                     const um_theme *t, int width, int height,
                     um_drawlist *out)
{
    const int p = UM_UI_PAD;
    int y = p;
    int i;
    unsigned long accent = accent_of(t, m->accent_kind);
    int btn_y = height - p - UM_UI_BTN_H;
    int rows_top;

    out->n = 0;

    /* 窗口底 + 边框（圆角；后端可用区域裁剪实现真圆角） */
    push_round(out, 0, 0, width, height, t->panel, 10, UM_HIT_NONE);

    /* ---- 头部：图标 + 标题 + 副标题 + 计数 ---- */
    push_icon(out, p, y, 34, 34,
              m->accent_kind == 1 ? "add"
              : m->accent_kind == 3 ? "remove" : "usb",
              accent, t->text);
    push_text(out, p + 44, y + 1, width - p * 2 - 60, UM_F_TITLE, t->text,
              UM_A_LEFT, m->headline);
    push_text(out, p + 44, y + 23, width - p * 2 - 60, UM_F_SMALL, t->muted,
              UM_A_LEFT, m->subtitle);
    if (m->count[0])
        push_text(out, width - p - 60, y + 5, 60, UM_F_SMALL, t->muted,
                  UM_A_RIGHT, m->count);
    y += 46;

    /* ---- 摘要 ---- */
    push_text(out, p, y, width - p * 2, UM_F_BODY, t->muted, UM_A_LEFT,
              m->summary);
    y += 20;

    /* ---- 状态行（安全弹出进行中等）---- */
    if (m->status[0]) {
        push_text(out, p, y, width - p * 2, UM_F_SMALL, t->warn, UM_A_LEFT,
                  m->status);
        y += 18;
    }
    y += 8;
    rows_top = y;

    /* ---- 设备/分区行 ---- */
    {
        int rows_bottom = btn_y - 10;
        int visible_rows = s->expanded ? m->n_rows : (m->n_rows > 0 ? 1 : 0);
        int clip_h = rows_bottom - rows_top;
        for (i = 0; i < visible_rows && i < UM_UI_MAX_ROWS; i++) {
            const um_volume *v = &m->rows[i];
            int ry = rows_top + i * UM_UI_ROW_H - s->scroll_px;
            int rh = UM_UI_ROW_H - 8;
            unsigned long bg = t->panel2;
            unsigned long bd = t->border;
            int hit = UM_HIT_ROW + i;
            if (ry >= rows_bottom || ry + rh <= rows_top) continue;

            if (i == s->hover_row || i == s->focus_row) {
                bd = t->accent;
                bg = t->panel2;
            }
            push_round(out, p, ry, width - p * 2, rh, bg, 6,
                       (um_ui_hit)hit);
            /* 行边框：用一条线不方便表达圆角，交给后端按 border 描边 */
            {
                um_draw *d = push(out);
                if (d) {
                    d->kind = UM_D_ROUNDRECT;
                    d->x = p; d->y = ry; d->w = width - p * 2; d->h = rh;
                    d->color = bd; d->radius = 6; d->hit = UM_HIT_NONE;
                    d->stroke = 1;          /* 只描边 */
                }
            }

            push_icon(out, p + 12, ry + 9, 30, 30, icon_status_for(v->kind),
                      v->ejectable ? t->accent : t->muted, t->text);
            push_text(out, p + 12 + 40, ry + 9,
                      width - p * 2 - 52 - (s->expanded ? 76 : 0),
                      UM_F_ROWTITLE, t->text, UM_A_LEFT, v->title);
            push_text(out, p + 12 + 40, ry + 29,
                      width - p * 2 - 52 - (s->expanded ? 76 : 0),
                      UM_F_SMALL, t->muted, UM_A_LEFT, v->subtitle);
            {
                char cap[160];
                um_ui_capacity_line(v, cap, sizeof cap);
                push_text(out, p + 12, ry + 47, width - p * 2 - 24,
                          UM_F_SMALL, t->muted, UM_A_LEFT, cap);
            }
            /* 容量条：底色 + 已用（自绘而非 PBM_SETBARCOLOR —— 启用视觉
             * 样式后该消息无效，见 docs/PORT_AND_TRADEOFFS.md） */
            push_round(out, p + 12, ry + 57, width - p * 2 - 24, 6,
                       t->progress, 3, UM_HIT_NONE);
            if (v->pct > 0) {
                int full = width - p * 2 - 24;
                int used_w = full * v->pct / 100;
                push_round(out, p + 12, ry + 57, used_w, 6, accent, 3,
                           UM_HIT_NONE);
            }
            if (s->expanded && v->openable) {
                int bx = width - p - 12 - 68;
                push_round(out, bx, ry + 9, 68, 34, accent, 6,
                           (um_ui_hit)(UM_HIT_ROW_OPEN + i));
                push_text(out, bx, ry + 19, 68, UM_F_BTN, 0xffffff,
                          UM_A_CENTER, "打开");
            }
        }
        (void)clip_h;
    }

    /* ---- 底栏按钮 ---- */
    if (m->n_rows > 1) {
        unsigned long bg = (s->hover_btn == UM_HIT_EXPAND) ? t->accent_hover
                                                           : t->border;
        push_round(out, p, btn_y, 90, UM_UI_BTN_H, bg, 6, UM_HIT_EXPAND);
        push_text(out, p, btn_y + 13, 90, UM_F_BTN,
                  (s->hover_btn == UM_HIT_EXPAND) ? 0xffffff : t->text,
                  UM_A_CENTER, s->expanded ? "折叠" : "展开");
    }
    {
        unsigned long bg = (s->hover_btn == UM_HIT_CLOSE) ? t->accent_hover
                                                          : t->border;
        int cx = (m->n_rows > 1) ? p + 90 + 8 : p;
        push_round(out, cx, btn_y, 90, UM_UI_BTN_H, bg, 6, UM_HIT_CLOSE);
        push_text(out, cx, btn_y + 13, 90, UM_F_BTN,
                  (s->hover_btn == UM_HIT_CLOSE) ? 0xffffff : t->text,
                  UM_A_CENTER, "关闭");
    }

    /* 倒计时（主按钮左侧；hover 暂停时显示"已暂停"） */
    {
        char cd[64];
        if (s->paused) snprintf(cd, sizeof cd, "%s", "已暂停");
        else if (m->status[0]) snprintf(cd, sizeof cd, "%s", "");
        else um_ui_countdown(s->remaining_ms, cd, sizeof cd);
        push_text(out, width - p - 118 - 130, btn_y + 13, 124, UM_F_SMALL,
                  t->muted, UM_A_RIGHT, cd);
    }

    /* 主按钮：折叠态显示（展开态每行自带"打开"）—— 与 1.1.1 一致 */
    if (m->n_rows > 0 && !s->expanded && model_has_openable(m)) {
        unsigned long bg = (s->hover_btn == UM_HIT_OPEN) ? t->accent_hover
                                                         : t->accent;
        push_round(out, width - p - 118, btn_y, 118, UM_UI_BTN_H, bg, 6,
                   UM_HIT_OPEN);
        push_text(out, width - p - 118, btn_y + 13, 118, UM_F_BTN, 0xffffff,
                  UM_A_CENTER, "打开U盘");
    }
}

um_ui_hit um_toast_hit_test(const um_toast_model *m, const um_toast_state *s,
                            int width, int height, int x, int y)
{
    um_drawlist dl;
    um_theme    tmp;
    int i;

    /* 几何与配色无关，用一个临时主题即可（避免 API 里多传一个参数）。 */
    um_theme_resolve(&tmp, "dark", 1);
    um_toast_layout(m, s, &tmp, width, height, &dl);

    /* 反向遍历 = 后绘制的在上层：按钮优先于行，"看得见就点得到"。 */
    for (i = dl.n - 1; i >= 0; i--) {
        const um_draw *d = &dl.items[i];
        if (!d->hit) continue;
        if (x >= d->x && x < d->x + d->w && y >= d->y && y < d->y + d->h)
            return d->hit;
    }
    return UM_HIT_NONE;
}

int um_toast_max_scroll(const um_toast_model *m, const um_toast_state *s,
                        int height)
{
    int rows_top = UM_UI_PAD + 46 + 20 + 8 + (m->status[0] ? 18 : 0);
    int btn_y    = height - UM_UI_PAD - UM_UI_BTN_H;
    int viewport = (btn_y - 10) - rows_top;
    int full, maxs;

    if (viewport < 0) viewport = 0;
    full = s->expanded ? m->n_rows * UM_UI_ROW_H : UM_UI_ROW_H;
    maxs = full - viewport;
    if (maxs < 0) maxs = 0;
    return maxs;
}

/* ============================================================== 新增能力 ==
 * 以下三段对应 1.1.1 明确修过的问题与本次新增的设备分类需求：
 *   1) UTF-8 → UTF-16（修"中文乱码"根因，而非把每个字节当码点）
 *   2) 设备分类决策树（HID 智能笔 / 网卡 / 拓展坞 / 无盘符 / 误报防御）
 *   3) 多分区聚合（以 DiskNumber 为键，而不是盘符）
 * ========================================================================== */

/* ------------------------------------------------------- UTF-8 → UTF-16 -- */

int um_ui_utf8_to_utf16(const char *utf8, uint16_t *out, int max_units)
{
    const unsigned char *s = (const unsigned char *)utf8;
    int n = 0;

    if (!utf8 || !out || max_units <= 0) return 0;

    while (*s && n + 1 < max_units) {
        unsigned long cp;
        int len, i, ok = 1;

        if (*s < 0x80) {
            cp = *s++; len = 0;
        } else if ((*s & 0xE0) == 0xC0) {
            cp = *s++ & 0x1F; len = 1;
        } else if ((*s & 0xF0) == 0xE0) {
            cp = *s++ & 0x0F; len = 2;
        } else if ((*s & 0xF8) == 0xF0) {
            cp = *s++ & 0x07; len = 3;
        } else {
            /* 非法起始字节（孤立续字节 80-BF / 5、6 字节引导 F8-FD / FE-FF）:
             * 按 Unicode "maximal subpart" 建议发一个 U+FFFD，只消费该字节 */
            out[n++] = 0xFFFD;
            s++;
            continue;
        }
        /* 续字节必须全部到位；否则发出 U+FFFD 且不消费 offending 字节
         * （它可能是下一个合法序列的引导字节，交给下一轮处理） */
        for (i = 0; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { ok = 0; break; }
        }
        if (!ok) {
            out[n++] = 0xFFFD;
            continue;
        }
        for (i = 0; i < len; i++)
            cp = (cp << 6) | (s[i] & 0x3F);
        s += len;

        /* 过长编码（overlong）、代理区、超界 → U+FFFD（不静默吞掉） */
        if ((len == 1 && cp < 0x80) ||
            (len == 2 && cp < 0x800) ||
            (len == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) {
            out[n++] = 0xFFFD;
            continue;
        }

        if (cp < 0x10000) {
            out[n++] = (uint16_t)cp;
        } else {
            /* 非 BMP：需要代理对，缓冲区不足则整体截断 */
            if (n + 3 > max_units) break;   /* 2 码元 + 终止符 */
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    if (n < max_units) out[n] = 0;
    else               out[max_units - 1] = 0;
    return n;
}

/* --------------------------------------------------------- 设备分类决策树 -- */

um_dev_kind um_ui_classify(unsigned tags, int has_letter, int bus_is_usb,
                           int removable, int media_present, int unlocked)
{
    int is_usb   = (tags & UM_TAG_USB) != 0;
    int is_stor  = (tags & UM_TAG_STORAGE) != 0;
    int is_vol   = (tags & UM_TAG_VOLUME) != 0;
    int is_hid   = (tags & UM_TAG_HID) != 0;
    int is_net   = (tags & UM_TAG_NET) != 0;
    int is_hub   = (tags & UM_TAG_HUB) != 0;

    /* 存储优先：复合设备（智能笔带配置盘、拓展坞带读卡器）仍按存储呈现，
     * 其余标签保留在 tags 里，UI 可决定是否提示"同时是 HID 设备"。 */
    if (is_stor) {
        /* 总线证据：BusTypeUsb 或 USB 祖先，二者至少其一；
         * 都不是 → 虚拟光驱 / iSCSI / VHD 之类，不纳入 USB 存储。 */
        if (!is_usb && !bus_is_usb) return UM_KIND_UNKNOWN;
        if (!media_present)         return UM_KIND_EMPTY_SLOT;   /* 读卡器空槽 */
        if (is_vol && !unlocked)    return UM_KIND_LOCKED_VOLUME;/* BitLocker */
        if (!has_letter)            return UM_KIND_STORAGE_NO_LETTER;
        return UM_KIND_USB_STORAGE;
    }

    if (is_hub) return is_usb ? UM_KIND_DOCK_HUB : UM_KIND_UNKNOWN;
    if (is_net) return is_usb ? UM_KIND_NET_ADAPTER : UM_KIND_UNKNOWN;
    if (is_hid) return is_usb ? UM_KIND_HID_PEN : UM_KIND_UNKNOWN;

    /* 只有 USB 标签、没有任何功能接口 → 证据不足（1.1.1 bug1 的教训）。
     * removable 只影响 can_eject，不影响分类。 */
    (void)removable;
    return UM_KIND_UNKNOWN;
}

int um_ui_can_open(um_dev_kind kind)
{
    return kind == UM_KIND_USB_STORAGE;
}

int um_ui_can_eject(um_dev_kind kind, int removable)
{
    switch (kind) {
    case UM_KIND_USB_STORAGE:
    case UM_KIND_STORAGE_NO_LETTER:
        /* 只有标记为可移除的存储才允许弹出；固定盘/系统盘必须禁止。 */
        return removable ? 1 : 0;
    /* 非存储设备（笔 / 网卡 / 拓展坞）与证据不足者一律不弹：
     * 对它们发存储 eject IOCTL 既无意义也会失败。 */
    case UM_KIND_HID_PEN:
    case UM_KIND_NET_ADAPTER:
    case UM_KIND_DOCK_HUB:
    case UM_KIND_EMPTY_SLOT:
    case UM_KIND_LOCKED_VOLUME:
    case UM_KIND_UNKNOWN:
    default:
        return 0;
    }
}

void um_ui_device_notice(um_dev_kind kind, char *out, size_t n)
{
    const char *msg;
    switch (kind) {
    case UM_KIND_USB_STORAGE:        msg = ""; break;
    case UM_KIND_STORAGE_NO_LETTER:
        msg = "USB 存储设备已连接，但未分配盘符（可在磁盘管理中分配）"; break;
    case UM_KIND_EMPTY_SLOT:
        msg = "读卡器已就绪，但未插入介质"; break;
    case UM_KIND_LOCKED_VOLUME:
        msg = "卷已锁定（如 BitLocker），解锁后才能打开"; break;
    case UM_KIND_DOCK_HUB:
        msg = "拓展坞 / USB 集线器 · 无存储"; break;
    case UM_KIND_NET_ADAPTER:
        msg = "USB 网络设备 · 无存储"; break;
    case UM_KIND_HID_PEN:
        msg = "USB HID 设备（如智能笔）· 无存储，不支持打开"; break;
    case UM_KIND_UNKNOWN:
    default:
        msg = "USB 设备已连接（类型未确定，无存储）"; break;
    }
    snprintf(out, n, "%s", msg);
}

void um_ui_remove_notice(char *out, size_t n)
{
    /* 与 1.1.1 app.py:1606 的原文保持一致 */
    snprintf(out, n, "%s", "USB 设备已拔出；该设备可能尚未分配盘符。");
}

/* ------------------------------------------------------------- 多分区聚合 -- */

int um_ui_group_by_disk(const um_volume *vols, int n_vols,
                        um_device_row *rows, int max_rows)
{
    int i, j, n = 0;

    if (!vols || !rows || max_rows <= 0) return 0;

    for (i = 0; i < n_vols && n < max_rows; i++) {
        const um_volume *v = &vols[i];
        um_device_row  *r = NULL;

        /* disk_number < 0 = "不知道在哪块盘上"：不可归并，各自成行。
         * （否则笔/网卡/拓展坞这类无盘号的非存储设备会被并成一行幽灵设备。） */
        if (v->disk_number >= 0) {
            for (j = 0; j < n; j++) {
                if (rows[j].disk_number == v->disk_number) { r = &rows[j]; break; }
            }
        }
        if (!r) {
            r = &rows[n++];
            memset(r, 0, sizeof *r);
            r->disk_number = v->disk_number;
            snprintf(r->title, sizeof r->title, "%s", v->title);
            r->tags = v->tags;
            r->kind = v->kind;
            r->pct = -1;
        }
        r->tags |= v->tags;
        r->total += v->total;
        r->used  += v->used;
        r->free  += v->free;
        if (r->pct < 0) r->pct = v->pct;
        if (r->title[0] == '\0' && v->title[0])
            snprintf(r->title, sizeof r->title, "%s", v->title);
        /* 优先保留"最具体"的类别：存储 > 非存储 */
        if (v->kind == UM_KIND_USB_STORAGE) r->kind = v->kind;
        if (v->ejectable) r->ejectable = 1;
        if (v->openable)  r->openable = 1;
        for (j = 0; j < v->n_letters && r->n_letters < UM_UI_MAX_LETTER; j++) {
            if (v->letters[j][0])
                snprintf(r->letters[r->n_letters++], 4, "%s", v->letters[j]);
        }
    }
    return n;
}

void um_ui_row_subtitle(const um_device_row *row, char *out, size_t n)
{
    char list[UM_UI_MAX_LETTER * 6];
    int i;

    if (!row) { if (n) out[0] = 0; return; }

    list[0] = '\0';
    for (i = 0; i < row->n_letters; i++) {
        if (i) strncat(list, "、", sizeof list - strlen(list) - 1);
        strncat(list, row->letters[i], sizeof list - strlen(list) - 1);
    }

    if (row->n_letters > 1)
        snprintf(out, n, "可移动磁盘 · %s（%d 个分区）", list, row->n_letters);
    else if (row->n_letters == 1)
        snprintf(out, n, "可移动磁盘 · %s", list);
    else
        snprintf(out, n, "%s", "未分配盘符");
}
