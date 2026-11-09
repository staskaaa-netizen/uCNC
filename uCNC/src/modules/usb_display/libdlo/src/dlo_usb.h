/** @file dlo_usb.h
 *
 *  @brief Header file for the USB-specific connectivity functions.
 *
 *  This file defines the API between the libdlo.c and the USB driver implementation. This
 *  example implementation uses libusb but it should be simple to replace dlo_usb.c with
 *  some alternative implementation.
 *
 *  DisplayLink Open Source Software (libdlo)
 *  Copyright (C) 2009, DisplayLink
 *  www.displaylink.com
 *
 *  This library is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU Library General Public License as published by the Free
 *  Software Foundation; LGPL version 2, dated June 1991.
 *
 *  This library is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU Library General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef DLO_USB_H
#define DLO_USB_H        /**< Avoid multiple inclusion. */

#include "dlo_structs.h"


/** Return the meaning of the last USB-related error as a human-readable string.
 *
 *  @return  Pointer to error message string (zero-terminated).
 */
extern char * dlo_usb_strerror(void);


/** Initialise the USB communications routines.
 *
 *  @param  flags  Initialisation flags word (unused flags ignored).
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_init(const dlo_init_t flags);


/** Finalisation call for USB communications routines.
 *
 *  @param  flags  Finalisation flags word (unused flags ignored).
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_final(const dlo_final_t flags);


/** Update or create nodes on the device list for any DisplayLink devices found.
 *
 *  @param  init  Is this the first call to the enumeration function?
 *
 *  @return  Return code, zero for no error.
 *
 *  This call will look for any DisplayLink devices which use the particular connection
 *  implementation and either add nodes onto the device list or update nodes if they are
 *  already present.
 *
 *  Once enumeration is complete for all connection types, any nodes on the list which
 *  haven't just been added or updated are removed (as the correponding device can no
 *  longer be found).
 */
extern dlo_retcode_t dlo_usb_enumerate(const bool init);


/** Open a connection to the specified device.
 *
 *  @param  dev  Pointer to @a dlo_device_t structure.
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_open(dlo_device_t * const dev);


/** Close the connection with a specified device.
 *
 *  @param  dev  Pointer to @a dlo_device_t structure.
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_close(dlo_device_t * const dev);


/** Select the input channel in the specified device.
 *
 *  @param  dev   Pointer to @a dlo_device_t structure.
 *  @param  buf   Pointer to the buffer containing the channel information.
 *  @param  size  Size of the buffer (bytes).
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_chan_sel(const dlo_device_t * const dev, const char * const buf, const size_t size);


/** Switch to the default input channel in the specified device.
 *
 *  @param  dev  Pointer to @a dlo_device_t structure.
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_std_chan(const dlo_device_t * const dev);


/** Flush the command buffer contents to the specified device.
 *
 *  @param  dev  Pointer to @a dlo_device_t structure.
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_write(dlo_device_t * const dev);


/** Discard command buffer contents without sending them.
 *
 *  @param  dev  Pointer to @a dlo_device_t structure.
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_discard(dlo_device_t * const dev);


/** Write the contents of a specified command buffer to the specified device.
 *
 *  @param  dev   Pointer to @a dlo_device_t structure.
 *  @param  buf   Pointer to the buffer containing commands to write.
 *  @param  size  Size of the buffer (bytes).
 *
 *  @return  Return code, zero for no error.
 */
extern dlo_retcode_t dlo_usb_write_buf(dlo_device_t * const dev, char * buf, size_t size);


/** Register a callback invoked repeatedly while a bulk write waits for the
 *  endpoint to come idle. Bulk transfers of full frames can hold the caller
 *  for hundreds of milliseconds at full-speed wire rates; a host that must
 *  keep servicing other real-time work (e.g. an ISA bus trap fabric) hooks
 *  it here. The callback runs in the waiting task's context, interleaved
 *  with tuh_task(), and must not issue DisplayLink operations.
 *
 *  @param  callback  Function to call while waiting, or NULL to clear.
 */
extern void dlo_usb_set_idle_callback(void (*callback)(void));


/** Report whether the device's bulk OUT endpoint still has a transfer in flight.
 *
 *  @param  uid  Unique ID of the device.
 *
 *  @return true while a submitted bulk transfer has not yet completed.
 *
 *  dlo_flush_usb() deliberately leaves the final transfer running, so a caller
 *  that needs to time or serialise a whole frame waits here (calling
 *  tuh_task() in between) instead of guessing at a delay.
 */
extern bool dlo_usb_endpoint_busy(const dlo_dev_t uid);


/** Total bytes handed to the bulk OUT endpoint since reset. */
extern uint64_t dlo_usb_bytes_sent_total(void);


/** Total number of accepted bulk OUT submissions since reset. */
extern uint32_t dlo_usb_xfer_count_total(void);


/** Total number of failed bulk OUT submissions or failed control transfers since reset. */
extern uint32_t dlo_usb_error_count_total(void);


/** Bulk OUT endpoint address the device is open on (0 if not open). */
extern uint8_t dlo_usb_bulk_out_ep(const dlo_dev_t uid);


/** Bulk IN endpoint address reported by the device (0 if none); diagnostics only. */
extern uint8_t dlo_usb_bulk_in_ep(const dlo_dev_t uid);


#endif
