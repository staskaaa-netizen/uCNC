/* The station's shell: the emulated panel and the machine's keypad on screen,
   the keys the PC keyboard adds, and the emulated machine both are driven
   through (host_shell.h). What the station must *do* is checked without a
   window in host_tests.c - this file only has to know the checks exist and
   hand them the command line.

   See tools/nc_ui_win/README.md for the layout, the keys and the checks, and
   the header comment below for what the flags are.
   */

/* nc_ui - the uCNC programming station on Windows: it runs the NC screen
   exactly as the LVDS panel shows it, plus the machine's own keypad and the
   active screen's own usage notes beside it.

   The window has two parts:
     - the emulated panel (800x600) rendered by modules/nc/nc_visual.c through
       the host LVDS backend, unmodified layout;
     - the keys the proprietary keyboard has in hardware: F1-F4 jump between
       the operation modes and the 4x4 keypad (the matrix cam_keyboard.c
       decodes: `*0#D` / `123C` / `456B` / `789A`) sits beside the panel. The
       pad asks the screen what each key means (footer label, or MANUAL's own
       jog hint), draws the arrows on the keys that step a field, shows the
       screen's own usage lines (`nc_visual_usage()`), reads the spindle off the
       signals the tool drives (`host_spindle.c`) and sends the key code
       nc_module.c would send, so what is pressed here is what is pressed on the
       machine.

   nc_ui --dump out.bmp   render one frame headlessly (layout smoke test)
   nc_ui --files DIR      mount DIR as the /D drive (default: the `nc-files`
                          folder beside the exe, so a shortcut works)
   nc_ui [--keys LIST] --dump out.bmp
                           press LIST (comma separated: the keypad's own keys
                           `0`-`9`, `*`, `#`, `A`-`D`, the PC keys with a fixed
                           meaning (`W`, Delete, Enter, Esc, Backspace), the
                           modes F1-F4, or named keys ACCEPT/NEXT/PREV/FINISH/
                           BACK/CANCEL/MODE/UP/DOWN/LEFT/RIGHT/MINUS/DOT)
                           before rendering, so a screen that only appears after
                           input can be checked
   nc_ui --state          print what the machine is doing now
   nc_ui --version        print which build this exe is
   nc_ui --fstest         list /D through the firmware fs_* API
   nc_ui --presettest     check the /D/presets entries contract
   nc_ui --streamtest     check the panel's one-shot blocks reach the reader
   nc_ui --dirtytest      a key that changes the screen asks for a repaint
   nc_ui --runtest        RUN's FROM and FULL send the program
   nc_ui --stoptest       the MANUAL stops are typed, taken and respected
   nc_ui --spindletest    the spindle runs from the machine's own signals
   nc_ui --demotest       the demo card seeds once, and the demo expands
   nc_ui --blocktest      the mark is the line in play and its cycle block, and
                          it stays on the unit that ran
   nc_ui --pacetest       RUN gives one block and waits for the machine
   nc_ui                  open the window
   */

#include <math.h>
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
#include "nc_layout.h"
#include "nc_manual.h"
#include "nc_palette.h"
#include "nc_preview.h"
#include "nc_tools.h"
#include "nc_vocab.h"
#include "nc_visual.h"
#include "g7x.h"
#include "host_fs.h"
#include "host_spindle.h"
#include "host_shell.h"
#include "host_tests.h"
#include "lvds_host.h"
#include "modules/cam_keyboard/cam_keyboard.h"


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
    {  12,  36, 132, 28, "F1 MANUAL", NC_VISUAL_KEY_NONE, NC_MODE_MANUAL },
    { 156,  36, 132, 28, "F2 EDIT",   NC_VISUAL_KEY_NONE, NC_MODE_PROGRAM },
    {  12,  68, 132, 28, "F3 TOOLS",  NC_VISUAL_KEY_NONE, NC_MODE_TOOLS },
    { 156,  68, 132, 28, "F4 RUN",    NC_VISUAL_KEY_NONE, NC_MODE_RUN },
    {  12, 100, 276, 26, "MODE",      NC_VISUAL_KEY_MODE, (nc_mode_t)-1 }

    /* The machine keypad is drawn and clicked by character (see host_pad_*):
       it is the hardware's own key row, not a second soft-key strip. */
};

static const int g_button_count = (int)(sizeof(g_buttons) / sizeof(g_buttons[0]));

/* The machine keypad, as cam_keyboard.c decodes it: a 4x4 matrix with the
   first row on top. The shell draws these keys and sends these key codes, so
   the bench presses what the machine presses - including the letter keys
   (A cancel, B/C step, D accept) that the footer labels name. */

const char g_pad_keys[PAD_ROWS][PAD_COLS + 1] = {
    "*0#D",
    "123C",
    "456B",
    "789A"
};

/* Pad geometry: cell (row, col), row major, top row first. */
#define PAD_X0 12
#define PAD_Y0 282
#define PAD_CELL_W 64
#define PAD_CELL_H 40
#define PAD_GAP_X 6
#define PAD_GAP_Y 4
#define PAD_PITCH_X (PAD_CELL_W + PAD_GAP_X)
#define PAD_PITCH_Y (PAD_CELL_H + PAD_GAP_Y)
#define PAD_CELL_RIGHT(col) (PAD_X0 + (col) * PAD_PITCH_X + PAD_CELL_W)
#define PAD_CELL_BOTTOM(row) (PAD_Y0 + (row) * PAD_PITCH_Y + PAD_CELL_H)

/* The bands beside the pad: the mode keys on top, then what the active screen
   is and how it is driven, then the keypad, then what the PC keyboard adds. */
#define SIDE_X 12
#define SIDE_TITLE_Y 10
#define SIDE_USAGE_NAME_Y 134
#define SIDE_USAGE_Y 152
#define SIDE_USAGE_STEP 15
#define SIDE_PAD_NAME_Y 266
#define SIDE_KEYS_NAME_Y 466
#define SIDE_KEYS_Y 482
#define SIDE_KEYS_STEP 15

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

/* What the key means right now. The screen answers - the footer entry that
   carries it, the screen's own word for a key the footer does not name
   (MANUAL's jog digits), and whether the key steps a field or the axis. The
   meanings live with the screen, never in this shell. */
bool host_key_meaning(char key, nc_visual_key_meaning_t *meaning)
{
    memset(meaning, 0, sizeof(*meaning));
    return nc_visual_key_meaning(key, meaning);
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

/* The machine keypad keys a PC key stands for when the key has the same meaning
   on every layout - the fixed half of the map below, and what the checks pin.
   `W` is the keypad's finish key on a PC keyboard: `#` needs Shift+3 on most
   layouts and AltGr on the rest, so the machine's `#` gets a letter of its own
   here. `#` and Delete still send it where the layout can produce them. */
char host_pc_machine_key(unsigned vk)
{
    switch (vk) {
    case VK_RETURN: return 'D';
    case VK_ESCAPE: return 'A';
    case VK_BACK:   return '*';
    case VK_DELETE: return '#';
    case 'W':       return '#';
    default: break;
    }
    return 0;
}

/* The machine keypad key a PC key stands for, 0 for the keys only the host has
   (arrows, sign, point). The rest is asked of the keyboard layout, so `*`, `#`
   and the letters A-D work on any layout. */
static char host_machine_key_for_vk(WPARAM vk, LPARAM lp)
{
    BYTE state[256];
    WORD chars[2];
    char fixed = host_pc_machine_key((unsigned)vk);

    if (fixed) {
        return fixed;
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
    TextOutA(dc, PANEL_W + SIDE_X, SIDE_TITLE_Y, "uCNC programming station", 24);

    /* What the active screen is and how it is driven, in the screen's own
       words (`nc_visual_usage()`), so the panel beside the machine and the
       panel under it cannot say different things about the same key. */
    {
        const char *const *usage = 0;
        const char *name = nc_visual_screen_name();
        size_t count = nc_visual_usage(&usage);
        size_t line;

        SelectObject(dc, small);
        SetTextColor(dc, RGB(160, 164, 168));
        TextOutA(dc, PANEL_W + SIDE_X, SIDE_USAGE_NAME_Y, "screen", 6);
        SelectObject(dc, font);
        SetTextColor(dc, RGB(120, 190, 150));
        TextOutA(dc, PANEL_W + SIDE_X + 64, SIDE_USAGE_NAME_Y,
                 name, (int)strlen(name));
        SelectObject(dc, small);
        SetTextColor(dc, RGB(198, 202, 206));
        for (line = 0u; line < count && line < NC_VISUAL_USAGE_MAX; line++) {
            TextOutA(dc, PANEL_W + SIDE_X,
                     SIDE_USAGE_Y + (int)line * SIDE_USAGE_STEP,
                     usage[line], (int)strlen(usage[line]));
        }
        SelectObject(dc, font);
    }

    SetTextColor(dc, RGB(160, 164, 168));
    SelectObject(dc, small);
    TextOutA(dc, PANEL_W + SIDE_X, SIDE_PAD_NAME_Y, "machine keypad", 14);
    /* The spindle the machine is being told, read off the signals the tool
       drives (PWM0/DOUT0) - the station has no spindle encoder. */
    {
        char spindle[32];

        if (host_spindle_text(spindle, sizeof(spindle))) {
            int width = 0;
            SIZE extent;

            if (GetTextExtentPoint32A(dc, spindle, (int)strlen(spindle), &extent)) {
                width = extent.cx;
            }
            SetTextColor(dc, RGB(120, 190, 150));
            TextOutA(dc, PANEL_W + SIDE_W - SIDE_X - width, SIDE_PAD_NAME_Y,
                     spindle, (int)strlen(spindle));
            SetTextColor(dc, RGB(160, 164, 168));
        }
    }
    SelectObject(dc, font);

    /* The keypad is the hardware, not a menu: the same 4x4 matrix the machine
       keyboard decodes, with the meaning the active screen gives each key. */
    {
        int row;
        int col;

        for (row = 0; row < PAD_ROWS; row++) {
            for (col = 0; col < PAD_COLS; col++) {
                char key = g_pad_keys[row][col];
                nc_visual_key_meaning_t meaning;
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
                key_rect.bottom = key_rect.top + 20;
                DrawTextA(dc, text, 1, &key_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (host_key_meaning(key, &meaning) && meaning.step) {
                    /* The key steps a field or the axis: beside the key it is,
                       draw the arrow it acts as - on this screen `B`/`C` are
                       the arrows, and the keyboard's own arrows do the same. */
                    RECT arrow_rect = r;

                    arrow_rect.left += 40;
                    arrow_rect.right -= 4;
                    arrow_rect.bottom = arrow_rect.top + 20;
                    SetTextColor(dc, RGB(120, 190, 150));
                    DrawTextW(dc, key == 'B' ? L"\u25C0" : L"\u25B6", 1,
                              &arrow_rect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                if (meaning.label) {
                    RECT label_rect = r;

                    label_rect.top = label_rect.top + 20;
                    /* The footer's own labels read one colour; a key the footer
                       does not name is the screen's (off-menu) and reads
                       dimmer - the usage block above says what it does. */
                    SetTextColor(dc, meaning.on_menu ? RGB(178, 208, 178)
                                                     : RGB(166, 166, 176));
                    SelectObject(dc, small);
                    DrawTextA(dc, meaning.label, -1, &label_rect,
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
            "F1-F4", "arrows", "Enter", "Esc", "Backspace", "W / Del", "- ."
        };
        static const char *const help_means[] = {
            "modes", "word / field", "accept (D)", "cancel / mode (A)",
            "delete (*)", "finish (#)", "sign / point"
        };
        int line;

        TextOutA(dc, PANEL_W + SIDE_X, SIDE_KEYS_NAME_Y, "PC keyboard", 11);
        for (line = 0; line < (int)(sizeof(help_keys) / sizeof(help_keys[0])); line++) {
            int y = SIDE_KEYS_Y + line * SIDE_KEYS_STEP;
            TextOutA(dc, PANEL_W + SIDE_X, y, help_keys[line],
                     (int)strlen(help_keys[line]));
            TextOutA(dc, PANEL_W + SIDE_X + 72, y, help_means[line],
                     (int)strlen(help_means[line]));
        }
    }
    SelectObject(dc, font);

    for (i = 0; i < g_button_count; i++) {
        const host_button_t *b = &g_buttons[i];
        RECT r = { PANEL_W + b->x, b->y, PANEL_W + b->x + b->w, b->y + b->h };
        HGDIOBJ old_brush = SelectObject(dc, b->mode >= 0 ? mode_button : button);
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

/* The emulated panel on its own: one 32bpp top-down buffer handed to GDI, which
   is why the panel never tears. */
static void host_draw_panel(HDC dc)
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
}

/* The whole bench: the emulated panel, then the machine keys beside it. */
static void host_draw_bench(HDC dc)
{
    host_draw_panel(dc);
    host_draw_side(dc);
}

/* What the strip beside the panel shows, as one string: the screen's name, the
   words it hands out (its usage lines and what every pad key means there) and
   the spindle. The strip is redrawn only when this changes - see host_paint(). */
static void host_side_signature(char *out, size_t out_sz)
{
    const char *const *usage = 0;
    size_t lines = nc_visual_usage(&usage);
    size_t used;
    char spindle[32];
    int row;
    int col;

    if (!out || out_sz == 0u) {
        return;
    }
    host_spindle_text(spindle, sizeof(spindle));
    used = (size_t)snprintf(out, out_sz, "%s|%s|", nc_visual_screen_name(),
                            spindle);
    for (row = 0; row < PAD_ROWS; row++) {
        for (col = 0; col < PAD_COLS; col++) {
            nc_visual_key_meaning_t meaning;
            char cell[24];
            int n;

            if (!host_key_meaning(g_pad_keys[row][col], &meaning) ||
                !meaning.label) {
                continue;
            }
            n = snprintf(cell, sizeof(cell), "%c=%s%c%c|",
                         g_pad_keys[row][col], meaning.label,
                         meaning.on_menu ? 'm' : 'o',
                         meaning.step ? 's' : '-');
            if (n > 0 && used + (size_t)n < out_sz) {
                memcpy(out + used, cell, (size_t)n + 1u);
                used += (size_t)n;
            }
        }
    }
    for (row = 0; row < (int)lines; row++) {
        int n = snprintf(out + used, out_sz - used, "%s|", usage[row]);

        if (n < 0 || (size_t)n >= out_sz - used) {
            break;
        }
        used += (size_t)n;
    }
}

/* The strip is dozens of GDI calls (fills, rounded keys, text); the panel is
   one buffer blit. Painting the strip straight onto the window every 20 ms put
   those calls on the glass one at a time - the strip visibly blinked while a
   feed or a run repainted. So the whole bench is composed in a memory bitmap
   and the window gets one BitBlt: the panel is redrawn every frame, the strip
   only when host_side_signature() says what it shows has changed.

   The bitmap is the station's for its lifetime (one window), and its pixels are
   handed out so `--painttest` can read what was composed. */
static HDC g_bench_dc;
static void *g_bench_bits;

static bool host_bench_backbuffer(HDC window, HDC *dc_out, void **pixels_out)
{
    if (!g_bench_dc && window) {
        BITMAPINFO info;
        HBITMAP bitmap;

        memset(&info, 0, sizeof(info));
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = WIN_W;
        info.bmiHeader.biHeight = -WIN_H;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        g_bench_dc = CreateCompatibleDC(window);
        bitmap = g_bench_dc
                     ? CreateDIBSection(g_bench_dc, &info, DIB_RGB_COLORS,
                                        &g_bench_bits, NULL, 0)
                     : NULL;
        if (!g_bench_dc || !bitmap) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            if (g_bench_dc) {
                DeleteDC(g_bench_dc);
            }
            g_bench_dc = NULL;
            g_bench_bits = NULL;
        } else {
            RECT strip = { PANEL_W, 0, WIN_W, WIN_H };
            HBRUSH back = CreateSolidBrush(RGB(24, 26, 28));

            SelectObject(g_bench_dc, bitmap);
            /* The panel is blitted over its own half every frame; this is the
               strip's backdrop until its first composition. */
            FillRect(g_bench_dc, &strip, back);
            DeleteObject(back);
        }
    }
    if (!g_bench_dc) {
        return false;
    }
    if (dc_out) {
        *dc_out = g_bench_dc;
    }
    if (pixels_out) {
        *pixels_out = g_bench_bits;
    }
    return true;
}

bool host_compose_bench(HDC window, HDC *dc_out, void **pixels_out)
{
    static char drawn[1024];
    char signature[1024];
    HDC bench;
    void *bits;

    if (!host_bench_backbuffer(window, &bench, &bits)) {
        return false;
    }
    host_draw_panel(bench);
    host_side_signature(signature, sizeof(signature));
    if (strcmp(signature, drawn) != 0) {
        host_draw_side(bench);
        memcpy(drawn, signature, sizeof(drawn));
    }
    if (dc_out) {
        *dc_out = bench;
    }
    if (pixels_out) {
        *pixels_out = bits;
    }
    return true;
}

static void host_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC bench = NULL;

    if (!host_compose_bench(dc, &bench, NULL)) {
        /* No memory bitmap: draw straight, as before. */
        host_draw_bench(dc);
    } else {
        BitBlt(dc, 0, 0, WIN_W, WIN_H, bench, 0, 0, SRCCOPY);
    }
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
void host_run_machine(unsigned elapsed_ms)
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
    case WM_ERASEBKGND:
        /* Every pixel is painted from the composed bench (host_paint()), so the
           background is not erased first: erasing it is what makes a window
           blink between frames. */
        return 1;
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


/* Where the station keeps its card when the operator just opens it: `nc-files`
   beside the exe, so double-clicking the downloaded station (or a shortcut to
   it) mounts the same folder whatever working directory Windows hands over.
   `--files` still names another one, and the headless checks always do. */
void host_files_root_beside_exe(const char *argv0)
{
    char path[260];
    char *slash;

    if (g_files_root[0]) {
        return;
    }
    if (GetModuleFileNameA(NULL, path, (DWORD)sizeof(path)) == 0u) {
        snprintf(g_files_root, sizeof(g_files_root), "%s", argv0 ? argv0 : "nc-files");
        return;
    }
    slash = strrchr(path, '\\');
    if (!slash) {
        snprintf(g_files_root, sizeof(g_files_root), ".\\nc-files");
        return;
    }
    *slash = '\0';
    snprintf(g_files_root, sizeof(g_files_root), "%s\\nc-files", path);
}

/* Where the demo files sit: `examples\` beside the exe, the folder the release
   zip unpacks next to the station. */
static void host_examples_beside_exe(char *out, size_t out_sz)
{
    char path[260];
    char *slash;

    if (!out || out_sz == 0u) {
        return;
    }
    if (GetModuleFileNameA(NULL, path, (DWORD)sizeof(path)) == 0u) {
        snprintf(out, out_sz, "examples");
        return;
    }
    slash = strrchr(path, '\\');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(path, sizeof(path), ".");
    }
    snprintf(out, out_sz, "%s\\examples", path);
}

/* The demo card - see host_shell.h. A card with a program of its own is the
   operator's, so it is not touched at all: only a card with no `.nc` in it is
   seeded, and only with files that are not there yet. */
int host_seed_card(const char *root, const char *examples)
{
    char dest_dir[260];
    char path[260];
    WIN32_FIND_DATAA find;
    HANDLE scan;
    int copied = 0;

    if (!root || !*root || !examples || !*examples) {
        return 0;
    }
    snprintf(dest_dir, sizeof(dest_dir), "%s\\nc\\files", root);
    /* Any program already on the card means the card is in use. */
    snprintf(path, sizeof(path), "%s\\*.nc", dest_dir);
    scan = FindFirstFileA(path, &find);
    if (scan != INVALID_HANDLE_VALUE) {
        FindClose(scan);
        return 0;
    }
    snprintf(path, sizeof(path), "%s\\*", examples);
    scan = FindFirstFileA(path, &find);
    if (scan == INVALID_HANDLE_VALUE) {
        return 0;
    }
    /* The driver expects the folders to be there (`host_fs_mount()` makes them
       itself when it mounts, which is after this runs). */
    snprintf(path, sizeof(path), "%s\\nc", root);
    (void)CreateDirectoryA(root, NULL);
    (void)CreateDirectoryA(path, NULL);
    (void)CreateDirectoryA(dest_dir, NULL);
    do {
        char src[260];
        char dst[260];
        const char *dot;

        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        dot = strrchr(find.cFileName, '.');
        if (!dot || (_stricmp(dot, ".nc") != 0 && _stricmp(dot, ".t") != 0)) {
            continue;
        }
        snprintf(src, sizeof(src), "%s\\%s", examples, find.cFileName);
        snprintf(dst, sizeof(dst), "%s\\%s", dest_dir, find.cFileName);
        if (CopyFileA(src, dst, TRUE)) {
            copied++;
        }
    } while (FindNextFileA(scan, &find));
    FindClose(scan);
    return copied;
}

/* Which build this exe is - see host_shell.h. The station has no version
   resource, so its own file is the answer: the timestamp and size Explorer
   shows in Properties, which is what tells a fresh build from one that was
   copied around for a day. */
void host_build_text(char *out, size_t out_sz)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    FILETIME local;
    SYSTEMTIME st;
    char path[260];

    if (!out || out_sz == 0u) {
        return;
    }
    out[0] = '\0';
    if (GetModuleFileNameA(NULL, path, (DWORD)sizeof(path)) == 0u ||
        !GetFileAttributesExA(path, GetFileExInfoStandard, &info) ||
        !FileTimeToLocalFileTime(&info.ftLastWriteTime, &local) ||
        !FileTimeToSystemTime(&local, &st)) {
        snprintf(out, out_sz, "unknown build");
        return;
    }
    snprintf(out, out_sz, "%04u-%02u-%02u %02u:%02u:%02u, %lu bytes",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
             (unsigned long)info.nFileSizeLow);
}

char g_key_script[160];
unsigned g_ticks;
char g_files_root[260];

/* Replays the machine's own key path - the keypad's key characters through
   nc_visual_key_for_char(), and the mode keys - so what a dump shows is what
   the panel shows after the same presses. */
void host_play_keys(const char *script)
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
            /* A PC key with a fixed meaning is accepted as well, so a script
               can press what an operator presses: `W` is the keypad's `#` (the
               VIEW key on EDIT), and Delete, Esc and Backspace are theirs. The
               machine's own characters were taken above. */
            {
                char machine = host_pc_machine_key((unsigned)token[0]);

                if (machine) {
                    nc_visual_handle_key(nc_visual_key_for_char(machine));
                    continue;
                }
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

void host_init_core(void)
{
    cnc_init();
    cnc_unit_test_start();
    host_fs_mount(g_files_root[0] ? g_files_root : NULL);
    nc_run_init();
    nc_visual_init();
}

int host_dump(const char *path)
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
int host_dump_bench(const char *path)
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

/* The machine's main loop is `cnc_parse_cmd(); cnc_dotasks();` with the clock
   running, so a test that wants motion has to do all three. */
void host_pump(unsigned iterations)
{
    unsigned i;

    for (i = 0u; i < iterations; i++) {
        (void)cnc_parse_cmd();
        cnc_dotasks();
        /* The panel's own main-loop work, the way the firmware's `nc` module
           runs it: the idle tasks are where the program is written to the card
           once the operator has stopped typing, so a harness that skips them
           cannot see an edit land in a file. */
        nc_visual_idle_tasks();
        mcu_unit_test_advance_time(1000u);
    }
}

/* Let everything the panel queued run out: the parser takes one line per
   iteration and the planner needs the clock to execute what it took. */
void host_pump_idle(unsigned max_iterations)
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
    host_files_root_beside_exe(argc > 0 ? argv[0] : NULL);
    /* Every headless flag - the dumps and the checks - belongs to the checks
       (host_tests_run(), host_tests.c). A flag they do not name is the
       window, with whatever keys `--keys` queued and `--ticks` ticks run. */
    {
        int result = host_tests_run(argc, argv);

        if (result >= 0) {
            return result;
        }
    }

    /* The window: give a fresh card the demo the release ships beside the exe,
       then run the panel on it. */
    {
        char examples[260];

        host_examples_beside_exe(examples, sizeof(examples));
        (void)host_seed_card(g_files_root[0] ? g_files_root : "nc-files",
                             examples);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = host_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "nc_ui_host";
    if (!RegisterClassA(&wc))
        return 1;

    /* The title carries the build, so the window itself says which station this
       is - `--version` prints the same line for a script. */
    {
        char build[80];
        char title[128];

        host_build_text(build, sizeof(build));
        snprintf(title, sizeof(title), "uCNC programming station (PC) - %s",
                 build);
        hwnd = CreateWindowExA(0, wc.lpszClassName,
                               title,
                               WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               WIN_W + 16, WIN_H + 60, NULL, NULL, wc.hInstance,
                               NULL);
    }
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
