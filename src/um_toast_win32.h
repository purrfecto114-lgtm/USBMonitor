/* um_toast_win32.h — Win32 通知浮层（1.1.1 ToastWindow 的 C 移植）。 */
#ifndef UM_TOAST_WIN32_H
#define UM_TOAST_WIN32_H

#include "um_toast_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UM_ACT_NONE = 0,
    UM_ACT_OPEN,
    UM_ACT_OPEN_ROW,
    UM_ACT_REVEAL,
    UM_ACT_COPY,
    UM_ACT_EJECT,
    UM_ACT_TOGGLE_EXPAND,
    UM_ACT_CLOSE,
    UM_ACT_AUTOHIDE
} um_toast_action;

typedef void (*um_toast_cb)(um_toast_action act, int row, void *user);

typedef struct um_toast_win um_toast_win;

/* 注册窗口类（进程内一次）。内部会先请求 PerMonitorV2 DPI 感知。 */
int  um_toast_win_init(void *hinstance);

/* 显式声明 PerMonitorV2 DPI 感知。必须在创建任何窗口之前调用（um_toast_win_init
 * 已经代为调用，这里只是给需要更早生效的宿主用）。
 *
 * 为什么用运行期 API 而不是内嵌 RT_MANIFEST：GNU ld 总会自己合成一份来自
 * mingw CRT 的默认清单（binutils 2.47 实测：.rc 里写 `1 24 "usbmon.manifest"`
 * 链接必然报 ".rsrc merge failure: multiple non-default manifests"，且 ld
 * 没有任何开关能关掉它自己那份）。运行期 API 效果等价，且与工具链无关。
 * 返回 1 = 已获得 per-monitor（V2 或兼容回退）感知。 */
int  um_toast_win_enable_dpi_awareness(void);

/* 系统是否处于深色模式（AppsUseLightTheme == 0）。 */
int  um_toast_system_dark(void);

/* 创建（不显示）：model 拷贝进窗口，theme 同理。
 * width 为设计空间（96DPI）像素宽，0 = 默认 UM_UI_WIDTH；
 * ttl_ms 为倒计时，0 = 默认 10s。
 * n_rows == 0 的模型渲染成"信息框"（纯文字，无按钮，点任意处/Esc 关闭，
 * 窗口标题 "usbmon-toast"）；n_rows > 0 是设备面板（标题 "usbmon"）。 */
um_toast_win *um_toast_win_new(const um_toast_model *m, const um_theme *t,
                               int topmost, int width, int ttl_ms,
                               um_toast_cb cb, void *user);

/* 信息框推荐宽度：按 headline/subtitle/summary 实测文本宽度推导，夹在
 * [300, UM_UI_WIDTH]（设计空间像素）。 */
int  um_toast_suggest_width(const um_toast_model *m);

/* 槽位叠放：槽 n（1..N-1）相对槽 0 向上偏移（高度 + 12px，按 fit 缩放）。
 * slot <= 0 不偏移。 */
void um_toast_win_move_slot(um_toast_win *w, int slot);

/* 带基线偏移的槽位叠放：base_px > 0 时先在基线上方留出 base_px（设备像素，
 * 例如"面板当前高度"），再按 slot 叠放，两段间隙都按 fit 缩放。设备面板可
 * 见时用它把通知整体叠到面板上方，避免 2.5.0 统一边距后精确重叠。 */
void um_toast_win_move_slot_ex(um_toast_win *w, int slot, int base_px);

/* 窗口当前物理高度（设备像素，含 fit/dpi）；失败返回 0。用于取面板占位。 */
int  um_toast_win_pixel_height(const um_toast_win *w);

/* 当前锚定显示器（光标所在屏）的工作区；参数是调用方提供的 RECT*。
 * 用 void* 是为了本头文件不必引入 windows.h。返回 1 表示成功。 */
int  um_toast_monitor_workarea(void *rc_work);

/* 新事件到达时刷新内容（1.1.1 的 consume/refresh）。 */
void um_toast_win_update(um_toast_win *w, const um_toast_model *m);

/* 显示（右下角工作区锚点 + 淡入 + 倒计时）。 */
void um_toast_win_show(um_toast_win *w);
void um_toast_win_hide(um_toast_win *w);
void um_toast_win_set_theme(um_toast_win *w, const um_theme *t);
void um_toast_win_destroy(um_toast_win *w);

/* 供外部（托盘/设置）驱动的状态查询与操作 */
int  um_toast_win_is_visible(const um_toast_win *w);
int  um_toast_win_height(const um_toast_win *w);
int  um_toast_win_is_expanded(const um_toast_win *w);
void um_toast_win_toggle_expand(um_toast_win *w);

/* 窗口句柄（真机冒烟测试直接投递 WM_KEYDOWN/菜单消息用；可为 NULL）。 */
void *um_toast_win_hwnd(const um_toast_win *w);

/* 透传给回调的 user 指针（NULL 安全；槽位替换时回收旧 carrier 用）。 */
void *um_toast_win_user(const um_toast_win *w);

/* 交互状态只读视图（宿主/测试断言 scroll/hover/焦点用；恒非 NULL）。 */
const um_toast_state *um_toast_win_state(const um_toast_win *w);

/* 模型只读视图（宿主回调按行取盘符/标题用；恒非 NULL）。 */
const um_toast_model *um_toast_win_model(const um_toast_win *w);

#ifdef __cplusplus
}
#endif

#endif /* UM_TOAST_WIN32_H */
