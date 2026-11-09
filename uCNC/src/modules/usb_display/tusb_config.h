/*
 * TinyUSB configuration for the uCNC RP2350 builds whose video goes out over
 * USB to a DisplayLink monitor.
 *
 * The device half is the framework's own configuration, kept exactly as it is
 * so the Arduino core's USB objects still find the device symbols they expect -
 * this firmware never starts the device side, it only has to link. The host
 * half is added here, because that is what the display needs: the native
 * controller is a full-speed host, and the DisplayLink bulk endpoints are
 * driven by the vendored tusb_libdlo through the raw endpoint API.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUSB_MCU
 #define CFG_TUSB_MCU             OPT_MCU_RP2040
#endif

#define CFG_TUSB_OS               OPT_OS_PICO

/* One controller, two roles in one build: the device side exists because the
 * Arduino core's own USB objects are compiled against it, while this firmware
 * initialises the controller as the host and never starts the device. */
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_HOST)
#define CFG_TUD_ENABLED           1

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG           0
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION (link-only: the display owns the controller as host)
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

/* The framework's own device-class set, unchanged: the Arduino core's USB
 * objects are compiled against these macros, so they have to keep the values
 * that core expects even though this firmware never starts the device side. */
#define CFG_TUD_HID              (2)
#define CFG_TUD_CDC              (1)
#define CFG_TUD_MSC              (1)
#define CFG_TUD_MIDI             (1)
#define CFG_TUD_VENDOR           (0)
#define CFG_TUD_NCM              (1)

#define CFG_TUD_CDC_RX_BUFSIZE  (256)
#define CFG_TUD_CDC_TX_BUFSIZE  (256)
#define CFG_TUD_MSC_EP_BUFSIZE  (64)
#define CFG_TUD_HID_EP_BUFSIZE  (64)
#define CFG_TUD_MIDI_RX_BUFSIZE (64)
#define CFG_TUD_MIDI_TX_BUFSIZE (64)

/* The framework's own values for a 1460-byte MSS, spelled out: this file is
 * force-included for every compilation in this target, including the
 * assembler, so it must not pull in C headers of its own. */
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE (3020)
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE (3020)
#ifndef CFG_TUD_NCM_OUT_NTB_N
#define CFG_TUD_NCM_OUT_NTB_N 1
#endif
#ifndef CFG_TUD_NCM_IN_NTB_N
#define CFG_TUD_NCM_IN_NTB_N 1
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION - the DisplayLink monitor
//--------------------------------------------------------------------

#define CFG_TUH_ENABLED          1

/* The native controller, not PIO USB. */
#define CFG_TUH_RPI_PIO_USB      0
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT         CFG_TUH_RPI_PIO_USB
#endif

/* RP2350 native USB is full speed only. */
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED      OPT_MODE_FULL_SPEED
#endif
#define CFG_TUH_MAX_SPEED        BOARD_TUH_MAX_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 1024

/* A powered hub may sit between the board and the monitor. */
#define CFG_TUH_HUB              1
#define CFG_TUH_DEVICE_MAX       (CFG_TUH_HUB ? (CFG_TUH_HUB * 4) : 1)

/* No TinyUSB class driver on the host side: tusb_libdlo opens the DisplayLink
 * bulk endpoint itself through the raw endpoint API. */
#define CFG_TUH_CDC              0
#define CFG_TUH_HID              0
#define CFG_TUH_MSC              0
#define CFG_TUH_VENDOR           0

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
