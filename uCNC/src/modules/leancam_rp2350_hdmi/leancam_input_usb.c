#include "leancam_input_usb.h"

#include <stdint.h>
#include <string.h>

#include "tusb.h"

#if CFG_TUH_RPI_PIO_USB
#include "pio_usb.h"
#endif

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

#define LC_INPUT_QUEUE_LEN 32
#define LC_INPUT_MAX_REPORT 4

static lc_input_event_t g_queue[LC_INPUT_QUEUE_LEN];
static volatile uint8_t g_head;
static volatile uint8_t g_tail;
static uint8_t g_prev_keys[6];
static hid_keyboard_report_t g_prev_report;
static volatile bool g_keyboard_connected;
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[LC_INPUT_MAX_REPORT];
} g_hid_info[CFG_TUH_HID];

static char hid_to_ascii(uint8_t key, bool shift);

static void push_event(lc_input_type_t type, char ch)
{
    uint8_t next = (uint8_t)((g_head + 1u) % LC_INPUT_QUEUE_LEN);
    if (next == g_tail) {
        return;
    }
    g_queue[g_head].type = type;
    g_queue[g_head].ch = ch;
    g_head = next;
}

static bool key_was_down(uint8_t key)
{
    for (unsigned i = 0; i < sizeof(g_prev_keys); ++i) {
        if (g_prev_keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void process_keyboard_report(const hid_keyboard_report_t *kbd)
{
    bool shift = (kbd->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    g_keyboard_connected = true;
    for (unsigned i = 0; i < sizeof(kbd->keycode); ++i) {
        uint8_t key = kbd->keycode[i];
        char ch;
        if (key == 0 || key_was_down(key)) {
            continue;
        }

        switch (key) {
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER: push_event(LC_INPUT_ENTER, 0); break;
        case HID_KEY_ESCAPE: push_event(LC_INPUT_ESC, 0); break;
        case HID_KEY_BACKSPACE: push_event(LC_INPUT_BACKSPACE, 0); break;
        case HID_KEY_ARROW_UP: push_event(LC_INPUT_UP, 0); break;
        case HID_KEY_ARROW_DOWN: push_event(LC_INPUT_DOWN, 0); break;
        case HID_KEY_ARROW_LEFT: push_event(LC_INPUT_LEFT, 0); break;
        case HID_KEY_ARROW_RIGHT: push_event(LC_INPUT_RIGHT, 0); break;
        case HID_KEY_TAB: push_event(shift ? LC_INPUT_SHIFT_TAB : LC_INPUT_TAB, 0); break;
        default:
            ch = hid_to_ascii(key, shift);
            if (ch) push_event(LC_INPUT_CHAR, ch);
            break;
        }
    }

    memcpy(g_prev_keys, kbd->keycode, sizeof(g_prev_keys));
    g_prev_report = *kbd;
}

static void process_generic_report(uint8_t instance, uint8_t const *report, uint16_t len)
{
    tuh_hid_report_info_t *rpt_info = NULL;
    uint8_t rpt_count;

    if (instance >= CFG_TUH_HID || !report || len == 0) {
        return;
    }

    rpt_count = g_hid_info[instance].report_count;
    if (rpt_count == 1 && g_hid_info[instance].report_info[0].report_id == 0) {
        rpt_info = &g_hid_info[instance].report_info[0];
    } else if (rpt_count > 0) {
        uint8_t rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; ++i) {
            if (rpt_id == g_hid_info[instance].report_info[i].report_id) {
                rpt_info = &g_hid_info[instance].report_info[i];
                break;
            }
        }
        if (len > 0) {
            ++report;
            --len;
        }
    }

    if (!rpt_info || len < sizeof(hid_keyboard_report_t)) {
        return;
    }

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP &&
        rpt_info->usage == HID_USAGE_DESKTOP_KEYBOARD) {
        process_keyboard_report((const hid_keyboard_report_t *)report);
    }
}

static char hid_to_ascii(uint8_t key, bool shift)
{
    if (key >= HID_KEY_A && key <= HID_KEY_Z) {
        char ch = (char)('a' + key - HID_KEY_A);
        return shift ? (char)(ch - ('a' - 'A')) : ch;
    }
    if (key >= HID_KEY_1 && key <= HID_KEY_9) {
        static const char normal[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        return shift ? shifted[key - HID_KEY_1] : normal[key - HID_KEY_1];
    }
    if (key == HID_KEY_0) return shift ? ')' : '0';
    if (key == HID_KEY_KEYPAD_MULTIPLY) return '*';
    if (key == HID_KEY_KEYPAD_ADD && shift) return '*';
    if (key == HID_KEY_3 && shift) return '#';
    if (key == HID_KEY_MINUS) return shift ? '_' : '-';
    if (key == HID_KEY_EQUAL) return shift ? '+' : '=';
    if (key == HID_KEY_PERIOD || key == HID_KEY_KEYPAD_DECIMAL) return '.';
    return 0;
}

void lc_input_usb_init(void)
{
#if CFG_TUH_RPI_PIO_USB
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIO_USB_DP_PIN_DEFAULT;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
#endif
    tuh_init(BOARD_TUH_RHPORT);
    memset(&g_prev_report, 0, sizeof(g_prev_report));
    memset(g_prev_keys, 0, sizeof(g_prev_keys));
}

void lc_input_usb_task(void)
{
    tuh_task();
}

bool lc_input_usb_poll(lc_input_event_t *event)
{
    if (!event || g_tail == g_head) {
        return false;
    }
    *event = g_queue[g_tail];
    g_tail = (uint8_t)((g_tail + 1u) % LC_INPUT_QUEUE_LEN);
    return true;
}

bool lc_input_usb_keyboard_connected(void)
{
    return g_keyboard_connected;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        g_keyboard_connected = true;
    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE && instance < CFG_TUH_HID) {
        g_hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            g_hid_info[instance].report_info, LC_INPUT_MAX_REPORT, desc_report, desc_len);
        for (uint8_t i = 0; i < g_hid_info[instance].report_count; ++i) {
            if (g_hid_info[instance].report_info[i].usage_page == HID_USAGE_PAGE_DESKTOP &&
                g_hid_info[instance].report_info[i].usage == HID_USAGE_DESKTOP_KEYBOARD) {
                g_keyboard_connected = true;
            }
        }
    }
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr;
    (void)instance;
    g_keyboard_connected = false;
    memset(g_prev_keys, 0, sizeof(g_prev_keys));
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
        process_keyboard_report((const hid_keyboard_report_t *)report);
    } else {
        process_generic_report(instance, report, len);
    }
    tuh_hid_receive_report(dev_addr, instance);
}
