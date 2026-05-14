#ifndef LEANCAM_PIO_USB_COMPAT_H
#define LEANCAM_PIO_USB_COMPAT_H

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

uint32_t pio_usb_host_get_frame_number(void);
bool pio_usb_host_endpoint_close(uint8_t root_id, uint8_t dev_addr, uint8_t ep_addr);
bool pio_usb_host_endpoint_abort_transfer(uint8_t root_id, uint8_t dev_addr, uint8_t ep_addr);

#endif

#endif
