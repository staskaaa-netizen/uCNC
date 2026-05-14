#include "leancam_pio_usb_compat.h"

uint32_t pio_usb_host_get_frame_number(void)
{
    return 0;
}

bool pio_usb_host_endpoint_close(uint8_t root_id, uint8_t dev_addr, uint8_t ep_addr)
{
    (void)root_id;
    (void)dev_addr;
    (void)ep_addr;
    return true;
}

bool pio_usb_host_endpoint_abort_transfer(uint8_t root_id, uint8_t dev_addr, uint8_t ep_addr)
{
    (void)root_id;
    (void)dev_addr;
    (void)ep_addr;
    return true;
}
