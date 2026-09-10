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

/* 注册窗口类（进程内一次）。 */
int  um_toast_win_init(void *hinstance);

/* 系统是否处于深色模式（AppsUseLightTheme == 0）。 */
int  um_toast_system_dark(void);

/* 创建（不显示）：model 拷贝进窗口，theme 同理。 */
um_toast_win *um_toast_win_new(const um_toast_model *m, const um_theme *t,
                               int topmost, um_toast_cb cb, void *user);

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

/* 交互状态只读视图（宿主/测试断言 scroll/hover/焦点用；恒非 NULL）。 */
const um_toast_state *um_toast_win_state(const um_toast_win *w);

/* 模型只读视图（宿主回调按行取盘符/标题用；恒非 NULL）。 */
const um_toast_model *um_toast_win_model(const um_toast_win *w);

#ifdef __cplusplus
}
#endif

#endif /* UM_TOAST_WIN32_H */
