/* nc_ui - Win32 shell that runs the NC screen exactly as the LVDS panel shows
   it, plus the proprietary key row the machine has in hardware.

   The window has two parts:
     - the emulated panel (800x600) rendered by modules/nc/nc_visual.c through
       the host LVDS backend, unmodified layout;
     - a side keypad: F1-F6 jump to the operation modes, F7-F12 are the soft
       keys, and the 3x3 numeric pad plus the control keys mirror the machine
       keyboard.

   nc_ui --dump out.bmp   render one frame headlessly (layout smoke test)
   nc_ui                  open the window
   */

#include <stdio.h>
#include <string.h>

/* windows.h defines FORCEINLINE with a storage class, which clashes with the
   core's `static FORCEINLINE` declarations. Parse windows.h first, then let the
   firmware headers define it their own way. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef FORCEINLINE

#include "cnc.h"
#include "file_system.h"
#include "nc_run.h"
#include "nc_menu.h"
#include "nc_visual.h"
#include "host_fs.h"
#include "lvds_host.h"

#define PANEL_W 800
#define PANEL_H 600
#define SIDE_W 300
#define WIN_W (PANEL_W + SIDE_W)
#define WIN_H PANEL_H
#define TIMER_ID 1
#define TIMER_MS 30

/* A host key is either an NC key or a direct mode jump. */
typedef struct {
    int x;
    int y;
    int w;
    int h;
    const char *label;
    nc_visual_key_t key;
    nc_mode_t mode;   /* used when key == NC_VISUAL_KEY_NONE and mode >= 0 */
} host_button_t;

static host_button_t g_buttons[] = {
    /* Operation modes: F1-F6 (firmware keeps the MODE key as well). */
    {  12,  40, 132, 30, "F1 MANUAL", NC_VISUAL_KEY_NONE, NC_MODE_MANUAL },
    { 156,  40, 132, 30, "F2 EDIT",   NC_VISUAL_KEY_NONE, NC_MODE_PROGRAM },
    {  12,  76, 132, 30, "F3 SIM",    NC_VISUAL_KEY_NONE, NC_MODE_SIM },
    { 156,  76, 132, 30, "F4 MDI",    NC_VISUAL_KEY_NONE, NC_MODE_MDI },
    {  12, 112, 132, 30, "F5 TOOLS",  NC_VISUAL_KEY_NONE, NC_MODE_TOOLS },
    { 156, 112, 132, 30, "F6 RUN",    NC_VISUAL_KEY_NONE, NC_MODE_RUN },

    /* Soft keys F7-F12: provisional mapping onto the NC key model. */
    {  12, 166, 276, 26, "F7  ACCEPT",   NC_VISUAL_KEY_ACCEPT,    (nc_mode_t)-1 },
    {  12, 196, 276, 26, "F8  NEXT",     NC_VISUAL_KEY_NEXT,      (nc_mode_t)-1 },
    {  12, 226, 276, 26, "F9  PREV",     NC_VISUAL_KEY_PREV,      (nc_mode_t)-1 },
    {  12, 256, 276, 26, "F10 FINISH",   NC_VISUAL_KEY_FINISH,    (nc_mode_t)-1 },
    {  12, 286, 276, 26, "F11 BACKSPACE",NC_VISUAL_KEY_BACKSPACE, (nc_mode_t)-1 },
    {  12, 316, 276, 26, "F12 MODE",     NC_VISUAL_KEY_MODE,      (nc_mode_t)-1 },

    /* The 3x3 pad is drawn dynamically (see host_pad_*): it shows the footer
       menu of the active mode, or digits while a value is being typed. */

    /* Control keys. */
    {  12, 518,  88, 32, "0",       NC_VISUAL_KEY_DIGIT_0,  (nc_mode_t)-1 },
    { 106, 518,  88, 32, "ENTER",   NC_VISUAL_KEY_ACCEPT,   (nc_mode_t)-1 },
    { 200, 518,  88, 32, "BACK",    NC_VISUAL_KEY_BACKSPACE,(nc_mode_t)-1 },
    {  12, 556,  88, 32, "PAGE -",  NC_VISUAL_KEY_PREV,     (nc_mode_t)-1 },
    { 106, 556,  88, 32, "END",     NC_VISUAL_KEY_FINISH,   (nc_mode_t)-1 },
    { 200, 556,  88, 32, "PAGE +",  NC_VISUAL_KEY_NEXT,     (nc_mode_t)-1 }
};

static const int g_button_count = (int)(sizeof(g_buttons) / sizeof(g_buttons[0]));

/* 3x3 pad geometry: cell 0..8, row major. */
#define PAD_X0 12
#define PAD_Y0 380
#define PAD_W 88
#define PAD_H 40
#define PAD_GAP_X 6
#define PAD_GAP_Y 6

static int host_pad_cell(int x, int y)
{
    int col = (x - PAD_X0) / (PAD_W + PAD_GAP_X);
    int row = (y - PAD_Y0) / (PAD_H + PAD_GAP_Y);
    int local_x = (x - PAD_X0) % (PAD_W + PAD_GAP_X);
    int local_y = (y - PAD_Y0) % (PAD_H + PAD_GAP_Y);

    if (x < PAD_X0 || y < PAD_Y0 || col > 2 || row > 2)
        return -1;
    if (local_x >= PAD_W || local_y >= PAD_H)
        return -1;
    return row * 3 + col;
}

static void host_pad_click(int cell)
{
    size_t count = 0u;
    const nc_footer_item_t *footer;

    if (cell < 0 || cell > 8)
        return;
    if (nc_visual_value_editing()) {
        nc_visual_handle_key((nc_visual_key_t)(NC_VISUAL_KEY_DIGIT_1 + cell));
        return;
    }
    footer = nc_visual_footer(&count);
    if (footer && (size_t)cell < count)
        nc_visual_footer_action((nc_footer_action_t)footer[cell].action);
}

static void host_send_button(const host_button_t *button)
{
    if (!button)
        return;
    if (button->key != NC_VISUAL_KEY_NONE) {
        nc_visual_handle_key(button->key);
        return;
    }
    if (button->mode >= 0 && button->mode < NC_MODE_COUNT)
        nc_visual_select_mode(button->mode);
}

static nc_visual_key_t host_key_for_vk(WPARAM vk, bool *handled)
{
    *handled = true;
    switch (vk) {
    case VK_RETURN: return NC_VISUAL_KEY_ACCEPT;
    case VK_ESCAPE: return NC_VISUAL_KEY_MODE;
    case VK_BACK:   return NC_VISUAL_KEY_BACKSPACE;
    case VK_DELETE: return NC_VISUAL_KEY_FINISH;
    case VK_UP:
    case VK_PRIOR:  return NC_VISUAL_KEY_FIELD_PREV;
    case VK_DOWN:
    case VK_NEXT:   return NC_VISUAL_KEY_FIELD_NEXT;
    /* Line and argument movement, as on a normal editor. */
    case VK_LEFT:   return NC_VISUAL_KEY_WORD_PREV;
    case VK_RIGHT:  return NC_VISUAL_KEY_WORD_NEXT;
        default: break;
    }
    if (vk >= '0' && vk <= '9')
        return (nc_visual_key_t)(NC_VISUAL_KEY_DIGIT_0 + (int)(vk - '0'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (nc_visual_key_t)(NC_VISUAL_KEY_DIGIT_0 + (int)(vk - VK_NUMPAD0));
    /* Dedicated sign and point, so value entry does not depend on the footer
       letters (which the machine pad overloads as UP/DOWN). */
    if (vk == VK_SUBTRACT || vk == VK_OEM_MINUS)
        return NC_VISUAL_KEY_MINUS;
    if (vk == VK_DECIMAL || vk == VK_OEM_PERIOD)
        return NC_VISUAL_KEY_DOT;
    *handled = false;
    return NC_VISUAL_KEY_NONE;
}

static void host_apply_vk(HWND hwnd, WPARAM vk)
{
    int i;

    /* F1-F6 select the operation modes directly, like the Heidenhain pilot
       row this panel copies. */
    if (vk >= VK_F1 && vk <= VK_F6) {
        static const nc_mode_t modes[6] = {
            NC_MODE_MANUAL, NC_MODE_PROGRAM, NC_MODE_SIM,
            NC_MODE_MDI, NC_MODE_TOOLS, NC_MODE_RUN
        };
        nc_visual_select_mode(modes[vk - VK_F1]);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (vk >= VK_F7 && vk <= VK_F12) {
        for (i = 0; i < g_button_count; i++) {
            if (g_buttons[i].key != NC_VISUAL_KEY_NONE &&
                strncmp(g_buttons[i].label, "F", 1u) == 0) {
                int index = 0;
                if (sscanf(g_buttons[i].label + 1, "%d", &index) == 1 &&
                    index == (int)(vk - VK_F1 + 1)) {
                    host_send_button(&g_buttons[i]);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return;
                }
            }
        }
        return;
    }
    {
        bool handled = false;
        nc_visual_key_t key = host_key_for_vk(vk, &handled);
        if (handled) {
            nc_visual_handle_key(key);
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

static void host_draw_side(HDC dc)
{
    HBRUSH panel = CreateSolidBrush(RGB(24, 26, 28));
    HBRUSH button = CreateSolidBrush(RGB(58, 62, 66));
    HBRUSH mode_button = CreateSolidBrush(RGB(34, 96, 60));
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(120, 124, 128));
    HFONT font = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    RECT panel_rect = { PANEL_W, 0, WIN_W, WIN_H };
    HGDIOBJ old_font;
    int i;

    FillRect(dc, &panel_rect, panel);
    old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 232, 234));
    TextOutA(dc, PANEL_W + 12, 12, "NC panel keys", 13);
    SetTextColor(dc, RGB(160, 164, 168));
    TextOutA(dc, PANEL_W + 12, 340,
             nc_visual_value_editing() ? "3x3 keypad: digits" :
                                         "3x3 keypad: footer menu", 29);

    /* Dynamic pad: footer menu of the active mode, or digits during value
       entry - the conversational behaviour the machine screen has. */
    {
        size_t footer_count = 0u;
        const nc_footer_item_t *footer = nc_visual_footer(&footer_count);
        bool editing = nc_visual_value_editing();
        int cell;

        for (cell = 0; cell < 9; cell++) {
            const char *label = NULL;
            char text[32];
            int col = cell % 3;
            int row = cell / 3;
            RECT r = { PANEL_W + PAD_X0 + col * (PAD_W + PAD_GAP_X),
                       PAD_Y0 + row * (PAD_H + PAD_GAP_Y),
                       PANEL_W + PAD_X0 + col * (PAD_W + PAD_GAP_X) + PAD_W,
                       PAD_Y0 + row * (PAD_H + PAD_GAP_Y) + PAD_H };
            HGDIOBJ old_brush;
            HGDIOBJ old_pen;

            if (editing) {
                snprintf(text, sizeof(text), "%d", cell + 1);
                label = text;
            } else if (footer && (size_t)cell < footer_count) {
                snprintf(text, sizeof(text), "%c %s", footer[cell].key,
                         footer[cell].label);
                label = text;
            }
            if (!label)
                continue;
            old_brush = SelectObject(dc, button);
            old_pen = SelectObject(dc, edge);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            SetTextColor(dc, RGB(238, 240, 242));
            DrawTextA(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    for (i = 0; i < g_button_count; i++) {
        const host_button_t *b = &g_buttons[i];
        RECT r = { PANEL_W + b->x, b->y, PANEL_W + b->x + b->w, b->y + b->h };
        HGDIOBJ old_brush = SelectObject(dc, b->key == NC_VISUAL_KEY_NONE ? mode_button : button);
        HGDIOBJ old_pen = SelectObject(dc, edge);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        SetTextColor(dc, RGB(238, 240, 242));
        DrawTextA(dc, b->label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteObject(edge);
    DeleteObject(button);
    DeleteObject(mode_button);
    DeleteObject(panel);
}

static void host_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    BITMAPINFO info;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = lvds_host_width();
    info.bmiHeader.biHeight = -lvds_host_height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    SetDIBitsToDevice(dc, 0, 0, (DWORD)lvds_host_width(), (DWORD)lvds_host_height(),
                      0, 0, 0, (UINT)lvds_host_height(), lvds_host_pixels(), &info,
                      DIB_RGB_COLORS);
    host_draw_side(dc);
    EndPaint(hwnd, &ps);
}

static void host_tick(HWND hwnd, unsigned now_ms)
{
    static unsigned last_draw_ms;
    bool periodic = nc_visual_periodic_needed() && (now_ms - last_draw_ms) >= 20u;

    if (nc_visual_dirty() || periodic) {
        last_draw_ms = now_ms;
        nc_visual_draw();
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK host_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        host_paint(hwnd);
        return 0;
    case WM_TIMER:
        cnc_dotasks();
        host_tick(hwnd, (unsigned)(GetTickCount64() & 0xFFFFFFFFu));
        return 0;
    case WM_KEYDOWN:
        host_apply_vk(hwnd, wp);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp) - PANEL_W;
        int y = (short)HIWORD(lp);
        int i;
        for (i = 0; i < g_button_count; i++) {
            const host_button_t *b = &g_buttons[i];
            if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
                host_send_button(b);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        host_pad_click(host_pad_cell(x, y));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static char g_files_root[260];

static void host_init_core(void)
{
    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    nc_run_init();
    nc_visual_init();
}

static int host_dump(const char *path)
{
    host_init_core();
    nc_visual_draw();
    if (!lvds_host_save_bmp(path)) {
        fprintf(stderr, "nc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("nc_ui: wrote %dx%d frame to %s\n", lvds_host_width(),
           lvds_host_height(), path);
    return 0;
}

/* Headless check that the desktop filesystem is mounted where the NC module
   expects it: list "/D" through the same fs_* API the file manager uses. */
static int host_fstest(void)
{
    fs_file_t *dir;
    fs_file_info_t info;
    unsigned count = 0u;

    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    printf("nc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");
    dir = fs_opendir("/D");
    if (!dir) {
        puts("nc_ui: /D is not mounted");
        return 1;
    }
    while (fs_next_file(dir, &info)) {
        printf("  %-4s %8lu  %s\n", info.is_dir ? "dir" : "file",
               (unsigned long)info.size, info.full_name);
        count++;
    }
    fs_close(dir);
    printf("nc_ui: %u entries\n", count);
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    if (argc >= 3 && strcmp(argv[1], "--dump") == 0)
        return host_dump(argv[2]);

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--files") == 0) {
            snprintf(g_files_root, sizeof(g_files_root), "%s", argv[i + 1]);
            i++;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fstest") == 0)
            return host_fstest();
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = host_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "nc_ui_host";
    if (!RegisterClassA(&wc))
        return 1;

    hwnd = CreateWindowExA(0, wc.lpszClassName, "uCNC NC panel (host)",
                           WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           WIN_W + 16, WIN_H + 60, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return 1;

    lvds_host_attach_window(hwnd);
    host_init_core();
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, TIMER_ID, TIMER_MS, NULL);
    InvalidateRect(hwnd, NULL, FALSE);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
