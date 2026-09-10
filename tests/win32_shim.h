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
typedef unsigned long DWORD;
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

#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_PAINT         0x000F
#define WM_ERASEBKGND    0x0014
#define WM_TIMER         0x0113
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONUP     0x0202
#define WM_RBUTTONUP     0x0205
#define WM_MOUSEHOVER    0x02A1
#define WM_MOUSELEAVE    0x02A3
#define WM_KEYDOWN       0x0100
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

#endif /* WIN32_SHIM_H */

/* ==========================================================================
 * 设备枚举 / SetupAPI / CM_* 仿真（供 src/um_enum.c 在 Linux 上被真实驱动）
 * ========================================================================== */

typedef void *HDEVINFO;
typedef unsigned long ULONG_PTR;
typedef unsigned long DEVINST;
typedef unsigned long CONFIGRET;
typedef void *PHANDLE;
typedef unsigned long long ULONGLONG;

#define MAX_PATH        260
#define MAX_DEVICE_ID_LEN 200
#define INVALID_HANDLE_VALUE ((void *)-1)

typedef struct { DWORD cbSize; void *Reserved; } SP_DEVINFO_DATA_SHIM;
typedef struct { DWORD cbSize; DWORD Flags; } SP_DEVICE_INTERFACE_DATA_SHIM;

/* 接口类（仿真用代号，真机上为 GUID） */
#define IF_DISK   1
#define IF_VOLUME 2
#define IF_HID    3
#define IF_NET    4
#define IF_HUB    5

/* bus types */
#define BUS_TYPE_UNKNOWN 0
#define BUS_TYPE_USB     7
#define BUS_TYPE_SD      12
#define BUS_TYPE_MMC     13
#define BUS_TYPE_SATA    11

#define CR_SUCCESS 0

/* IOCTL codes（仿真） */
#define IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 0x560000
#define IOCTL_STORAGE_QUERY_PROPERTY         0x2D1400
#define IOCTL_STORAGE_GET_DEVICE_NUMBER      0x2D1080

/* 可注入的"设备世界"，由测试编排 */
typedef struct {
    const char *instance_id;    /* 设备实例 ID（同 ID 视为同一物理设备） */
    const char *device_path;    /* 接口路径 */
    int         iface;          /* IF_DISK / IF_VOLUME / IF_HID / IF_NET / IF_HUB */
    int         disk_number;    /* 物理磁盘号，-1 = 非存储 */
    int         bus_type;       /* BUS_TYPE_* */
    int         removable;      /* RemovableMedia */
    int         media_present;  /* 介质就绪（读卡器空槽 = 0） */
    int         unlocked;       /* 卷已解锁 */
    const char *letters;        /* "E:F:G:" 或 ""（未分配盘符） */
    const char *parent_inst;    /* 父设备实例 ID（NULL = 无父）USB 笔/网卡的
                                   父节点通常是 USB\VID_xxx 或 USB 集线器 */
} shim_dev;

void shim_set_world(const shim_dev *devs, int n);

/* --- 卷枚举 --- */
void        *FindFirstVolumeW(wchar_t *buf, DWORD n);
int          FindNextVolumeW(void *h, wchar_t *buf, DWORD n);
int          FindVolumeClose(void *h);
int          GetVolumePathNamesForVolumeNameW(const wchar_t *vol,
                                              wchar_t *names, DWORD n,
                                              DWORD *ret);

/* --- SetupAPI --- */
HDEVINFO     SetupDiGetClassDevsW(const void *guid, const wchar_t *enumerator,
                                  void *hwnd, DWORD flags);
int          SetupDiEnumDeviceInterfaces(HDEVINFO set, void *a,
                                         const void *guid, DWORD idx,
                                         SP_DEVICE_INTERFACE_DATA_SHIM *d);
int          SetupDiGetDeviceInterfaceDetailW(HDEVINFO set,
                                              SP_DEVICE_INTERFACE_DATA_SHIM *d,
                                              void *detail, DWORD sz,
                                              DWORD *need, void *devinfo);
int          SetupDiDestroyDeviceInfoList(HDEVINFO set);

/* --- CM_* --- */
CONFIGRET    CM_Get_Parent(DEVINST *parent, DEVINST child, DWORD flags);
CONFIGRET    CM_Get_Device_IDW(DEVINST inst, wchar_t *buf, DWORD n, DWORD flags);
DEVINST      shim_devnode_of(const char *instance_id);

/* --- 设备 I/O --- */
void        *CreateFileW(const wchar_t *path, DWORD access, DWORD share,
                         void *sa, DWORD disp, DWORD flags, void *tmpl);
int          DeviceIoControl(void *h, DWORD code, void *in, DWORD in_sz,
                             void *out, DWORD out_sz, DWORD *ret, void *ov);
int          CloseHandleShim(void *h);
