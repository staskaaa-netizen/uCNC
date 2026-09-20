/* nc_ui - Win32 shell that runs the NC screen exactly as the LVDS panel shows
   it, plus the machine's own keypad beside it.

   The window has two parts:
     - the emulated panel (800x600) rendered by modules/nc/nc_visual.c through
       the host LVDS backend, unmodified layout;
     - the keys the proprietary keyboard has in hardware: F1-F4 jump between
       the operation modes and the 4x4 keypad (the matrix cam_keyboard.c
       decodes: `*0#D` / `123C` / `456B` / `789A`) sits beside the panel. The
       pad asks the screen what each key means (footer label, or MANUAL's own
       jog hint) and sends the key code nc_module.c would send, so what is
       pressed here is what is pressed on the machine.

   nc_ui --dump out.bmp   render one frame headlessly (layout smoke test)
   nc_ui --files DIR      mount DIR as the /D drive (default: .\nc-files)
   nc_ui [--keys LIST] --dump out.bmp
                          press LIST (comma separated: the keypad's own keys
                          `0`-`9`, `*`, `#`, `A`-`D`, the modes F1-F4, or named
                          keys ACCEPT/NEXT/PREV/FINISH/BACK/CANCEL/MODE/UP/
                          DOWN/LEFT/RIGHT/MINUS/DOT) before rendering, so a
                          screen that only appears after input can be checked
   nc_ui --fstest         list /D through the firmware fs_* API
   nc_ui --presettest     check the /D/presets.txt contract
   nc_ui --streamtest     check the panel's one-shot blocks reach the reader
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
#include "core/interpolator.h"
#include "core/planner.h"
#include "file_system.h"
#include "nc.h"
#include "nc_state.h"
#include "nc_presets.h"
#include "nc_run.h"
#include "nc_menu.h"
#include "nc_emit.h"
#include "nc_g7x.h"
#include "nc_tools.h"
#include "nc_visual.h"
#include "host_fs.h"
#include "lvds_host.h"
#include "modules/cam_keyboard/cam_keyboard.h"

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
    /* Operation modes: F1-F4 (firmware keeps the MODE key as well). */
    {  12,  40, 132, 30, "F1 MANUAL", NC_VISUAL_KEY_NONE, NC_MODE_MANUAL },
    { 156,  40, 132, 30, "F2 EDIT",   NC_VISUAL_KEY_NONE, NC_MODE_PROGRAM },
    {  12,  76, 132, 30, "F3 TOOLS",  NC_VISUAL_KEY_NONE, NC_MODE_TOOLS },
    { 156,  76, 132, 30, "F4 RUN",    NC_VISUAL_KEY_NONE, NC_MODE_RUN },
    {  12, 112, 276, 30, "",          NC_VISUAL_KEY_NONE, (nc_mode_t)-1 }

    /* The machine keypad is drawn and clicked by character (see host_pad_*):
       it is the hardware's own key row, not a second soft-key strip. */
};

static const int g_button_count = (int)(sizeof(g_buttons) / sizeof(g_buttons[0]));

/* The machine keypad, as cam_keyboard.c decodes it: a 4x4 matrix with the
   first row on top. The shell draws these keys and sends these key codes, so
   the bench presses what the machine presses - including the letter keys
   (A cancel, B/C step, D accept) that the footer labels name. */
#define PAD_COLS 4
#define PAD_ROWS 4

static const char g_pad_keys[PAD_ROWS][PAD_COLS + 1] = {
    "*0#D",
    "123C",
    "456B",
    "789A"
};

/* Pad geometry: cell (row, col), row major, top row first. */
#define PAD_X0 12
#define PAD_Y0 166
#define PAD_CELL_W 64
#define PAD_CELL_H 44
#define PAD_GAP_X 6
#define PAD_GAP_Y 6
#define PAD_PITCH_X (PAD_CELL_W + PAD_GAP_X)
#define PAD_PITCH_Y (PAD_CELL_H + PAD_GAP_Y)
#define PAD_CELL_RIGHT(col) (PAD_X0 + (col) * PAD_PITCH_X + PAD_CELL_W)
#define PAD_CELL_BOTTOM(row) (PAD_Y0 + (row) * PAD_PITCH_Y + PAD_CELL_H)

static bool host_pad_cell(int x, int y, int *row, int *col)
{
    int c;
    int r;

    if (x < PAD_X0 || y < PAD_Y0)
        return false;
    c = (x - PAD_X0) / PAD_PITCH_X;
    r = (y - PAD_Y0) / PAD_PITCH_Y;
    if (c >= PAD_COLS || r >= PAD_ROWS)
        return false;
    if ((x - PAD_X0) % PAD_PITCH_X >= PAD_CELL_W ||
        (y - PAD_Y0) % PAD_PITCH_Y >= PAD_CELL_H)
        return false;
    *row = r;
    *col = c;
    return true;
}

/* The key the machine keypad has down right now, 0 when none is. The shell
   reports it the way the keypad driver does, so a MANUAL feed can be held: the
   feed runs while the key is down and stops when it comes up. */
static char g_host_held_key;

static void host_hold_key(char key)
{
    if (key == g_host_held_key)
        return;
    g_host_held_key = key;
    nc_visual_hold_key(key);
}

static void host_release_key(char key)
{
    if (key != 0 && key == g_host_held_key)
        host_hold_key(0);
}

/* Pressing a pad key is pressing the machine key: the same character goes
   through the screen's own key table, whatever the active mode calls it. */
static void host_pad_click(int row, int col)
{
    nc_visual_key_t key;

    if (row < 0 || row >= PAD_ROWS || col < 0 || col >= PAD_COLS)
        return;
    key = nc_visual_key_for_char(g_pad_keys[row][col]);
    if (key != NC_VISUAL_KEY_NONE) {
        nc_visual_handle_key(key);
        host_hold_key(g_pad_keys[row][col]);
    }
}

/* What the key means right now: the footer entry that carries it, or the
   screen's own hint for a key the footer does not name (MANUAL's jog digits).
   The meanings live with the screen, never in this shell. */
static const char *host_key_label(char key)
{
    size_t count = 0u;
    const nc_footer_item_t *footer = nc_visual_footer(&count);
    const char *hint;
    size_t i;

    for (i = 0u; i < count; i++) {
        if (footer[i].key == key && footer[i].label[0] != '\0')
            return footer[i].label;
    }
    hint = nc_visual_key_hint(key);
    return (hint && hint[0]) ? hint : NULL;
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

/* The machine keypad key a PC key stands for, 0 for the keys only the host has
   (arrows, sign, point). Asked of the keyboard layout, so `*`, `#` and the
   letters A-D work on any layout. */
static char host_machine_key_for_vk(WPARAM vk, LPARAM lp)
{
    BYTE state[256];
    WORD chars[2];

    switch (vk) {
    case VK_RETURN: return 'D';
    case VK_ESCAPE: return 'A';
    case VK_BACK:   return '*';
    case VK_DELETE: return '#';
    default: break;
    }
    if (vk >= '0' && vk <= '9')
        return (char)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return (char)('0' + (int)(vk - VK_NUMPAD0));
    if (GetKeyboardState(state) &&
        ToAscii((UINT)vk, (UINT)lp, state, chars, 0) == 1) {
        char ch = (char)chars[0];

        if (ch >= 'a' && ch <= 'd')
            ch = (char)(ch - 'a' + 'A');
        if (nc_visual_key_for_char(ch) != NC_VISUAL_KEY_NONE)
            return ch;
    }
    return 0;
}

static nc_visual_key_t host_key_for_vk(WPARAM vk, LPARAM lp, bool *handled)
{
    char machine_key;

    *handled = true;
    switch (vk) {
    case VK_UP:
    case VK_PRIOR:  return NC_VISUAL_KEY_FIELD_PREV;
    case VK_DOWN:
    case VK_NEXT:   return NC_VISUAL_KEY_FIELD_NEXT;
    /* Line and argument movement, as on a normal editor. */
    case VK_LEFT:   return NC_VISUAL_KEY_WORD_PREV;
    case VK_RIGHT:  return NC_VISUAL_KEY_WORD_NEXT;
    /* Dedicated sign and point, so value entry does not depend on the footer
       letters (which the machine pad overloads as UP/DOWN). */
    case VK_SUBTRACT:
    case VK_OEM_MINUS:  return NC_VISUAL_KEY_MINUS;
    case VK_DECIMAL:
    case VK_OEM_PERIOD: return NC_VISUAL_KEY_DOT;
    default: break;
    }
    /* Everything else is a machine keypad key and goes through the screen's key
       table - the same one the on-screen pad and the machine keypad use. */
    machine_key = host_machine_key_for_vk(vk, lp);
    if (machine_key) {
        nc_visual_key_t mapped = nc_visual_key_for_char(machine_key);

        if (mapped != NC_VISUAL_KEY_NONE)
            return mapped;
    }
    *handled = false;
    return NC_VISUAL_KEY_NONE;
}

static void host_apply_vk(HWND hwnd, WPARAM vk, LPARAM lp)
{
    /* Holding a PC key repeats WM_KEYDOWN; the machine keypad does not. A
       repeat is therefore "still held", not another press - otherwise a held
       feed would restart itself. */
    bool repeat = (lp & (1L << 30)) != 0;

    /* F1-F4 select the operation modes directly, like the Heidenhain pilot
       row this panel copies. */
    if (vk >= VK_F1 && vk <= VK_F4) {
        static const nc_mode_t modes[4] = {
            NC_MODE_MANUAL, NC_MODE_PROGRAM,
            NC_MODE_TOOLS, NC_MODE_RUN
        };
        if (!repeat) {
            nc_visual_select_mode(modes[vk - VK_F1]);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return;
    }
    {
        char machine_key = host_machine_key_for_vk(vk, lp);
        bool handled = false;

        /* The keypad reports the key that is down, not just the press: this is
           what a held MANUAL feed runs on. */
        if (machine_key)
            host_hold_key(machine_key);
        if (repeat && machine_key)
            return;
        {
            nc_visual_key_t key = host_key_for_vk(vk, lp, &handled);

            if (handled) {
                nc_visual_handle_key(key);
                InvalidateRect(hwnd, NULL, FALSE);
            }
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
    HFONT small = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
    SelectObject(dc, small);
    TextOutA(dc, PANEL_W + 12, 148, "machine keypad", 14);
    SelectObject(dc, font);

    /* The keypad is the hardware, not a menu: the same 4x4 matrix the machine
       keyboard decodes, with the meaning the active screen gives each key. */
    {
        int row;
        int col;

        for (row = 0; row < PAD_ROWS; row++) {
            for (col = 0; col < PAD_COLS; col++) {
                char key = g_pad_keys[row][col];
                const char *label = host_key_label(key);
                RECT r = { PANEL_W + PAD_X0 + col * PAD_PITCH_X,
                           PAD_Y0 + row * PAD_PITCH_Y,
                           PANEL_W + PAD_CELL_RIGHT(col),
                           PAD_CELL_BOTTOM(row) };
                RECT key_rect = r;
                HGDIOBJ old_brush;
                HGDIOBJ old_pen;
                char text[2];

                old_brush = SelectObject(dc, button);
                old_pen = SelectObject(dc, edge);
                RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
                SelectObject(dc, old_brush);
                SelectObject(dc, old_pen);
                SetTextColor(dc, RGB(238, 240, 242));
                text[0] = key;
                text[1] = '\0';
                key_rect.bottom = key_rect.top + 24;
                DrawTextA(dc, text, 1, &key_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (label) {
                    RECT label_rect = r;

                    label_rect.top = label_rect.top + 22;
                    SetTextColor(dc, RGB(170, 196, 170));
                    SelectObject(dc, small);
                    DrawTextA(dc, label, -1, &label_rect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(dc, font);
                }
            }
        }
    }

    /* What the PC keyboard adds on top of the machine keys. */
    SetTextColor(dc, RGB(160, 164, 168));
    SelectObject(dc, small);
    {
        static const char *const help_keys[] = {
            "F1-F4", "arrows", "Enter", "Esc", "Backspace", "Del", "- ."
        };
        static const char *const help_means[] = {
            "modes", "line / field", "accept (D)", "cancel / mode (A)",
            "delete (*)", "finish (#)", "sign / point"
        };
        int line;

        TextOutA(dc, PANEL_W + 12, 384, "PC keyboard", 11);
        for (line = 0; line < (int)(sizeof(help_keys) / sizeof(help_keys[0])); line++) {
            int y = 402 + line * 16;
            TextOutA(dc, PANEL_W + 12, y, help_keys[line], (int)strlen(help_keys[line]));
            TextOutA(dc, PANEL_W + 84, y, help_means[line], (int)strlen(help_means[line]));
        }
    }
    SelectObject(dc, font);

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
    DeleteObject(small);
    DeleteObject(font);
    DeleteObject(edge);
    DeleteObject(button);
    DeleteObject(mode_button);
    DeleteObject(panel);
}

/* The whole bench: the emulated panel, then the machine keys beside it. */
static void host_draw_bench(HDC dc)
{
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
}

static void host_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    host_draw_bench(dc);
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

/* One tick of the firmware's main loop: parse what the panel queued and what a
   program stream holds, run the machine's tasks, and move the virtual clock.
   All three have to happen - with the parser or the clock missing, a jog is
   queued and never executed, which looks exactly like a feed that does not
   work. The machine keypad reports the key that is down every poll; the shell
   does the same here, so a feed also sees an alarm or a program take the
   reader. */
static void host_run_machine(unsigned elapsed_ms)
{
    unsigned i;

    nc_visual_hold_key(g_host_held_key);
    for (i = 0u; i < 8u && grbl_stream_available(); i++) {
        (void)cnc_parse_cmd();
    }
    cnc_dotasks();
    mcu_unit_test_advance_time(elapsed_ms * 1000u);
}

static LRESULT CALLBACK host_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        host_paint(hwnd);
        return 0;
    case WM_TIMER:
        host_run_machine(TIMER_MS);
        host_tick(hwnd, (unsigned)(GetTickCount64() & 0xFFFFFFFFu));
        return 0;
    case WM_KEYDOWN:
        host_apply_vk(hwnd, wp, lp);
        return 0;
    case WM_KEYUP:
        host_release_key(host_machine_key_for_vk(wp, lp));
        return 0;
    case WM_KILLFOCUS:
        /* Nothing is held once the keyboard is somewhere else: the machine
           keypad cannot lose a release like this. */
        host_hold_key(0);
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
        {
            int row = 0;
            int col = 0;

            if (host_pad_cell(x, y, &row, &col))
                host_pad_click(row, col);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        host_hold_key(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static char g_files_root[260];
static char g_key_script[160];
static unsigned g_ticks;

/* Replays the machine's own key path - the keypad's key characters through
   nc_visual_key_for_char(), and the mode keys - so what a dump shows is what
   the panel shows after the same presses. */
static void host_play_keys(const char *script)
{
    static const struct {
        const char *name;
        nc_visual_key_t key;
    } named[] = {
        { "ACCEPT", NC_VISUAL_KEY_ACCEPT },
        { "NEXT", NC_VISUAL_KEY_NEXT },
        { "PREV", NC_VISUAL_KEY_PREV },
        { "FINISH", NC_VISUAL_KEY_FINISH },
        { "BACK", NC_VISUAL_KEY_BACKSPACE },
        { "CANCEL", NC_VISUAL_KEY_CANCEL },
        { "MODE", NC_VISUAL_KEY_MODE },
        { "UP", NC_VISUAL_KEY_FIELD_PREV },
        { "DOWN", NC_VISUAL_KEY_FIELD_NEXT },
        { "LEFT", NC_VISUAL_KEY_WORD_PREV },
        { "RIGHT", NC_VISUAL_KEY_WORD_NEXT },
        { "MINUS", NC_VISUAL_KEY_MINUS },
        { "DOT", NC_VISUAL_KEY_DOT }
    };
    char buf[sizeof(g_key_script)];
    char *token;

    snprintf(buf, sizeof(buf), "%s", script);
    for (token = strtok(buf, ","); token; token = strtok(0, ",")) {
        size_t i;
        bool used = false;

        while (*token == ' ')
            token++;
        if (!token[0])
            continue;
        if (token[0] == 'F' && token[1] >= '1' && token[1] <= '4' && token[2] == '\0') {
            static const nc_mode_t modes[] = {
                NC_MODE_MANUAL, NC_MODE_PROGRAM,
                NC_MODE_TOOLS, NC_MODE_RUN
            };
            nc_visual_select_mode(modes[token[1] - '1']);
            continue;
        }
        /* `HOLD<key>` presses the machine key and keeps it down, `RELEASE`
           lets it go: the held feed needs both edges. */
        if (strncmp(token, "HOLD", 4) == 0 && token[4] != '\0' && token[5] == '\0') {
            char key = token[4];

            if (nc_visual_key_for_char(key) != NC_VISUAL_KEY_NONE) {
                nc_visual_handle_key(nc_visual_key_for_char(key));
                host_hold_key(key);
                continue;
            }
        }
        if (strcmp(token, "RELEASE") == 0) {
            host_hold_key(0);
            continue;
        }
        /* `WAIT<n>` lets the machine run between presses, so a script can set a
           stop, jog away from it and then feed back into it. */
        if (strncmp(token, "WAIT", 4) == 0 && token[4] >= '0' && token[4] <= '9') {
            unsigned ticks = (unsigned)strtoul(token + 4, NULL, 0);

            for (i = 0u; i < ticks; i++) {
                host_run_machine(TIMER_MS);
            }
            continue;
        }
        if (token[1] == '\0') {
            nc_visual_key_t key = nc_visual_key_for_char(token[0]);

            if (key != NC_VISUAL_KEY_NONE) {
                nc_visual_handle_key(key);
                continue;
            }
        }
        for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
            if (strcmp(token, named[i].name) == 0) {
                nc_visual_handle_key(named[i].key);
                used = true;
                break;
            }
        }
        if (!used)
            fprintf(stderr, "nc_ui: unknown key '%s'\n", token);
    }
}

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
    if (g_key_script[0]) {
        host_play_keys(g_key_script);
    }
    for (unsigned i = 0u; i < g_ticks; i++) {
        host_run_machine(TIMER_MS);
    }
    nc_visual_draw();
    if (!lvds_host_save_bmp(path)) {
        fprintf(stderr, "nc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("nc_ui: wrote %dx%d frame to %s\n", lvds_host_width(),
           lvds_host_height(), path);
    return 0;
}

/* The machine's main loop is `cnc_parse_cmd(); cnc_dotasks();` with the clock
   running, so a test that wants motion has to do all three. */
static void host_pump(unsigned iterations)
{
    unsigned i;

    for (i = 0u; i < iterations; i++) {
        (void)cnc_parse_cmd();
        cnc_dotasks();
        mcu_unit_test_advance_time(1000u);
    }
}

/* Let everything the panel queued run out: the parser takes one line per
   iteration and the planner needs the clock to execute what it took. */
static void host_pump_idle(unsigned max_iterations)
{
    unsigned i;

    for (i = 0u; i < max_iterations; i++) {
        if (!grbl_stream_available() && planner_buffer_is_empty() &&
            itp_is_empty()) {
            break;
        }
        host_pump(1u);
    }
    host_pump(20u);
}

/* Headless check of the held feed, on the real parser, planner and virtual MCU.

     1. the value keys change what the next block carries;
     2. a held direction key toward the stop sends one jog block covering the
        whole distance to it - the feed is the controller's jog, not a stream
        of short moves - and nothing else while the key stays down;
     3. the axis lands on the stop and does not cross it;
     4. on the stop the direction that would cross it is refused, and the other
        one feeds away (the wall is one-sided, so the axis is never locked);
     5. letting the key go cancels that feed where it stands.

   This proves the blocks the panel sends and the state it leaves behind. That
   the machine moves, and that it decelerates where the controller says, are
   bench items. */
static int host_feedtest(void)
{
    char expected[48];
    char reader[128];
    nc_runtime_state_t rt;
    double before_feed;
    double travel = (double)g_settings.max_distance[AXIS_X];
    /* The panel bounds a feed with no wall in front of it by the axis travel
       the machine states, or its own cap when it states none. */
    double limit = (travel > 1.0) ? travel : 25.0;
    size_t n = 0u;
    int failures = 0;
    int step;

    host_init_core();
    nc_visual_select_mode(NC_MODE_MANUAL);
    host_pump_idle(64u);
    nc_state_runtime(&rt);
    printf("feedtest: X starts at %.3f, the feed limit is %.1f mm\n",
           (double)rt.x, limit);

    /* Step away from the wall first: X+ in step mode (the default), one step
       per press. The first move off the wall is also what tells the panel which
       side of it the axis works on. */
    nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);        /* '*' at X0 */
    for (step = 0; step < 16; step++) {
        nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);
        host_pump_idle(512u);
    }
    host_pump_idle(512u);
    nc_state_runtime(&rt);
    printf("feedtest: after sixteen X+ steps X is %.3f\n", (double)rt.x);
    if (rt.x <= 0.0f) {
        puts("feedtest: FAIL the step jogs did not move the virtual axis");
        failures++;
    }

    /* `3` and `1` change the value the mode is using, and the block follows it:
       one step of the next size up, then back. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_3);          /* step 0.100 -> 0.250 */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);
    for (n = 0u; n + 1u < sizeof(reader) && grbl_stream_available(); ) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '\n';
    }
    reader[n] = '\0';
    printf("feedtest: bigger step queued \"%s\"\n", reader);
    /* X is programmed as a diameter (G7 is the parser's default), so a step of
       0.250 axis millimetres is written as 0.500: without that the axis would
       move half of what the readout says. */
    if (strcmp(reader, "G91 G1 X0.500 F500\nG90\n") != 0) {
        puts("feedtest: FAIL '3' did not change the step the jog uses");
        failures++;
    }
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);          /* step back to 0.100 */
    host_pump_idle(512u);
    nc_state_runtime(&rt);

    /* Feed back toward the wall, holding the key: one block, as far as the wall,
       and nothing else while the key stays down. Reading the reader is what
       takes the block away from the parser, so this pass only checks the block;
       the next one lets the controller run it. */
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);           /* feed mode again */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);          /* feed 500 -> 300 */
    snprintf(expected, sizeof(expected), "$J=G91 X-%.3f F300\n",
             (double)rt.x * 2.0);                         /* axis room, as a diameter */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_hold_key('2');
    for (n = 0u; n + 1u < sizeof(reader) && grbl_stream_available(); ) {
        char c = grbl_stream_getc();

        reader[n++] = c ? c : '\n';
    }
    reader[n] = '\0';
    printf("feedtest: feed queued \"%s\" (wanted \"%s\")\n", reader, expected);
    if (strcmp(reader, expected) != 0) {
        puts("feedtest: FAIL the held feed is not one jog block covering the stop");
        failures++;
    }
    if (grbl_stream_available()) {
        puts("feedtest: FAIL the held feed kept queueing blocks while it was held");
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);

    /* For real now: the controller takes the block and the axis lands on the
       wall. This is also what proves the distance was written in the units the
       parser reads - half of it and the axis would stop in the middle. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_hold_key('2');
    host_pump_idle(4000u);
    nc_state_runtime(&rt);
    printf("feedtest: fed to the stop, X is %.3f\n", (double)rt.x);
    if (rt.x < -0.02f || rt.x > 0.02f) {
        printf("feedtest: FAIL the feed did not end on the stop (X %.3f)\n", (double)rt.x);
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);

    /* On the wall: the way that would cross it is refused, so an irrelevant
       press queues nothing at all. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_hold_key('2');
    host_pump(20u);
    if (grbl_stream_available() || cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL a feed crossed the stop");
        failures++;
    }
    nc_visual_hold_key(0);

    /* The other way is a feed away from the wall - bounded, because the wall is
       one-sided and nothing else ends it - and the key release stops it. */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_8);
    nc_visual_hold_key('8');
    host_pump(20u);
    if (!cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL the axis could not feed away from the stop");
        failures++;
    }
    host_pump(20u);
    nc_state_runtime(&rt);
    printf("feedtest: backing off, X is %.3f (jog=%u)\n", (double)rt.x,
           (unsigned)cnc_get_exec_state(EXEC_JOG));
    if (rt.x <= 0.0005f) {
        puts("feedtest: FAIL the feed away from the stop did not move the axis");
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);
    nc_state_runtime(&rt);
    printf("feedtest: after release X is %.3f (jog=%u)\n", (double)rt.x,
           (unsigned)cnc_get_exec_state(EXEC_JOG));
    if (cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL the feed kept jogging after the key came up");
        failures++;
    }
    if ((double)rt.x > limit) {
        printf("feedtest: FAIL the feed away ran past its limit (X %.3f)\n", (double)rt.x);
        failures++;
    }

    /* With no stop at all a feed is still a move, bounded the same way: the
       operator is never left with keys that do nothing. */
    before_feed = (double)rt.x;
    nc_visual_handle_key(NC_VISUAL_KEY_BACKSPACE);        /* '*' clears it */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_hold_key('2');
    host_pump(20u);
    if (!cnc_get_exec_state(EXEC_JOG)) {
        puts("feedtest: FAIL a feed with no stop set did nothing");
        failures++;
    }
    host_pump(20u);
    nc_state_runtime(&rt);
    printf("feedtest: no stop set, feeding X- from %.3f to %.3f\n",
           before_feed, (double)rt.x);
    if ((double)rt.x >= before_feed - 0.0005) {
        puts("feedtest: FAIL the feed with no stop set did not move the axis");
        failures++;
    }
    nc_visual_hold_key(0);
    host_pump_idle(64u);
    if (failures) {
        printf("feedtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("feedtest: PASS held key feeds to the stop, release cancels it");
    return 0;
}

/* The keypad's event byte: bit 7 is the release flag and the low seven bits are
   the key. Every key the machine has must decode on both edges - a release that
   decodes as "no key" leaves the driver believing the key is still down, and
   then the panel takes the next press of that key for a repeat and drops it
   ("A works once until B is pressed"). */
static int host_keytest(void)
{
    static const struct {
        uint8_t event;      /* as the TCA8418 numbers them, 1*row+col */
        cam_key_t key;
        const char *name;
    } map[] = {
        {  1, CAM_KEY_STAR, "*" }, {  2, CAM_KEY_0, "0" },
        {  3, CAM_KEY_HASH, "#" }, {  4, CAM_KEY_D, "D" },
        { 11, CAM_KEY_1, "1" },    { 12, CAM_KEY_2, "2" },
        { 13, CAM_KEY_3, "3" },    { 14, CAM_KEY_C, "C" },
        { 21, CAM_KEY_4, "4" },    { 22, CAM_KEY_5, "5" },
        { 23, CAM_KEY_6, "6" },    { 24, CAM_KEY_B, "B" },
        { 31, CAM_KEY_7, "7" },    { 32, CAM_KEY_8, "8" },
        { 33, CAM_KEY_9, "9" },    { 34, CAM_KEY_A, "A" }
    };
    uint8_t raw[6] = {0};
    unsigned i;
    int failures = 0;

    for (i = 0u; i < sizeof(map) / sizeof(map[0]); i++) {
        raw[0] = map[i].event;
        if (cam_keyboard_decode_key(raw) != map[i].key) {
            printf("keytest: FAIL event %u is not '%s' pressed\n",
                   map[i].event, map[i].name);
            failures++;
        }
        raw[0] = (uint8_t)(map[i].event | 0x80);
        if (cam_keyboard_decode_key(raw) != map[i].key) {
            printf("keytest: FAIL event %u is not '%s' released\n",
                   map[i].event, map[i].name);
            failures++;
        }
        if (cam_keyboard_key_to_char(map[i].key) != map[i].name[0]) {
            printf("keytest: FAIL '%s' is not the character it sends\n", map[i].name);
            failures++;
        }
    }
    raw[0] = 0x00;
    if (cam_keyboard_decode_key(raw) != CAM_KEY_NONE) {
        puts("keytest: FAIL a zero event is a key");
        failures++;
    }
    if (failures) {
        printf("keytest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("keytest: PASS every keypad key decodes on both edges");
    return 0;
}

/* The real thing, loaded through the same code the machine uses: one NC program
   and the tool table that belongs to it (`uCNC/src/modules/nc/tests/fixtures`,
   copied into the `--files` root by the test runner).

   The program is the operator's facing job: a G971/G973 setup, a T2 tool line
   and a G80-terminated G71 contour with the corner round/chamfer words. The
   checks are the ones that would catch a regression the eye might miss - the
   block scan finding the block a line inside it, the emitter expanding it, and
   the T word resolving through the tool table to the tool that cuts it. */
static int host_filetest(void)
{
    static const char *const program = "/D/nc/files/facing.nc";
    static const char *const tools = "/D/nc/files/tool.t";
    nc_document_t doc;
    nc_document_t tool_doc;
    nc_emit_stream_t stream;
    nc_tool_t tool;
    char line[NC_MAX_LINE_LEN + 2];
    size_t source_line;
    size_t end_line = 0u;
    unsigned emitted = 0u;
    unsigned tool_lines = 0u;
    bool saw_deep_z = false;
    int failures = 0;
    nc_result_t r;
    size_t i;

    host_init_core();
    nc_document_init(&doc);
    nc_document_init(&tool_doc);

    r = nc_load_file(&doc, program);
    if (r != NC_OK) {
        printf("filetest: FAIL cannot load %s (%s)\n", program, nc_result_text(r));
        failures++;
    } else if (doc.line_count != 11u ||
               strcmp(doc.lines[0].text, "G970 X-5 U60 Z-60 W5") != 0 ||
               strcmp(doc.lines[4].text, "G71 U1 R0.2 X0.5 Z0.5 F450") != 0 ||
               strcmp(doc.lines[10].text, "G80") != 0) {
        printf("filetest: FAIL the program did not load as written (%u lines)\n",
               (unsigned)doc.line_count);
        failures++;
    } else {
        printf("filetest: program %s: %u lines\n", program, (unsigned)doc.line_count);
    }

    r = nc_load_file(&tool_doc, tools);
    if (r != NC_OK) {
        printf("filetest: FAIL cannot load %s (%s)\n", tools, nc_result_text(r));
        failures++;
    } else {
        for (i = 0u; i < tool_doc.line_count; i++) {
            if (nc_tool_line_is_tool(tool_doc.lines[i].text)) {
                tool_lines++;
            }
        }
        if (tool_lines != 7u) {
            printf("filetest: FAIL the tool table has %u tools, not 7\n", tool_lines);
            failures++;
        } else if (!nc_tool_by_number(&tool_doc, 2, &tool) ||
                   tool.r != 3.0f || tool.orient != 176) {
            puts("filetest: FAIL T2 is not the R3 O176 tool");
            failures++;
        } else {
            printf("filetest: tools %s: %u tools\n", tools, tool_lines);
        }
    }

    /* The block the operator sees as one: the G71 line and its G80 range. */
    if (doc.line_count == 11u) {
        if (nc_g7x_block_start(&doc, 4u) != 4u ||
            !nc_g7x_block_end(&doc, 4u, &end_line) || end_line != 10u ||
            !nc_g7x_line_is_contour(&doc, 4u, 5u) ||
            nc_g7x_line_is_contour(&doc, 4u, 10u)) {
            puts("filetest: FAIL the G71 block does not start at line 5 and end at G80");
            failures++;
        } else {
            printf("filetest: G71 block is lines 5..%u\n", (unsigned)(end_line + 1u));
        }

        /* Expanded through the emitter RUN and the preview share. */
        nc_emit_stream_begin(&stream, &doc, 4u);
        while (emitted < 400u) {
            nc_emit_result_t er = nc_emit_stream_next(&stream, line, sizeof(line),
                                                      &source_line);

            if (er == NC_EMIT_SKIP) {
                break;
            }
            if (er == NC_EMIT_ERROR) {
                printf("filetest: FAIL the emitter stopped at line %u\n",
                       (unsigned)(source_line + 1u));
                failures++;
                break;
            }
            if (strstr(line, "Z-25")) {
                saw_deep_z = true;
            }
            emitted++;
        }
        if (emitted < 10u) {
            printf("filetest: FAIL the G71 block expanded to only %u lines\n", emitted);
            failures++;
        } else if (!saw_deep_z) {
            puts("filetest: FAIL the expansion never reaches the Z-25 finish");
            failures++;
        } else {
            printf("filetest: G71 expanded to %u lines\n", emitted);
        }

        /* And the tool line above it resolves through the table to T2. */
        if (nc_tool_active_from_table(&doc, 4u, &tool_doc, &tool) &&
            tool.t == 2 && tool.r == 3.0f && tool.orient == 176) {
            printf("filetest: the G71 block cuts with T%d R%.1f O%d\n", tool.t,
                   (double)tool.r, tool.orient);
        } else {
            puts("filetest: FAIL the T2 line above the G71 block did not resolve");
            failures++;
        }
    }

    /* Text files are text: the list carries them (the frame in
       tmp\nc-ui-show\ROOT-list shows `presets.txt` at /D) and the editor can
       open and save them, but only the program extensions are read as G-code -
       no preview parse, no RUN. */
    if (!nc_path_text("presets.txt") || nc_path_supported("presets.txt") ||
        !nc_path_text("facing.nc") || !nc_path_supported("facing.nc")) {
        puts("filetest: FAIL the text/program split is wrong for .txt or .nc");
        failures++;
    } else {
        puts("filetest: a .txt is text (list, edit, save) and not a program");
    }

    if (failures) {
        printf("filetest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("filetest: PASS program, tool table, block scan, expansion and T link");
    return 0;
}

/* Print what the machine thinks it is doing after the keys and ticks have run:
   the states a scripted feed depends on, and the figures the panel reads. */
static int host_state(void)
{
    nc_runtime_state_t rt;
    unsigned i;

    host_init_core();
    if (g_key_script[0]) {
        host_play_keys(g_key_script);
    }
    for (i = 0u; i < g_ticks; i++) {
        host_run_machine(TIMER_MS);
    }
    nc_state_runtime(&rt);
    printf("state: exec=0x%04x run=%u jog=%u hold=%u alarm=%u canceling=%u\n",
           cnc_get_exec_state(EXEC_ALLACTIVE),
           (unsigned)!!cnc_get_exec_state(EXEC_RUN),
           (unsigned)!!cnc_get_exec_state(EXEC_JOG),
           (unsigned)!!cnc_get_exec_state(EXEC_HOLD),
           (unsigned)cnc_has_alarm(),
           (unsigned)!!cnc_get_exec_state(EXEC_CANCELING));
    printf("state: X=%.3f Z=%.3f feed=%.1f planner_empty=%u itp_empty=%u reader=%u\n",
           (double)rt.x, (double)rt.z, (double)rt.feed,
           (unsigned)planner_buffer_is_empty(),
           (unsigned)itp_is_empty(),
           (unsigned)grbl_stream_available());
    return 0;
}

/* Write a 32bpp top-down buffer as a .bmp, the shape the GDI DIB section and
   the host framebuffer both use (BGRA, rows top to bottom). */
static bool host_write_bmp32(const char *path, const void *pixels, int width, int height)
{
    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;
    DWORD image_size = (DWORD)(width * height * 4);
    FILE *fp;

    memset(&file_header, 0, sizeof(file_header));
    memset(&info_header, 0, sizeof(info_header));
    file_header.bfType = 0x4D42;                    /* 'BM' */
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + image_size;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = width;
    info_header.biHeight = -height;                 /* top-down */
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = image_size;

    fp = fopen(path, "wb");
    if (!fp) {
        return false;
    }
    if (fwrite(&file_header, sizeof(file_header), 1, fp) != 1u ||
        fwrite(&info_header, sizeof(info_header), 1, fp) != 1u ||
        fwrite(pixels, 1u, image_size, fp) != image_size) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

/* The whole bench - panel plus the machine keys - so the key row is reviewable
   without opening the window. The panel dump above stays the firmware layout. */
static int host_dump_bench(const char *path)
{
    BITMAPINFO info;
    void *bits = 0;
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    bool ok;

    host_init_core();
    if (g_key_script[0]) {
        host_play_keys(g_key_script);
    }
    for (unsigned i = 0u; i < g_ticks; i++) {
        host_run_machine(TIMER_MS);
    }
    nc_visual_draw();

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = WIN_W;
    info.bmiHeader.biHeight = -WIN_H;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        fprintf(stderr, "nc_ui: cannot create the bench bitmap\n");
        return 1;
    }
    old = SelectObject(dc, bitmap);
    host_draw_bench(dc);
    GdiFlush();
    ok = host_write_bmp32(path, bits, WIN_W, WIN_H);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (!ok) {
        fprintf(stderr, "nc_ui: cannot write %s\n", path);
        return 1;
    }
    printf("nc_ui: wrote %dx%d bench to %s\n", WIN_W, WIN_H, path);
    return 0;
}

/* Headless check of the NC preset-file contract, driven through the same
   fs_* API the firmware uses:

     1. no /D/presets.txt          -> the compiled presets are written out
     2. an edited file             -> the file wins over the compiled default
     3. an unparsable file         -> compiled default stays in use and the
                                      user's text is left alone to be fixed
*/
static bool host_fs_write_text(const char *path, const char *text)
{
    size_t len = strlen(text);
    fs_file_t *fp = fs_open(path, "w");

    if (!fp) {
        return false;
    }
    if (fs_write(fp, (const uint8_t *)text, len) != len) {
        fs_close(fp);
        return false;
    }
    fs_close(fp);
    return true;
}

static bool host_preset_line_is(int id, const char *expected)
{
    nc_document_t doc;
    bool ok;

    nc_document_init(&doc);
    ok = nc_insert_preset_id(&doc, id) &&
         doc.line_count == 1u &&
         strcmp(doc.lines[0].text, expected) == 0;
    return ok;
}

static int host_presettest(void)
{
    static const char *const default_od = "G71 U0 R0 X0 Z0 F0 P0 Q0 N0";
    static const char *const edited_od = "G71 U2 R1 X10 Z-5 F0.2 P100 Q200";
    static const char *const edited_file =
        "[41]\nname=OD TEST\nline=G71 U2 R1 X10 Z-5 F0.2 P100 Q200\n";
    /* The file owns the insert text, so a section may carry several lines - the
       header and its contour - and they go in in order. */
    static const char *const multi_file =
        "[41]\nname=OD MULTI\n"
        "line=G71 U1 R0.2 X0.5 Z0.5 F450\n"
        "line=G1 X30 Z0\n"
        "line=G80\n";
    /* A section without a name is not usable: the name is what names the entry,
       even while the helper still shows its own labels. */
    static const char *const unnamed_file =
        "[41]\nline=G71 U9 R9 X9 Z9 F9 P9 Q9\n";
    static const char *const broken_file = "this is not a preset file\n";
    char buf[1024];
    fs_file_info_t info;
    fs_file_t *fp;
    int failures = 0;
    size_t read;

    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    printf("nc_ui: fs root %s\n", g_files_root[0] ? g_files_root : "nc-files");

    /* 1. missing file is materialised from the compiled presets */
    (void)fs_remove("/D/presets.txt");
    (void)nc_presets_init();
    if (!fs_finfo("/D/presets.txt", &info)) {
        puts("presettest: FAIL missing preset file was not created");
        failures++;
    } else if (info.size == 0u) {
        puts("presettest: FAIL created preset file is empty");
        failures++;
    } else if (!host_preset_line_is(41, default_od)) {
        puts("presettest: FAIL compiled OD preset did not insert");
        failures++;
    } else {
        puts("presettest: PASS missing file is written from the compiled presets");
    }

    /* 2. an edited file is what the menu inserts */
    if (!host_fs_write_text("/D/presets.txt", edited_file)) {
        puts("presettest: FAIL could not write the edited preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, edited_od)) {
            puts("presettest: FAIL edited preset file was not used");
            failures++;
        } else {
            puts("presettest: PASS edited preset file is used");
        }
    }

    /* 3. a file with no usable section falls back without touching the text */
    if (!host_fs_write_text("/D/presets.txt", broken_file)) {
        puts("presettest: FAIL could not write the unparsable preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, default_od)) {
            puts("presettest: FAIL unparsable preset file did not fall back");
            failures++;
        } else {
            read = 0u;
            buf[0] = '\0';
            fp = fs_open("/D/presets.txt", "r");
            if (fp) {
                read = fs_read(fp, (uint8_t *)buf, sizeof(buf) - 1u);
                fs_close(fp);
            }
            buf[read] = '\0';
            if (strcmp(buf, broken_file) != 0) {
                puts("presettest: FAIL the unparsable file was rewritten");
                failures++;
            } else {
                puts("presettest: PASS unparsable file falls back and is left alone");
            }
        }
    }

    /* 4. a section with several lines inserts all of them, in order, through
          the same call the panel's OD entry makes. */
    if (!host_fs_write_text("/D/presets.txt", multi_file)) {
        puts("presettest: FAIL could not write the multi-line preset file");
        failures++;
    } else {
        nc_document_t doc;
        bool ok;

        (void)nc_presets_init();
        nc_document_init(&doc);
        ok = nc_insert_preset(&doc, NC_PRESET_OD) == NC_OK &&
             doc.line_count == 3u &&
             strcmp(doc.lines[0].text, "G71 U1 R0.2 X0.5 Z0.5 F450") == 0 &&
             strcmp(doc.lines[1].text, "G1 X30 Z0") == 0 &&
             strcmp(doc.lines[2].text, "G80") == 0;
        if (!ok) {
            printf("presettest: FAIL the OD section inserted %u lines\n",
                   (unsigned)doc.line_count);
            failures++;
        } else {
            puts("presettest: PASS every line= of a section is inserted, in order");
        }
    }

    /* 5. a section without `name=` is skipped, so the compiled OD stays. */
    if (!host_fs_write_text("/D/presets.txt", unnamed_file)) {
        puts("presettest: FAIL could not write the nameless preset file");
        failures++;
    } else {
        (void)nc_presets_init();
        if (!host_preset_line_is(41, default_od)) {
            puts("presettest: FAIL a section without a name was used");
            failures++;
        } else {
            puts("presettest: PASS a section without a name is skipped");
        }
    }

    printf("presettest: %s (%d failure%s)\n",
           failures ? "FAILED" : "OK",
           failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

/* Headless check of the panel's one-shot blocks on the reader the RUN stream
   also shares: the two blocks a jog sends are both delivered, in order, and the
   reader goes back to the console when they are done. A jog used to lose the
   first block because the second one was written over it before the parser had
   read it. */
static int host_streamtest(void)
{
    char got[128];
    size_t n = 0u;
    unsigned guard;

    host_init_core();
    if (!nc_run_send_line("G91 G1 X0.100 F500") ||
        !nc_run_send_line("G90")) {
        puts("streamtest: FAIL the panel blocks were not queued");
        return 1;
    }
    for (guard = 0u; guard < 256u && grbl_stream_available(); guard++) {
        char c = grbl_stream_getc();

        if (n + 2u < sizeof(got)) {
            got[n++] = c ? c : '|';
        }
    }
    got[n] = '\0';
    if (strcmp(got, "G91 G1 X0.100 F500|G90|") != 0) {
        printf("streamtest: FAIL the reader saw \"%s\"\n", got);
        return 1;
    }
    if (grbl_stream_available()) {
        puts("streamtest: FAIL the reader did not go back to the console");
        return 1;
    }
    puts("streamtest: PASS both jog blocks delivered, reader handed back");
    return 0;
}

/* Headless check of the key model the shell presents. Three things can go
   wrong and all of them are silent on screen:

     1. a key character does not reach the key the machine sends
        (nc_visual_key_for_char() is that table, and nc_module.c maps the
        hardware keypad through it);
     2. the pad is not the machine's keypad - the shell would press a key the
        machine does not have, or draw one where the hardware has another;
     3. a screen offers a footer key the pad cannot press, so the bench - and
        the machine - cannot reach that entry at all.

   The third one is what the old pad did wrong: it mapped its cells to footer
   positions, so MANUAL's B/C/D and the skipped slots sent the wrong key. */
/* The footer is a strip of fixed slots: more than NC_FOOTER_SLOTS and the keys
   shrink and move between screens, so a screen that needs fewer entries leaves
   them empty instead. `D` (accept) and `0` (file) keep their slot because the
   screen names what they do there; `*` is the delete/back key. */
#define HOST_FOOTER_SLOTS 8

static bool host_pad_has_key(char key)
{
    int row;
    int col;

    for (row = 0; row < PAD_ROWS; row++) {
        for (col = 0; col < PAD_COLS; col++) {
            if (g_pad_keys[row][col] == key)
                return true;
        }
    }
    return false;
}

static int host_padtest(void)
{
    /* The matrix cam_keyboard.c decodes: key event 1*row+col, as the driver
       numbers them. Row 1 on top. */
    static const char *const machine[PAD_ROWS] = {
        "*0#D",
        "123C",
        "456B",
        "789A"
    };
    static const nc_mode_t modes[] = {
        NC_MODE_MANUAL, NC_MODE_PROGRAM, NC_MODE_TOOLS, NC_MODE_RUN
    };
    static const struct {
        char key;
        nc_visual_key_t want;
    } map[] = {
        { '0', NC_VISUAL_KEY_DIGIT_0 },
        { '1', NC_VISUAL_KEY_DIGIT_1 },
        { '2', NC_VISUAL_KEY_DIGIT_2 },
        { '3', NC_VISUAL_KEY_DIGIT_3 },
        { '4', NC_VISUAL_KEY_DIGIT_4 },
        { '5', NC_VISUAL_KEY_DIGIT_5 },
        { '6', NC_VISUAL_KEY_DIGIT_6 },
        { '7', NC_VISUAL_KEY_DIGIT_7 },
        { '8', NC_VISUAL_KEY_DIGIT_8 },
        { '9', NC_VISUAL_KEY_DIGIT_9 },
        { '*', NC_VISUAL_KEY_BACKSPACE },
        { '#', NC_VISUAL_KEY_FINISH },
        { 'A', NC_VISUAL_KEY_MODE },
        { 'B', NC_VISUAL_KEY_FIELD_PREV },
        { 'C', NC_VISUAL_KEY_FIELD_NEXT },
        { 'D', NC_VISUAL_KEY_ACCEPT }
    };
    unsigned m;
    unsigned k;
    int row;
    int failures = 0;

    host_init_core();
    for (k = 0u; k < sizeof(map) / sizeof(map[0]); k++) {
        if (nc_visual_key_for_char(map[k].key) != map[k].want) {
            printf("padtest: FAIL keypad '%c' is not the key the machine sends\n",
                   map[k].key);
            failures++;
        }
    }
    if (nc_visual_key_for_char('a') != NC_VISUAL_KEY_NONE ||
        nc_visual_key_for_char('X') != NC_VISUAL_KEY_NONE ||
        nc_visual_key_for_char('\0') != NC_VISUAL_KEY_NONE) {
        puts("padtest: FAIL a key outside the keypad was accepted");
        failures++;
    }
    for (row = 0; row < PAD_ROWS; row++) {
        if (strcmp(g_pad_keys[row], machine[row]) != 0) {
            printf("padtest: FAIL row %d is \"%s\", the keypad has \"%s\"\n",
                   row, g_pad_keys[row], machine[row]);
            failures++;
        }
    }
    for (m = 0u; m < sizeof(modes) / sizeof(modes[0]); m++) {
        size_t count = 0u;
        const nc_footer_item_t *footer;
        size_t i;

        nc_visual_select_mode(modes[m]);
        footer = nc_visual_footer(&count);
        for (i = 0u; i < count; i++) {
            if (footer[i].key == ' ' || footer[i].key == '\0')
                continue; /* empty slot: it keeps its place, it is not a key */
            if (!host_pad_has_key(footer[i].key)) {
                printf("padtest: FAIL %s offers key '%c' (%s) the keypad has not\n",
                       nc_menu_mode_name(modes[m]), footer[i].key, footer[i].label);
                failures++;
            }
            if (!host_key_label(footer[i].key)) {
                printf("padtest: FAIL %s key '%c' (%s) is drawn with no label\n",
                       nc_menu_mode_name(modes[m]), footer[i].key, footer[i].label);
                failures++;
            }
        }
        /* Both strips the screen can show - its own and the file list's - have
           to fit the fixed slots. */
        {
            unsigned view;

            for (view = 0u; view < 2u; view++) {
                size_t fcount = 0u;
                const nc_footer_item_t *items =
                    nc_menu_footer(modes[m], view != 0u, &fcount);

                if (fcount > HOST_FOOTER_SLOTS) {
                    printf("padtest: FAIL %s footer has %u slots (max %u)\n",
                           nc_menu_mode_name(modes[m]), (unsigned)fcount,
                           (unsigned)HOST_FOOTER_SLOTS);
                    failures++;
                }
                /* Only a screen that edits a document may delete with a key:
                   RUN carries no delete, because `*` used to delete a line of
                   the program being run. */
                if (modes[m] != NC_MODE_PROGRAM && modes[m] != NC_MODE_TOOLS &&
                    view == 0u) {
                    size_t k;

                    for (k = 0u; k < fcount; k++) {
                        if (items[k].action == NC_FOOTER_ACTION_DELETE) {
                            printf("padtest: FAIL %s footer deletes with '%c' (%s)\n",
                                   nc_menu_mode_name(modes[m]), items[k].key,
                                   items[k].label);
                            failures++;
                        }
                    }
                }
            }
        }
        /* EDIT's full-screen strip is a view too: it must fit the slots and it
           must not offer a delete while the preview is the whole screen. */
        {
            size_t pcount = 0u;
            const nc_footer_item_t *preview_items = nc_menu_preview_footer(&pcount);
            size_t k;

            if (pcount > HOST_FOOTER_SLOTS) {
                printf("padtest: FAIL full-screen footer has %u slots (max %u)\n",
                       (unsigned)pcount, (unsigned)HOST_FOOTER_SLOTS);
                failures++;
            }
            for (k = 0u; k < pcount; k++) {
                if (preview_items[k].action == NC_FOOTER_ACTION_DELETE) {
                    printf("padtest: FAIL full screen deletes with '%c' (%s)\n",
                           preview_items[k].key, preview_items[k].label);
                    failures++;
                }
            }
        }
        /* MANUAL's jog digits are not footer entries: the screen's own hint is
           the only thing that can label them, and the panel draws them too. */
        if (modes[m] == NC_MODE_MANUAL) {
            int digit;

            for (digit = 1; digit <= 9; digit++) {
                if (!nc_visual_key_hint((char)('0' + digit))) {
                    printf("padtest: FAIL MANUAL digit %d has no meaning to show\n",
                           digit);
                    failures++;
                }
            }
        }
    }
    if (failures) {
        printf("padtest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("padtest: PASS the keypad is the machine's matrix, its keys are the keys "
         "the machine sends, and every footer key is on it");
    return 0;
}

/* Headless check of the editor's new-file field: the keys have to reach the
   name, the name has to reach the card, and the file that is created has to be
   the one the editor then holds.

   This path is why the check exists: the extraction of the editor passed the
   key character to the field handler and the handler kept a local copy of it,
   so every digit was dropped and the file was created with no name at all -
   something no frame dump shows, because the field is only drawn in the file
   list. */
static int host_newfiletest(void)
{
    static const char *const created = "/D/nc/files/12.nc";
    nc_document_t doc;
    int failures = 0;
    nc_result_t r;

    host_init_core();
    nc_visual_select_mode(NC_MODE_PROGRAM);
    host_pump_idle(64u);

    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);             /* drop the selection:
                                                               `0` opens the list
                                                               only without one */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_0);            /* open the file list */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_5);            /* 5 NEW */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_1);            /* type the name */
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_2);
    nc_visual_handle_key(NC_VISUAL_KEY_FINISH);             /* '#' creates it */
    host_pump_idle(64u);

    nc_document_init(&doc);
    r = nc_load_file(&doc, created);
    if (r != NC_OK) {
        printf("newfiletest: FAIL the typed name did not reach the card (%s)\n",
               nc_result_text(r));
        failures++;
    } else {
        printf("newfiletest: created %s with %u lines\n", created,
               (unsigned)doc.line_count);
    }

    /* The next digits must land in the next field: a field that keeps its own
       copy of the key is exactly what this checks against. */
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_0);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_5);
    nc_visual_handle_key(NC_VISUAL_KEY_DIGIT_3);
    nc_visual_handle_key(NC_VISUAL_KEY_CANCEL);             /* 'A' cancels */
    host_pump_idle(64u);

    {
        nc_document_t other;

        nc_document_init(&other);
        if (nc_load_file(&other, "/D/nc/files/3.nc") == NC_OK) {
            puts("newfiletest: FAIL a cancelled new-file field still created a file");
            failures++;
        }
    }

    if (failures) {
        printf("newfiletest: FAILED (%d)\n", failures);
        return 1;
    }
    puts("newfiletest: PASS the new-file field takes the typed name and creates it");
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
    fflush(stdout);
    printf("nc_ui: %u entries\n", count);
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--files") == 0) {
            snprintf(g_files_root, sizeof(g_files_root), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--keys") == 0) {
            snprintf(g_key_script, sizeof(g_key_script), "%s", argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--ticks") == 0) {
            /* Run the machine for N ticks before drawing, so a dump can show a
               program or a feed that has been running. */
            g_ticks = (unsigned)strtoul(argv[i + 1], NULL, 0);
            i++;
        }
    }
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0)
            return host_dump(argv[i + 1]);
        if (strcmp(argv[i], "--dump-bench") == 0)
            return host_dump_bench(argv[i + 1]);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fstest") == 0)
            return host_fstest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--presettest") == 0)
            return host_presettest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--streamtest") == 0)
            return host_streamtest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--padtest") == 0)
            return host_padtest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--feedtest") == 0)
            return host_feedtest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state") == 0)
            return host_state();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--keytest") == 0)
            return host_keytest();
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--filetest") == 0)
            return host_filetest();
        if (strcmp(argv[i], "--newfiletest") == 0)
            return host_newfiletest();
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
