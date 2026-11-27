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

/* 主题没有 alpha 通道：用整数混合模拟半透明叠色。pct = 0..100。 */
static unsigned long blend(unsigned long a, unsigned long b, int pct)
{
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r, g, bl;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    r  = (ar * (100 - pct) + br * pct) / 100;
    g  = (ag * (100 - pct) + bg * pct) / 100;
    bl = (ab * (100 - pct) + bb * pct) / 100;
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)bl;
}

/* 面板底色亮 = 浅色主题（图标壳体需要另一套配色）。 */
static int theme_is_light(const um_theme *t)
{
    unsigned long c = t->panel;
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    return (r * 299 + g * 587 + b * 114) / 1000 > 140;
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

static void push_rect(um_drawlist *o, int x, int y, int w, int h,
                      unsigned long c, um_ui_hit hit)
{
    um_draw *d = push(o);
    if (!d) return;
    d->kind = UM_D_RECT; d->x = x; d->y = y; d->w = w; d->h = h;
    d->color = c; d->hit = hit;
}

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
                      const char *status, unsigned long badge, unsigned long fg,
                      int theme_light)
{
    um_draw *d = push(o);
    if (!d) return;
    d->kind = UM_D_ICON; d->x = x; d->y = y; d->w = w; d->h = h;
    d->color = fg; d->color2 = badge; d->theme_light = theme_light;
    snprintf(d->text, sizeof d->text, "%s", status);
}

/* --------------------------------------------------------------- 布局器 --- */

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

static void push_line(um_drawlist *out, int x, int y, int dx, int dy,
                      unsigned long color, int width, um_ui_hit hit)
{
    um_draw *d = push(out);
    if (!d) return;
    d->kind = UM_D_LINE;
    d->x = x; d->y = y; d->w = dx; d->h = dy;
    d->color = color; d->stroke = width > 0 ? width : 1; d->hit = hit;
}

static int row_top(void) { return 90; }
static int button_top(int height) { return height - UM_UI_PAD - UM_UI_BTN_H; }

/* ------------------------------------------------------- 视图过滤（新增）-- */

int um_toast_row_is_storage(const um_volume *v)
{
    if (!v) return 0;
    switch (v->kind) {
    case UM_KIND_USB_STORAGE:
    case UM_KIND_STORAGE_NO_LETTER:
    case UM_KIND_EMPTY_SLOT:
    case UM_KIND_LOCKED_VOLUME:
        return 1;
    default:
        return 0;
    }
}

int um_toast_visible_count(const um_toast_model *m, const um_toast_state *s)
{
    int i, n = 0;
    if (!m) return 0;
    if (s->show_others) return m->n_rows;
    for (i = 0; i < m->n_rows; i++)
        if (um_toast_row_is_storage(&m->rows[i])) n++;
    return n;
}

int um_toast_has_other(const um_toast_model *m)
{
    int i;
    if (!m) return 0;
    for (i = 0; i < m->n_rows; i++)
        if (!um_toast_row_is_storage(&m->rows[i])) return 1;
    return 0;
}

int um_toast_visible_row(const um_toast_model *m, const um_toast_state *s,
                         int visible_index)
{
    int i, n = 0;
    if (!m || visible_index < 0) return -1;
    for (i = 0; i < m->n_rows; i++) {
        if (!s->show_others && !um_toast_row_is_storage(&m->rows[i])) continue;
        if (n == visible_index) return i;
        n++;
    }
    return -1;
}

int um_toast_first_openable_row(const um_toast_model *m, const um_toast_state *s)
{
    int i, total = um_toast_visible_count(m, s);
    for (i = 0; i < total; i++) {
        int row = um_toast_visible_row(m, s, i);
        if (row >= 0 && m->rows[row].openable) return row;
    }
    return -1;
}

int um_toast_rows_visible(const um_toast_model *m, const um_toast_state *s, int height)
{
    int rows, total = um_toast_visible_count(m, s);
    if (!total) return 0;
    if (!s->expanded) return 1;
    rows = (button_top(height) - 6 - row_top()) / UM_UI_ROW_H;
    if (rows < 1) rows = 1;
    return rows < total ? rows : total;
}

int um_toast_measure_height(const um_toast_model *m, const um_toast_state *s)
{
    int total = um_toast_visible_count(m, s);
    if (!m->n_rows && !m->overview) return m->status[0] ? 166 : 148;
    if (s->expanded && total > 1) {
        int height = row_top() + total * UM_UI_ROW_H + UM_UI_PAD + UM_UI_BTN_H + 6;
        return height < UM_UI_EXPANDED_H ? height : UM_UI_EXPANDED_H;
    }
    return UM_UI_COLLAPSED_H;
}

/* ------------------------------------------------------------- 组件 ------ */

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

/* 底部文字按钮：无边框、hover 才有浅底（原型 A/B/C 的 foot 样式）。 */
static void link_button(um_drawlist *out, const um_theme *t,
                        const um_toast_state *s, int x, int y, int w, int h,
                        um_ui_hit hit, const char *label)
{
    int hover = s->hover_btn == (int)hit;
    unsigned long bg = hover ? blend(t->panel2, t->text, 8) : t->panel;
    push_round(out, x, y, w, h, bg, 8, hit);
    push_text(out, x, y + (h - 16) / 2, w, UM_F_SMALL,
              hover ? t->text : t->muted, UM_A_CENTER, label);
}

/* 行内"打开卷"：透明底 + 细边框 + accent 文字。 */
static void row_open_button(um_drawlist *out, const um_theme *t,
                            const um_toast_state *s, int x, int y, int w, int h,
                            um_ui_hit hit, const char *label)
{
    int hover = s->hover_btn == (int)hit;
    unsigned long bg = hover ? blend(t->panel2, t->accent, 12) : t->panel2;
    push_round(out, x, y, w, h, bg, 8, hit);
    stroke(out, x, y, w, h, 8, hover ? t->accent : t->border);
    push_text(out, x, y + (h - 16) / 2, w, UM_F_SMALL, t->accent, UM_A_CENTER, label);
}

/* 右上角 ✖：24×24 命中区，两笔 2px 线。 */
static void close_button(um_drawlist *out, const um_theme *t,
                         const um_toast_state *s, int x, int y)
{
    int hover = s->hover_btn == (int)UM_HIT_CLOSE;
    unsigned long fg = hover ? t->text : t->muted;
    push_round(out, x, y, 24, 24,
               hover ? blend(t->panel2, t->text, 10) : t->panel, 7,
               UM_HIT_CLOSE);
    push_line(out, x + 7, y + 7, 10, 10, fg, 2, UM_HIT_NONE);
    push_line(out, x + 17, y + 7, -10, 10, fg, 2, UM_HIT_NONE);
}

/* 8 点旋转指示：图标位变成加载环（原型 D）。 */
static void spinner(um_drawlist *out, const um_theme *t,
                    const um_toast_state *s, int x, int y, int size)
{
    static const int dx[8] = { 0, 9, 13, 9, 0, -9, -13, -9 };
    static const int dy[8] = { -13, -9, 0, 9, 13, 9, 0, -9 };
    int cx = x + size / 2, cy = y + size / 2, i;
    push_round(out, x, y, size, size, t->panel2, 12, UM_HIT_NONE);
    for (i = 0; i < 8; i++) {
        int k = (i + s->spinner_phase) & 7;
        unsigned long c = blend(t->panel2, t->accent, 15 + k * 11);
        push_round(out, cx + dx[i] - 1, cy + dy[i] - 1, 3, 3, c, 2,
                   UM_HIT_NONE);
    }
}

/* 底部 3px 剩余时间条：暂停时停住并转暖色。 */
static void progress_bar(um_drawlist *out, const um_theme *t,
                         const um_toast_state *s, int width, int height)
{
    int fill = 0;
    if (s->ttl_ms > 0 && s->remaining_ms > 0) {
        int remain = s->remaining_ms > s->ttl_ms ? s->ttl_ms : s->remaining_ms;
        fill = (int)((long long)width * remain / s->ttl_ms);
        if (fill < 1) fill = 1;
    }
    push_rect(out, 0, height - 3, width, 3, t->progress, UM_HIT_NONE);
    if (fill > 0)
        push_rect(out, 0, height - 3, fill, 3,
                  s->paused ? t->warn : t->accent, UM_HIT_NONE);
}

/* --------------------------------------------------------------- 布局器 --- */

void um_toast_layout(const um_toast_model *m, const um_toast_state *s,
                     const um_theme *t, int width, int height, um_drawlist *out)
{
    const int p = UM_UI_PAD;
    const int light = theme_is_light(t);
    int by = button_top(height);
    int total = um_toast_visible_count(m, s);
    int has_other = um_toast_has_other(m);
    int rows = um_toast_rows_visible(m, s, height);
    int first = s->expanded ? s->scroll_px / UM_UI_ROW_H : 0;
    int first_open = um_toast_first_openable_row(m, s);
    int i;
    unsigned long accent = accent_of(t, m->accent_kind);
    const char *subtitle = (s->show_others && m->subtitle_all[0])
                           ? m->subtitle_all : m->subtitle;
    const char *count = (s->show_others && m->count_all[0])
                        ? m->count_all : m->count;
    const char *summary = (s->show_others && m->summary_all[0])
                          ? m->summary_all : m->summary;
    char summary_buf[UM_UI_MAX_TEXT];
    int badge_w = count[0] ? 60 : 0;   /* 54 徽章 + 6 间距 */
    int head_w;

    out->n = 0;
    push_round(out, 0, 0, width, height, t->panel, 18, UM_HIT_NONE);
    stroke(out, 1, 1, width - 2, height - 2, 17, t->border);

    /* 标题区：图标 / 标题 / 副标题 / 数量徽章 / 右上 ✖ */
    push_round(out, p, p, 38, 38, t->panel2, 12, UM_HIT_NONE);
    if (m->scanning) {
        spinner(out, t, s, p, p, 38);
    } else {
        push_icon(out, p + 6, p + 6, 27, 27,
                  m->accent_kind == 1 ? "add" :
                  m->accent_kind == 3 ? "remove" : "usb",
                  accent, t->text, light);
    }
    head_w = width - (p + 49) - p - badge_w - 24 - 6;
    if (head_w < 60) head_w = 60;
    push_text(out, p + 49, p + 1, head_w, UM_F_TITLE,
              t->text, UM_A_LEFT, m->headline);
    push_text(out, p + 49, p + 24, head_w, UM_F_SMALL,
              t->muted, UM_A_LEFT, subtitle);
    if (badge_w) {
        push_round(out, width - p - 24 - 6 - 54, p + 3, 54, 26,
                   t->panel2, 12, UM_HIT_NONE);
        push_text(out, width - p - 24 - 6 - 54, p + 9, 54, UM_F_SMALL,
                  t->text, UM_A_CENTER, count);
    }
    close_button(out, t, s, width - p - 24, p + 1);

    /* 摘要行 */
    if (!summary[0] && !m->status[0]) {
        if (total > 0)
            snprintf(summary_buf, sizeof summary_buf, "%d 个存储设备已就绪", total);
        else if (m->n_rows)
            snprintf(summary_buf, sizeof summary_buf, "%s", "未检测到存储设备");
        else
            snprintf(summary_buf, sizeof summary_buf, "%s", "当前没有检测到外接设备");
        summary = summary_buf;
    }
    if (m->scanning) {
        if (summary[0]) {
            push_round(out, p, 62, 3, 18, accent, 2, UM_HIT_NONE);
            push_text(out, p + 12, 63, width - 2 * p - 12, UM_F_BODY,
                      t->muted, UM_A_LEFT, summary);
        }
        if (m->status[0])
            push_text(out, p + 12, 86, width - 2 * p - 12, UM_F_SMALL,
                      t->muted, UM_A_LEFT, m->status);
    } else if (m->status[0] || summary[0]) {
        push_round(out, p, 62, 3, 18, accent, 2, UM_HIT_NONE);
        push_text(out, p + 12, 63, width - 2 * p - 12, UM_F_BODY,
                  t->muted, UM_A_LEFT, m->status[0] ? m->status : summary);
    }

    /* 0 行：纯文字信息框 / 扫描提示，点任意处关闭 */
    if (!m->n_rows && !m->overview) {
        progress_bar(out, t, s, width, height);
        return;
    }

    /* 行区 */
    if (total == 0) {
        push_round(out, p, row_top(), width - 2 * p, UM_UI_ROW_H - 8,
                   t->panel2, 12, UM_HIT_NONE);
        push_text(out, p, row_top() + 18, width - 2 * p, UM_F_BODY,
                  t->muted, UM_A_CENTER,
                  m->n_rows ? "未检测到存储设备" : "接入设备后会自动更新");
        if (has_other)
            push_text(out, p, row_top() + 42, width - 2 * p, UM_F_SMALL,
                      t->muted, UM_A_CENTER, "点击下方“显示其他设备”查看全部");
    } else {
        if (first + rows > total) first = total - rows;
        if (first < 0) first = 0;
        for (i = 0; i < rows; i++) {
            int index = um_toast_visible_row(m, s, first + i);
            int y = row_top() + i * UM_UI_ROW_H;
            const um_volume *v;
            int row_w, row_h, right, text_w, stor, show_open, show_tag;
            int ctrl_w, meta_w, hover;
            unsigned long edge;
            const char *cap = "";
            char cap_buf[64];

            if (index < 0) continue;
            v = &m->rows[index];
            row_w = width - 2 * p - (s->expanded && total > rows ? 8 : 0);
            row_h = UM_UI_ROW_H - 8;
            right = p + row_w;
            stor = um_toast_row_is_storage(v);
            show_open = s->expanded && stor && v->openable;
            show_tag = !stor;
            ctrl_w = show_open ? 74 : show_tag ? 68 : 0;
            meta_w = (stor && (v->total || v->capacity[0])) ? 76 : 0;
            text_w = row_w - 58 - ctrl_w - meta_w - 8;
            if (text_w < 40) text_w = 40;
            hover = index == s->hover_row || index == s->focus_row;
            edge = hover ? t->accent : t->border;

            push_round(out, p, y, row_w, row_h, t->panel2, 12,
                       (um_ui_hit)(UM_HIT_ROW + index));
            stroke(out, p, y, row_w, row_h, 12, edge);
            push_round(out, p + 12, y + 13, 29, 29,
                       blend(t->panel2, (stor && v->openable) ? t->accent : t->muted, 10),
                       9, UM_HIT_NONE);
            push_icon(out, p + 16, y + 17, 21, 21, icon_status_for(v->kind),
                      v->openable ? t->accent : t->muted, t->text, light);
            push_text(out, p + 54, y + 10, text_w, UM_F_ROWTITLE,
                      t->text, UM_A_LEFT, v->title);
            push_text(out, p + 54, y + 30, text_w, UM_F_SMALL,
                      t->muted, UM_A_LEFT, v->subtitle);

            if (stor) {
                if (v->total) {
                    um_ui_format_bytes(v->total, cap_buf, sizeof cap_buf);
                    cap = cap_buf;
                } else if (v->capacity[0]) {
                    cap = v->capacity;
                }
                if (cap[0])
                    push_text(out, right - ctrl_w - 72, y + 13, 68,
                              UM_F_SMALL, t->muted, UM_A_RIGHT, cap);
                if (v->total && v->pct >= 0) {
                    int pct = v->pct > 100 ? 100 : v->pct;
                    push_round(out, right - ctrl_w - 68, y + 35, 64, 4,
                               t->progress, 2, UM_HIT_NONE);
                    if (pct > 0)
                        push_round(out, right - ctrl_w - 68, y + 35,
                                   64 * pct / 100, 4, accent, 2, UM_HIT_NONE);
                }
            }
            if (show_tag) {
                push_round(out, right - 64, y + 27, 52, 22, t->panel2, 11,
                           UM_HIT_NONE);
                stroke(out, right - 64, y + 27, 52, 22, 11, t->border);
                push_text(out, right - 64, y + 31, 52, UM_F_SMALL,
                          t->muted, UM_A_CENTER, "非存储");
            }
            if (show_open)
                row_open_button(out, t, s, right - 74, y + 12, 62, 31,
                                (um_ui_hit)(UM_HIT_ROW_OPEN + index), "打开卷");
        }
        if (s->expanded && rows > 0 && rows < total) {
            int track = rows * UM_UI_ROW_H - 8;
            int thumb = track * rows / total;
            if (thumb < 16) thumb = 16;
            push_round(out, width - p - 4, row_top(), 4, track,
                       t->progress, 2, UM_HIT_NONE);
            push_round(out, width - p - 4,
                       row_top() + (track - thumb) * first / (total - rows),
                       4, thumb, t->muted, 2, UM_HIT_NONE);
        }
    }

    /* 底部：收起/展开、显示其他设备、打开U盘 */
    {
        int lx = p;
        if (total > 1) {
            link_button(out, t, s, lx, by + 5, 90, 32, UM_HIT_EXPAND,
                        s->expanded ? "收起列表" : "展开列表");
            lx += 98;
        }
        if (has_other)
            link_button(out, t, s, lx, by + 5, 112, 32, UM_HIT_SHOW_OTHERS,
                        s->show_others ? "隐藏其他设备" : "显示其他设备");
        if (!s->expanded && first_open >= 0)
            button(out, t, s, width - p - 118, by, 118, UM_UI_BTN_H,
                   UM_HIT_OPEN, "打开U盘", 1);
    }

    progress_bar(out, t, s, width, height);
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
    int total, rows;
    if (!s->expanded) return 0;
    total = um_toast_visible_count(m, s);
    rows = um_toast_rows_visible(m, s, height);
    return (total - rows > 0) ? (total - rows) * UM_UI_ROW_H : 0;
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
