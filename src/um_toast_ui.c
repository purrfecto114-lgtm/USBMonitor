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
        t->panel = 0x111923; t->panel2 = 0x1b2837;
        t->text = 0xf2f6fc; t->muted = 0xb4c3d5;
        t->border = 0x374a60;
        t->accent = 0x9abaff; t->accent_hover = 0xb8cfff; t->on_accent = 0x0b1930;
        t->progress = 0x364860;
        t->ok = 0x6be1b5; t->warn = 0xffcc80; t->error = 0xffa6a3;
    } else {
        t->name = "light";
        t->panel = 0xfafcff; t->panel2 = 0xf0f5fb;
        t->text = 0x17263a; t->muted = 0x4c5f76;
        t->border = 0xcbd8e7;
        t->accent = 0x2051b0; t->accent_hover = 0x163e91; t->on_accent = 0xffffff;
        t->progress = 0xd6e2f2;
        t->ok = 0x0b7159; t->warn = 0x8a4b00; t->error = 0x9c373c;
    }
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

void um_toast_restart(um_toast_state *s)
{
    if (!s->visible) return;
    /* 交互完成后重置倒计时为完整时长；hover 暂停语义不变——
     * 鼠标仍悬停时保持暂停，移开后从满时长重新倒数（2.6.1）。 */
    s->remaining_ms = s->ttl_ms;
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

static void stroke(um_drawlist *out, int x, int y, int w, int h, int radius,
                   unsigned long color)
{
    int index = out->n;
    push_round(out, x, y, w, h, color, radius, UM_HIT_NONE);
    if (out->n > index) out->items[index].stroke = 1;
}

static int row_top(void) { return 90; }
static int button_top(int height) { return height - UM_UI_PAD - UM_UI_BTN_H; }

int um_toast_rows_visible(const um_toast_model *m, const um_toast_state *s, int height)
{
    int rows;
    if (!m->n_rows) return 0;
    if (!s->expanded) return 1;
    rows = (button_top(height) - 6 - row_top()) / UM_UI_ROW_H;
    if (rows < 1) rows = 1;
    return rows < m->n_rows ? rows : m->n_rows;
}

int um_toast_measure_height(const um_toast_model *m, const um_toast_state *s)
{
    if (!m->n_rows && !m->overview) return m->status[0] ? 166 : 148;
    if (s->expanded && m->n_rows > 1) {
        int height = row_top() + m->n_rows * UM_UI_ROW_H + UM_UI_PAD + UM_UI_BTN_H + 6;
        return height < UM_UI_EXPANDED_H ? height : UM_UI_EXPANDED_H;
    }
    return UM_UI_COLLAPSED_H;
}

static void button(um_drawlist *out, const um_theme *t, const um_toast_state *s,
                   int x, int y, int w, int h, um_ui_hit hit, const char *label,
                   int primary)
{
    int hover = s->hover_btn == (int)hit;
    unsigned long bg = primary ? (hover ? t->accent_hover : t->accent)
                               : (hover ? t->progress : t->panel2);
    push_round(out, x, y, w, h, bg, 10, hit);
    stroke(out, x, y, w, h, 10, s->focus_btn == (int)hit ? t->text :
           (primary ? bg : t->border));
    push_text(out, x, y + (h - 16) / 2, w, UM_F_BTN,
              primary ? t->on_accent : t->text, UM_A_CENTER, label);
}

void um_toast_layout(const um_toast_model *m, const um_toast_state *s,
                     const um_theme *t, int width, int height, um_drawlist *out)
{
    const int p = UM_UI_PAD;
    int by = button_top(height);
    int badge = m->count[0] ? 62 : 0;
    int rows = um_toast_rows_visible(m, s, height);
    int first = s->expanded ? s->scroll_px / UM_UI_ROW_H : 0;
    int i, has_open = !s->expanded && model_has_openable(m);
    unsigned long accent = accent_of(t, m->accent_kind);
    char countdown[64];

    out->n = 0;
    push_round(out, 0, 0, width, height, t->panel, 18, UM_HIT_NONE);
    stroke(out, 1, 1, width - 2, height - 2, 17, t->border);
    push_round(out, p, p, 38, 38, t->panel2, 12, UM_HIT_NONE);
    push_icon(out, p + 6, p + 6, 27, 27,
              m->accent_kind == 1 ? "add" : m->accent_kind == 3 ? "remove" : "usb",
              accent, t->text);
    push_text(out, p + 49, p + 1, width - 2 * p - 49 - badge, UM_F_TITLE,
              t->text, UM_A_LEFT, m->headline);
    push_text(out, p + 49, p + 24, width - 2 * p - 49 - badge, UM_F_SMALL,
              t->muted, UM_A_LEFT, m->subtitle);
    if (badge) {
        push_round(out, width - p - 54, p + 3, 54, 26, t->panel2, 12, UM_HIT_NONE);
        push_text(out, width - p - 54, p + 9, 54, UM_F_SMALL,
                  t->text, UM_A_CENTER, m->count);
    }
    if (m->summary[0] || m->status[0]) {
        push_round(out, p, 62, 3, 18, accent, 2, UM_HIT_NONE);
        push_text(out, p + 12, 63, width - 2 * p - 12, UM_F_BODY,
                  t->muted, UM_A_LEFT, m->status[0] ? m->status : m->summary);
    }

    if (first + rows > m->n_rows) first = m->n_rows - rows;
    for (i = 0; i < rows; i++) {
        int index = first + i, y = row_top() + i * UM_UI_ROW_H;
        const um_volume *v = &m->rows[index];
        int row_w = width - 2 * p - (s->expanded && rows < m->n_rows ? 8 : 0);
        int row_h = UM_UI_ROW_H - 8;
        int right = p + row_w;
        int text_w = row_w - 58 - (s->expanded && v->openable ? 80 : 12);
        int hover = index == s->hover_row || index == s->focus_row;
        unsigned long edge = hover ? t->accent : t->border;
        const char *capacity = v->capacity;
        char formatted[128];

        push_round(out, p, y, row_w, row_h, t->panel2, 12,
                   (um_ui_hit)(UM_HIT_ROW + index));
        stroke(out, p, y, row_w, row_h, 12, edge);
        push_icon(out, p + 12, y + 13, 29, 29, icon_status_for(v->kind),
                  v->openable ? t->accent : t->muted, t->text);
        push_text(out, p + 54, y + 10, text_w, UM_F_ROWTITLE, t->text, UM_A_LEFT, v->title);
        push_text(out, p + 54, y + 30, text_w, UM_F_SMALL, t->muted, UM_A_LEFT, v->subtitle);
        if (!capacity[0] && v->total) {
            um_ui_capacity_line(v, formatted, sizeof formatted);
            capacity = formatted;
        }
        if (capacity[0])
            push_text(out, p + 54, y + 52, row_w - (v->pct >= 0 ? 152 : 64),
                      UM_F_SMALL, t->muted, UM_A_LEFT, capacity);
        if (v->total && v->pct >= 0) {
            int pct = v->pct > 100 ? 100 : v->pct;
            push_round(out, right - 80, y + 56, 68, 5, t->progress, 3, UM_HIT_NONE);
            if (pct > 0) push_round(out, right - 80, y + 56, 68 * pct / 100, 5,
                                    t->accent, 3, UM_HIT_NONE);
        }
        if (s->expanded && v->openable)
            button(out, t, s, right - 74, y + 12, 62, 31,
                   (um_ui_hit)(UM_HIT_ROW_OPEN + index), "打开", 0);
    }
    if (s->expanded && rows > 0 && rows < m->n_rows) {
        int track = rows * UM_UI_ROW_H - 8;
        int thumb = track * rows / m->n_rows;
        if (thumb < 16) thumb = 16;
        push_round(out, width - p - 4, row_top(), 4, track, t->progress, 2, UM_HIT_NONE);
        push_round(out, width - p - 4, row_top() + (track - thumb) * first / (m->n_rows - rows),
                   4, thumb, t->muted, 2, UM_HIT_NONE);
    }

    if (s->paused) snprintf(countdown, sizeof countdown, "计时已暂停");
    else um_ui_countdown(s->remaining_ms, countdown, sizeof countdown);
    if (!m->n_rows && !m->overview) {
        push_text(out, p, by + 12, width - 2 * p, UM_F_SMALL,
                  t->muted, UM_A_RIGHT, countdown);
        return;
    }
    if (!m->n_rows)
        push_round(out, p, row_top(), width - 2 * p, UM_UI_ROW_H - 8,
                   t->panel2, 12, UM_HIT_NONE);
    if (!m->n_rows)
        push_text(out, p, row_top() + 27, width - 2 * p, UM_F_BODY,
                  t->muted, UM_A_CENTER, "接入设备后会自动更新");

    if (m->n_rows > 1)
        button(out, t, s, p, by, 90, UM_UI_BTN_H, UM_HIT_EXPAND,
               s->expanded ? "收起列表" : "展开列表", 0);
    {
        int close_x = p + (m->n_rows > 1 ? 98 : 0);
        int text_x = close_x + 70;
        int text_right = has_open ? width - p - 126 : width - p;
        button(out, t, s, close_x, by, 66, UM_UI_BTN_H, UM_HIT_CLOSE, "关闭", 0);
        push_text(out, text_x, by + 14, text_right - text_x, UM_F_SMALL,
                  t->muted, UM_A_RIGHT, countdown);
    }
    if (has_open)
        button(out, t, s, width - p - 118, by, 118, UM_UI_BTN_H, UM_HIT_OPEN, "打开卷", 1);
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
    return s->expanded ? (m->n_rows - um_toast_rows_visible(m, s, height)) * UM_UI_ROW_H : 0;
}

/* ============================================================== 新增能力 ==
 * 以下三段对应 1.1.1 明确修过的问题与本次新增的设备分类需求：
 *   1) UTF-8 → UTF-16（修"中文乱码"根因，而非把每个字节当码点）
 *   2) 设备分类决策树（HID 智能笔 / 网卡 / 拓展坞 / 无盘符 / 误报防御）
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

    /* A transport tag alone does not identify a useful device function. */
    (void)removable;
    return UM_KIND_UNKNOWN;
}

void um_ui_device_notice(um_dev_kind kind, char *out, size_t n)
{
    const char *msg;
    switch (kind) {
    case UM_KIND_USB_STORAGE:        msg = ""; break;
    case UM_KIND_STORAGE_NO_LETTER:
        msg = "设备已连接 · 未分配盘符"; break;
    case UM_KIND_EMPTY_SLOT:
        msg = "读卡器已就绪，但未插入介质"; break;
    case UM_KIND_LOCKED_VOLUME:
        msg = "卷当前不可访问"; break;
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
