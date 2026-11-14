/* win32_shim.h — 最小 Win32 API 仿真层（仅用于 Linux 上的 UI 测试）。
 *
 * 让 src/um_toast_win32.c 这份**生产代码**能在没有 Windows、没有 mingw 的
 * 机器上被编译、被真实地驱动（消息、定时器、菜单、点击），并把绘制指令导
 * 出成 SVG，用于视觉回归。实现在 win32_shim.c（共享状态必须唯一）。
 */
#ifndef WIN32_SHIM_H
#define WIN32_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>

typedef void *HWND;
typedef void *HDC;
typedef void *HINSTANCE;
typedef void *HRGN;
typedef void *HBITMAP;
typedef void *HBRUSH;
typedef void *HPEN;
typedef void *HFONT;
typedef void *HMENU;
typedef void *HMODULE;
typedef void *HKEY;
typedef uint32_t DWORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef unsigned int UINT;
typedef unsigned long COLORREF;
typedef long LONG;
typedef long LONG_PTR;
typedef unsigned long long WPARAM;
typedef long long LPARAM;
typedef long long LRESULT;
typedef void *LPVOID;
typedef const wchar_t *LPCWSTR;
typedef wchar_t *LPWSTR;
typedef const void *LPCVOID;

#define TRUE 1
#define FALSE 0
#define CALLBACK
#define WINAPI
/* 与真 wingdi.h 一致：NULL_BRUSH 是 stock object 索引 5（原来这里定义成
 * (HBRUSH)0，而生产代码用 shim 专造的 NULL_BRUSH_OBJ——真机编译必炸）。
 * GetStockObject(5) 在 shim 里返回哨兵 (void*)1，RoundRect 据此走“只描边”。 */
#define NULL_BRUSH 5

typedef struct { long left, top, right, bottom; } RECT;
typedef struct { long x, y; } POINT;

/* 多显示器 / DPI 仿真：只有一块"屏"，但工作区与 DPI 可注入。 */
typedef void *HMONITOR;
typedef struct { long cx, cy; } SIZE;
#define MONITOR_DEFAULTTONEAREST 2
typedef struct {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} MONITORINFO;

/* 跟踪尺寸：生产代码在 WM_GETMINMAXINFO 里放宽上下限，保证自绘窗口
 * 可以任意 SetWindowPos 放大（DPI 缩放后尺寸可能超过样式派生上限）。 */
typedef struct {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
} MINMAXINFO;

#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_CLOSE         0x0010
#define WM_PAINT         0x000F
#define WM_ERASEBKGND    0x0014
#define WM_GETMINMAXINFO 0x0024
#define WM_TIMER         0x0113
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONUP     0x0202
#define WM_RBUTTONUP     0x0205
#define WM_MOUSEHOVER    0x02A1
#define WM_MOUSELEAVE    0x02A3
#define WM_KEYDOWN       0x0100
#define WM_DPICHANGED    0x02E0
#define WM_NULL          0x0000
#define WM_MOUSEWHEEL    0x020A
#define WHEEL_DELTA      120

#define VK_ESCAPE 0x1B
#define VK_RETURN 0x0D
#define VK_TAB    0x09
#define VK_UP     0x26
#define VK_DOWN   0x28

#define GWLP_USERDATA (-21)
#define TME_LEAVE 0x00000002
#define TME_HOVER 0x00000001
#define HOVER_DEFAULT 0xFFFFFFFF

typedef struct { DWORD cbSize; DWORD dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT;

#define WS_POPUP         0x80000000L
#define WS_EX_TOPMOST    0x00000008L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_LAYERED    0x00080000L
#define SW_HIDE     0
#define SW_SHOWNOACTIVATE 4
#define LWA_ALPHA   0x00000002
#define SWP_NOSIZE  0x0001
#define SWP_NOMOVE  0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define HWND_TOPMOST    ((HWND)(LONG_PTR)-1)
#define HWND_NOTOPMOST  ((HWND)(LONG_PTR)-2)
#define IDC_ARROW 32512
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SPI_GETWORKAREA 0x0030
#define LOGPIXELSY 90
#define SRCCOPY 0x00CC0020
#define PS_SOLID 0
#define TRANSPARENT 1
#define DT_SINGLELINE 0x00000020
#define DT_NOPREFIX   0x00000800
#define DT_END_ELLIPSIS 0x00008000
#define DT_LEFT   0x00000000
#define DT_RIGHT  0x00000002
#define DT_CENTER 0x00000001
#define FW_NORMAL 400
#define FW_SEMIBOLD 600
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define CLEARTYPE_QUALITY 5
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0
#define MF_STRING 0x00000000L
#define MF_SEPARATOR 0x00000800L
#define MF_GRAYED   0x00000001L
#define TPM_RETURNCMD 0x0100
#define TPM_RIGHTBUTTON 0x0002

typedef struct {
    UINT cbSize; DWORD style; LONG_PTR lpfnWndProc; int cbClsExtra;
    int cbWndExtra; HINSTANCE hInstance; void *hIcon; void *hCursor;
    HBRUSH hbrBackground; LPCWSTR lpszMenuName; LPCWSTR lpszClassName;
} WNDCLASSW;

typedef struct {
    HDC hdc; int fErase; RECT rcPaint;
    int fRestore, fIncUpdate; BYTE rgbReserved[32];
} PAINTSTRUCT;

#define LOWORD(l) ((unsigned short)(((unsigned long long)(l)) & 0xFFFF))
#define HIWORD(l) ((unsigned short)((((unsigned long long)(l)) >> 16) & 0xFFFF))
#define MulDiv(a,b,c) ((int)(((long long)(a) * (long long)(b)) / (long long)(c)))

typedef LRESULT (*SHIM_WNDPROC)(HWND, UINT, WPARAM, LPARAM);

/* --------------------------------------------------------- 仿真状态/绘制 -- */

#define SHIM_MAXOBJ 64
#define SHIM_MAXDRAW 1024

typedef struct {
    int kind;              /* 0 rect 1 roundrect 2 text 3 ellipse 4 line */
    int x, y, w, h, r;
    unsigned long fill;    /* COLORREF(0xBBGGRR) —— 与 GDI 一致 */
    int stroke;
    unsigned long stroke_col;
    int fontpx, bold;
    int align;             /* 0 left 1 right 2 center */
    char text[256];
} shim_draw;

extern shim_draw g_draws[SHIM_MAXDRAW];
extern int       g_ndraws;
extern HWND      g_painting;

/* ------------------------------------------------------------- 仿真 API --- */

void    shim_reset(void);
void    shim_svg(const char *path, int width, int height, const char *bg);
LRESULT shim_send(HWND hw, UINT msg, WPARAM w, LPARAM l);
void    shim_paint(HWND hw);
HWND    shim_window(void *userdata, int w, int h);
void   *shim_userdata(HWND hw);
int     shim_visible(HWND hw);
void    shim_set_next_menu_result(int cmd);
int     shim_menu_item_count(void);
/* 查询仿真窗口几何（SetWindowPos 后生效；供几何/锚点回归断言）。 */
void    shim_window_geometry(int *x, int *y, int *w, int *h);
/* 注入"当前显示器"：屏幕 + 工作区尺寸（0,0,w,h）。默认 1920x1080 屏幕 /
 * 1920x1040 工作区（40px 任务栏）/ 96dpi，与既有几何断言一致。 */
void    shim_set_workarea(int w, int h);
/* 注入 DPI（<16 视为无效 → 96；注意 75 是合法的低 dpi）。 */
void    shim_set_dpi(int dpi);
/* CreateFontW 字号日志（顺序 = paint_toast 的 UM_F_* 顺序：
 * 0=标题16, 1=正文13, 2=小字12, 3=按钮13, 4=行标题13），px = -lfHeight。 */
int     shim_font_log_count(void);
int     shim_font_log_px(int idx);

/* --------------------------------------------------------- Win32 API 仿真 -- */

int      RegisterClassW(const WNDCLASSW *wc);
HWND     CreateWindowExW(DWORD ex, const wchar_t *cls, const wchar_t *name,
                         DWORD style, int x, int y, int w, int h, HWND parent,
                         HMENU menu, HINSTANCE inst, LPVOID p);
HMODULE  GetModuleHandleW(const wchar_t *n);
void    *LoadCursorW(HINSTANCE h, LPCWSTR n);
void    *GetStockObject(int i);
LONG_PTR SetWindowLongPtrW(HWND hw, int idx, LONG_PTR v);
LONG_PTR GetWindowLongPtrW(HWND hw, int idx);
UINT     SetTimer(HWND hw, UINT id, UINT elapse, void *fn);
BOOL     KillTimer(HWND hw, UINT id);
BOOL     ShowWindow(HWND hw, int cmd);
BOOL     InvalidateRect(HWND hw, const RECT *r, BOOL e);
BOOL     SetWindowPos(HWND hw, HWND after, int x, int y, int w, int h, UINT f);
BOOL     SetForegroundWindow(HWND hw);
BOOL     PostMessageW(HWND hw, UINT msg, WPARAM wp, LPARAM lp);
HRGN     CreateRoundRectRgn(int a, int b, int c, int d, int e, int f);
int      SetWindowRgn(HWND hw, HRGN r, BOOL redraw);
BOOL     SetLayeredWindowAttributes(HWND hw, COLORREF key, BYTE a, DWORD f);
BOOL     TrackMouseEvent(TRACKMOUSEEVENT *t);
BOOL     ClientToScreen(HWND hw, POINT *p);
BOOL     SystemParametersInfoW(UINT a, UINT b, void *c, UINT d);
int      GetSystemMetrics(int i);
HDC      GetDC(HWND hw);
int      ReleaseDC(HWND hw, HDC dc);
int      GetDeviceCaps(HDC dc, int idx);
BOOL     GetCursorPos(POINT *pt);
HMONITOR MonitorFromPoint(POINT pt, DWORD flags);
BOOL     GetMonitorInfoA(HMONITOR mon, MONITORINFO *mi);
BOOL     GetWindowRect(HWND hw, RECT *rc);
BOOL     DestroyWindow(HWND hw);
LRESULT  DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l);

HBRUSH   CreateSolidBrush(COLORREF c);
HPEN     CreatePen(int style, int w, COLORREF c);
HFONT    CreateFontW(int h, int w, int esc, int orient, int weight, DWORD it,
                     DWORD ul, DWORD st, DWORD charset, DWORD outp, DWORD clip,
                     DWORD quality, DWORD pitch, LPCWSTR face);
void     DeleteObject(void *o);
void    *SelectObject(HDC dc, void *obj);
int      SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF c);
int      FillRect(HDC dc, const RECT *rc, HBRUSH br);
BOOL     RoundRect(HDC dc, int x1, int y1, int x2, int y2, int w, int h);
BOOL     Ellipse(HDC dc, int x1, int y1, int x2, int y2);
BOOL     Rectangle(HDC dc, int x1, int y1, int x2, int y2);
BOOL     MoveToEx(HDC dc, int x, int y, POINT *old);
BOOL     LineTo(HDC dc, int x, int y);
int      DrawTextW(HDC dc, const uint16_t *text, int len, RECT *rc, UINT fmt);
BOOL     GetTextExtentPoint32W(HDC dc, const uint16_t *text, int len, SIZE *sz);
HDC      CreateCompatibleDC(HDC dc);
HBITMAP  CreateCompatibleBitmap(HDC dc, int w, int h);
BOOL     BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy,
                DWORD op);
BOOL     DeleteDC(HDC dc);
HDC      BeginPaint(HWND hw, PAINTSTRUCT *ps);
BOOL     EndPaint(HWND hw, const PAINTSTRUCT *ps);

HMENU    CreatePopupMenu(void);
BOOL     AppendMenuW(HMENU m, UINT f, unsigned long long id, LPCWSTR t);
BOOL     DestroyMenu(HMENU m);
int      TrackPopupMenuEx(HMENU m, UINT f, int x, int y, HWND hw, void *p);

/* SetupAPI and disk IO used by the shared Windows inventory. */
typedef void *HDEVINFO;
typedef void *HANDLE;
typedef uint32_t DEVINST;
typedef uint32_t CONFIGRET;
typedef struct { int dummy; } GUID;
typedef union { uint64_t QuadPart; } ULARGE_INTEGER;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { DWORD cbSize; DEVINST DevInst; } SP_DEVINFO_DATA;
typedef struct { DWORD cbSize; int index; } SP_DEVICE_INTERFACE_DATA;
typedef struct { DWORD cbSize; wchar_t DevicePath[1]; } SP_DEVICE_INTERFACE_DETAIL_DATA_W;

#define MAX_PATH 260
#define MAX_DEVICE_ID_LEN 200
#define INVALID_HANDLE_VALUE ((void *)-1)
#define IF_DISK 1
#define IF_VOLUME 2
#define IF_HID 3
#define IF_NET 4
#define IF_HUB 5

enum { BusTypeUsb = 7, BusTypeSd = 12, BusTypeMmc = 13 };
#define BUS_TYPE_UNKNOWN 0
#define BUS_TYPE_USB BusTypeUsb
#define BUS_TYPE_SD BusTypeSd
#define BUS_TYPE_MMC BusTypeMmc
#define BUS_TYPE_SATA 11

enum { CR_SUCCESS, CR_NO_SUCH_DEVNODE, CR_REMOVE_VETOED };
enum { ERROR_NO_MORE_ITEMS = 259, ERROR_INSUFFICIENT_BUFFER = 122,
       ERROR_MORE_DATA = 234, ERROR_NO_SUCH_DEVICE = 433, ERROR_DEVICE_NOT_CONNECTED = 1167,
       ERROR_NOT_READY = 21, ERROR_NO_MEDIA_IN_DRIVE = 1112 };
#define ERROR_NO_SUCH_DEVINST 0xE000020Bu
#define DIGCF_PRESENT 2
#define DIGCF_DEVICEINTERFACE 16
#define SPDRP_FRIENDLYNAME 12
#define SPDRP_DEVICEDESC 0
#define CM_DRP_CAPABILITIES 16
#define CM_LOCATE_DEVNODE_NORMAL 0
#define CM_DEVCAP_REMOVABLE 4
#define CM_DEVCAP_DOCKDEVICE 8
#define SEM_FAILCRITICALERRORS 1
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_EXISTING 3
#define IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 0x560000
#define IOCTL_STORAGE_QUERY_PROPERTY 0x2D1400
#define IOCTL_STORAGE_GET_DEVICE_NUMBER 0x2D1080
#define IOCTL_DISK_GET_DRIVE_GEOMETRY_EX 0x700A0
#define IOCTL_STORAGE_CHECK_VERIFY2 0x2D0800

typedef struct { DWORD DeviceType, DeviceNumber, PartitionNumber; } STORAGE_DEVICE_NUMBER;
typedef struct { DWORD PropertyId, QueryType; BYTE AdditionalParameters[1]; } STORAGE_PROPERTY_QUERY;
typedef struct {
    DWORD Version, Size;
    BYTE DeviceType, DeviceTypeModifier, RemovableMedia, CommandQueueing;
    DWORD VendorIdOffset, ProductIdOffset, ProductRevisionOffset, SerialNumberOffset;
    DWORD BusType, RawPropertiesLength;
    BYTE RawDeviceProperties[1];
} STORAGE_DEVICE_DESCRIPTOR;
typedef struct { DWORD DiskNumber; LARGE_INTEGER Offset, Length; } DISK_EXTENT;
typedef struct { DWORD NumberOfDiskExtents; DISK_EXTENT Extents[1]; } VOLUME_DISK_EXTENTS;
typedef struct { int64_t geometry[3]; LARGE_INTEGER DiskSize; BYTE Data[1]; } DISK_GEOMETRY_EX;

typedef struct {
    const char *instance_id;
    const char *device_path;
    int iface;
    int disk_number;
    int bus_type;
    int removable;
    int media_present;
    int unlocked;
    const char *letters;       /* E:F: or a directory path */
    const char *parent_inst;
} shim_dev;

void shim_set_world(const shim_dev *devs, int n);
void shim_enum_failure(int fail);
void shim_eject_veto(int veto);
DEVINST shim_last_eject(void);
DWORD GetLastError(void);
BOOL SetThreadErrorMode(DWORD mode, DWORD *old);
HANDLE FindFirstVolumeW(wchar_t *buf, DWORD n);
BOOL FindNextVolumeW(HANDLE h, wchar_t *buf, DWORD n);
BOOL FindVolumeClose(HANDLE h);
BOOL GetVolumePathNamesForVolumeNameW(const wchar_t *vol, wchar_t *names, DWORD n, DWORD *ret);
BOOL GetDiskFreeSpaceExW(const wchar_t *path, ULARGE_INTEGER *available,
                        ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes);
HDEVINFO SetupDiGetClassDevsW(const GUID *guid, const wchar_t *enumerator, HWND hwnd, DWORD flags);
BOOL SetupDiEnumDeviceInterfaces(HDEVINFO set, void *a, const GUID *guid,
                                 DWORD idx, SP_DEVICE_INTERFACE_DATA *d);
BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO set, SP_DEVICE_INTERFACE_DATA *d,
                                      SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail,
                                      DWORD sz, DWORD *need, SP_DEVINFO_DATA *devinfo);
BOOL SetupDiGetDeviceRegistryPropertyW(HDEVINFO set, SP_DEVINFO_DATA *info, DWORD prop,
                                       DWORD *type, BYTE *buf, DWORD size, DWORD *need);
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO set);
CONFIGRET CM_Get_Parent(DEVINST *parent, DEVINST child, DWORD flags);
CONFIGRET CM_Get_Device_IDW(DEVINST inst, wchar_t *buf, DWORD n, DWORD flags);
CONFIGRET CM_Locate_DevNodeW(DEVINST *inst, wchar_t *id, DWORD flags);
CONFIGRET CM_Get_DevNode_Registry_PropertyW(DEVINST inst, DWORD prop, DWORD *type,
                                           void *out, DWORD *size, DWORD flags);
CONFIGRET CM_Request_Device_EjectW(DEVINST inst, void *type, wchar_t *name, DWORD n, DWORD flags);
HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share,
                   void *sa, DWORD disp, DWORD flags, void *tmpl);
BOOL DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD in_sz,
                      void *out, DWORD out_sz, DWORD *ret, void *ov);
BOOL CloseHandle(HANDLE h);

#endif /* WIN32_SHIM_H */
