#ifndef UM_WIN_APP_H
#define UM_WIN_APP_H

#include "um_enum.h"
#include <windows.h>

enum {
    UMWM_QUIT = WM_APP + 2,
    UMWM_TRAY,
    UMWM_TRAY_RESCAN,
    UMWM_TRAY_QUIT,
    UMWM_PANEL,
    UMWM_TRAY_EJECT_TEST,
    UMWM_EJECT_DONE
};

void um_tray_install(HWND owner, um_gui *gui);
void um_tray_uninstall(void);
int um_tray_filter(UINT msg, WPARAM wp, LPARAM lp);
FILE *um_win_test_output(const char *variable);
void um_tray_test_log(const char *key, const char *detail);
void um_tray_open(char letter, int disk, int reveal);
void um_tray_eject(const char *instance);
void um_tray_finish_eject(unsigned long result);
void um_gui_win_notice(const char *title, const char *body, int accent);
void um_gui_win_open_panel(void);
/* 托盘菜单“显示其他设备”：只改面板视图，不主动弹出面板。 */
void um_gui_win_set_show_others(int on);
/* 托盘侧切换并持久化偏好；菜单项与测试用 WM_COMMAND 共用。 */
void um_tray_toggle_show_others(void);
const um_toast_model *um_gui_win_inventory(void);
void um_request_stop(void);

/* 托盘命令 ID（与 tray_win32.c 的私有枚举保持一致）。 */
#define UM_TRAY_ID_PANEL  1
#define UM_TRAY_ID_OTHERS 7

#endif
