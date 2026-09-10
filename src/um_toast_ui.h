/* um_toast_ui.h — platform-neutral toast UI kernel for usbmon (C99).
 *
 * 把 v1.1.1 (Python/PySide6) ToastWindow / VolumeRow / Theme 里"看得见、
 * 点得着"的部分抽成一层不依赖任何窗口系统的 C 代码：
 *
 *   model  (数据)  ──►  layout (几何)  ──►  draw list (图元)  ──► 后端绘制
 *                            │
 *                            └──► hit test (命中)  ──► 状态机 (交互)
 *
 * 这样带来的好处：
 *   - 布局/交互/文案可以在 Linux 上编译并跑单元测试（守护进程永不链接 X11
 *     的既定设计不被破坏）；
 *   - Win32 / X11 / 甚至 WebView2 渲染后端只负责"把图元画出来 + 转发输入"，
 *     换后端不动业务逻辑；
 *   - 与 1.1.1 的文案、配色、尺寸一一对应，视觉回归有据可依。
 *
 * 颜色统一用 0xRRGGBB（与 1.1.1 的 CSS 十六进制一致），后端自行转成
 * GDI 的 COLORREF(0xBBGGRR) 或 X11 的像素值。
 */
#ifndef UM_TOAST_UI_H
#define UM_TOAST_UI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM_UI_MAX_ROWS   16
#define UM_UI_MAX_TEXT   256
#define UM_UI_MAX_LETTER 8

/* ------------------------------------------------------------------ 主题 -- */

typedef struct {
    const char *name;      /* "dark" / "light"                       */
    unsigned long panel, panel2, text, muted, border;
    unsigned long accent, accent_hover, progress;
    unsigned long ok, warn, error;
    unsigned long shadow;  /* 0xAARRGGBB                              */
} um_theme;

/* requested: "auto" / "dark" / "light"；system_dark = 系统是否深色（auto 时用） */
void um_theme_resolve(um_theme *t, const char *requested, int system_dark);

/* ------------------------------------------------------- 设备分类（新增）-- */
/*
 * 设备树是事实源，卷是存储视图，盘符只是可选挂载点。
 * 一个 USB 设备可同时暴露多个功能接口，因此用**位掩码**而非互斥枚举：
 * 智能笔（HID）带一个配置盘（Storage）是完全正常的复合设备。
 */
#define UM_TAG_USB      0x01   /* 设备树上是 USB 枚举器 / 有 USB 祖先 */
#define UM_TAG_STORAGE  0x02   /* GUID_DEVINTERFACE_DISK             */
#define UM_TAG_VOLUME   0x04   /* GUID_DEVINTERFACE_VOLUME           */
#define UM_TAG_HID      0x08   /* GUID_DEVINTERFACE_HID（笔/数位板） */
#define UM_TAG_NET      0x10   /* GUID_DEVINTERFACE_NET（无线网卡等）*/
#define UM_TAG_HUB      0x20   /* GUID_DEVINTERFACE_USB_HUB（拓展坞）*/

typedef enum {
    UM_KIND_USB_STORAGE = 0,   /* USB 存储，有盘符，可打开可弹出     */
    UM_KIND_STORAGE_NO_LETTER, /* USB 存储但未分配盘符 / 未挂载      */
    UM_KIND_EMPTY_SLOT,        /* 读卡器空槽（有设备无介质）         */
    UM_KIND_LOCKED_VOLUME,     /* BitLocker 等未解锁卷               */
    UM_KIND_DOCK_HUB,          /* 拓展坞 / 集线器                    */
    UM_KIND_NET_ADAPTER,       /* USB 无线网卡                       */
    UM_KIND_HID_PEN,           /* 智能笔 / HID 设备                  */
    UM_KIND_UNKNOWN            /* 证据不足，不纳入 USB 存储          */
} um_dev_kind;

/* 决策树：给定证据，输出设备类别。
 *   tags          接口标签位掩码（UM_TAG_*）
 *   has_letter    是否至少有一个盘符
 *   bus_is_usb    IOCTL_STORAGE_QUERY_PROPERTY 的 BusType == BusTypeUsb
 *   removable     RemovableMedia（或 CM_DEVCAP_REMOVABLE）
 *   media_present 介质是否就绪（读卡器空槽 = 0）
 *   unlocked      卷是否已解锁（BitLocker 未解锁 = 0）
 * 证据不足一律判 UNKNOWN，绝不退化成"DRIVE_REMOVABLE 就算 USB 存储"的弱启发。
 */
um_dev_kind um_ui_classify(unsigned tags, int has_letter, int bus_is_usb,
                           int removable, int media_present, int unlocked);

/* 能力判定：非存储设备一律不可打开/不可弹出（对网卡、笔、拓展坞弹介质
 * 既无意义也会失败）。 */
int um_ui_can_open(um_dev_kind kind);
int um_ui_can_eject(um_dev_kind kind, int removable);

/* 行提示文案（无盘符 / 无存储 / 读卡器空槽 / 未解锁…） */
void um_ui_device_notice(um_dev_kind kind, char *out, size_t n);

/* 1.1.1 原文：remove 事件但没有卷变化时 → "该设备可能尚未分配盘符。" */
void um_ui_remove_notice(char *out, size_t n);

/* ------------------------------------------------------------------ 模型 -- */

typedef struct {
    char  path[16];        /* "E:\\"                                   */
    char  title[96];       /* "Kingston DataTraveler 3.0 (E:)"         */
    char  subtitle[160];   /* "可移动磁盘 · E:\\"                       */
    char  capacity[128];   /* "容量 62.7 GB · 可用 20.1 GB · 68%"      */
    char  tooltip[256];    /* 进度条 tooltip                           */
    unsigned long long total, used, free;
    int   pct;             /* 0..100，-1 = 未知（不确定进度条）        */
    unsigned     tags;     /* UM_TAG_* 位掩码                          */
    um_dev_kind  kind;
    int   disk_number;     /* 物理磁盘号（多分区聚合键），-1 = 未知    */
    int   n_letters;       /* 该物理磁盘上的盘符数（多分区）           */
    char  letters[UM_UI_MAX_LETTER][4];   /* "E:" "F:" "G:"            */
    int   removable;       /* RemovableMedia / CM_DEVCAP_REMOVABLE     */
    int   ejectable;       /* 可安全弹出                               */
    int   openable;        /* 可打开（有盘符且文件系统可用）           */
} um_volume;

/* 按物理磁盘聚合后的"设备行"（多分区 U 盘只显示一行） */
typedef struct {
    int   disk_number;
    char  title[96];
    int   n_letters;
    char  letters[UM_UI_MAX_LETTER][4];
    unsigned long long total, used, free;
    int   pct;
    unsigned     tags;
    um_dev_kind  kind;
    int   ejectable, openable;
} um_device_row;

/* 多分区聚合：以 DiskNumber 为键，把同一物理磁盘上的多个卷合成一行。
 * 返回写入 rows 的数量。 */
int  um_ui_group_by_disk(const um_volume *vols, int n_vols,
                         um_device_row *rows, int max_rows);

/* 生成设备行副标题："可移动磁盘 · E:、F:、G:（3 个分区）" */
void um_ui_row_subtitle(const um_device_row *row, char *out, size_t n);

typedef struct {
    char       headline[UM_UI_MAX_TEXT];   /* "USB 设备监控" / "USB 已连接"  */
    char       subtitle[UM_UI_MAX_TEXT];   /* "2 个设备 · 3 个卷"            */
    char       summary[UM_UI_MAX_TEXT];    /* "USB 已连接：E:、F:"           */
    char       status[UM_UI_MAX_TEXT];     /* "正在安全弹出 E:…"（可为空）   */
    char       count[32];                  /* 右上角 "3 个"                  */
    int        accent_kind;                /* 0=accent 1=ok 2=warn 3=error   */
    um_volume  rows[UM_UI_MAX_ROWS];
    int        n_rows;
} um_toast_model;

/* -------------------------------------------------------------- 交互状态 -- */

typedef struct {
    int  visible;
    int  expanded;          /* 折叠=设备视图，展开=分区视图（1.1.1 同款） */
    int  hover_row;         /* -1 = 无                                  */
    int  hover_btn;         /* um_ui_hit，0 = 无                        */
    int  focus_row;         /* 键盘焦点所在行，-1 = 焦点在按钮区         */
    int  focus_btn;         /* 键盘焦点所在按钮                          */
    int  paused;            /* hover 暂停倒计时                          */
    int  remaining_ms;      /* 剩余自动隐藏时间                          */
    int  ttl_ms;            /* 总时长（默认 10000，与 1.1.1 AUTO_HIDE_MS）*/
    int  scroll_px;         /* 行区滚动偏移                              */
} um_toast_state;

void um_toast_state_init(um_toast_state *s, int ttl_ms);

/* 倒计时推进：dt_ms 毫秒。返回 1 表示"该自动隐藏了"。 */
int  um_toast_tick(um_toast_state *s, int dt_ms);
void um_toast_pause(um_toast_state *s);
void um_toast_resume(um_toast_state *s);

/* -------------------------------------------------------------- 命中测试 -- */

typedef enum {
    UM_HIT_NONE   = 0,
    UM_HIT_OPEN   = 1,     /* 底部主按钮 "打开U盘"      */
    UM_HIT_EXPAND = 2,     /* "展开" / "折叠"           */
    UM_HIT_CLOSE  = 3,     /* "关闭"                    */
    UM_HIT_ROW    = 100,   /* + 行号（0..UM_UI_MAX_ROWS）*/
    UM_HIT_ROW_OPEN = 200  /* + 行号：行内"打开"按钮（展开态） */
} um_ui_hit;

/* ------------------------------------------------------------ 绘制图元层 -- */

typedef enum {
    UM_D_RECT, UM_D_ROUNDRECT, UM_D_LINE, UM_D_TEXT, UM_D_ICON
} um_draw_kind;

typedef enum {
    UM_F_TITLE = 0,   /* 16px bold   */
    UM_F_BODY,        /* 13px        */
    UM_F_SMALL,       /* 12px muted  */
    UM_F_BTN,         /* 13px semibold */
    UM_F_ROWTITLE     /* 13px semibold */
} um_font;

typedef enum { UM_A_LEFT = 0, UM_A_RIGHT, UM_A_CENTER } um_align;

typedef struct {
    um_draw_kind kind;
    int x, y, w, h;
    unsigned long color;      /* 0xRRGGBB（矩形填充/边框/文字）      */
    unsigned long color2;     /* 渐变/边框备用；icon 时用作主色      */
    int radius;               /* roundrect 圆角                      */
    um_font  font;
    um_align align;
    int      clip;            /* 文本超出用 "…" 截断                 */
    int      stroke;          /* 1 = 只描边（边框），0 = 填充        */
    char     text[UM_UI_MAX_TEXT];
    um_ui_hit hit;            /* 该图元对应的可点击区域（0 = 不可点） */
} um_draw;

typedef struct {
    um_draw items[256];
    int     n;
} um_drawlist;

/* ---------------------------------------------------------------- 布局器 -- */

/* 逻辑像素（96 DPI）下的窗口尺寸常量，取自 1.1.1（px(v) = v * 0.88 * scale）。 */
#define UM_UI_WIDTH          440
#define UM_UI_PAD            16
#define UM_UI_ROW_H          62
#define UM_UI_BTN_H          42
#define UM_UI_COLLAPSED_H    230
#define UM_UI_EXPANDED_H     430
#define UM_UI_ROWS_MAX_H     285
#define UM_UI_ROWS_COLLAPSED_H 105
#define UM_UI_MARGIN         18   /* 距屏幕工作区边缘 */

/* 计算窗口内容高度（未乘缩放）。 */
int  um_toast_measure_height(const um_toast_model *m, const um_toast_state *s);

/* 生成绘制图元。width/height 为逻辑尺寸；scale 为 DPI 缩放分子（96=1x）。 */
void um_toast_layout(const um_toast_model *m, const um_toast_state *s,
                     const um_theme *t, int width, int height,
                     um_drawlist *out);

/* 命中测试：x/y 为逻辑坐标。 */
um_ui_hit um_toast_hit_test(const um_toast_model *m, const um_toast_state *s,
                            int width, int height, int x, int y);

/* 展开态行区滚动的上界（scroll_px 的合法最大值；折叠态恒为 0）。
 * 与 um_toast_layout 同一套几何：行顶 = PAD+46+20+8(+18)，行底 = 按钮上沿-10。 */
int  um_toast_max_scroll(const um_toast_model *m, const um_toast_state *s,
                         int height);

/* ------------------------------------------------------------ 文案工具集 -- */

void um_ui_format_bytes(unsigned long long v, char *out, size_t n);
void um_ui_countdown(int remaining_ms, char *out, size_t n);
void um_ui_capacity_line(const um_volume *v, char *out, size_t n);

/* -------------------------------------------------------- UTF-8 → UTF-16 -- */
/*
 * 后端绘制需要 UTF-16。历史上这里逐字节 cast（(wchar_t)(unsigned char)c），
 * 把 "设"(E8 AE BE) 变成三个拉丁码点，渲染成 "è®¾"。
 * 本函数做真正的 UTF-8 解码：BMP 出 1 个码元，非 BMP 出代理对（2 个码元）。
 * 返回写入的 UTF-16 码元数（不含终止 0）；缓冲区不足返回截断长度，不越界。
 */
int um_ui_utf8_to_utf16(const char *utf8, uint16_t *out, int max_units);

#ifdef __cplusplus
}
#endif

#endif /* UM_TOAST_UI_H */
