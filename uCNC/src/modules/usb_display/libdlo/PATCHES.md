# Local patches to tusb_libdlo

Base revision: `8124b0e6708ab4ae7720394fa317cf0efc153295`

Everything below is a deliberate, minimal change needed to bring up an
unknown DisplayLink monitor (TVS UM80, `17E9:0205`) and to report *where* a
bring-up fails. Nothing changes the wire protocol.

## 1. Tracing independent of `DEBUG` (`dlo_defs.h`)

Added `DLO_LOG()`, gated on `DLO_LOG_ENABLE` (default on, `-DDLO_LOG_ENABLE=0`
to silence). Upstream's `DPRINTF()` is tied to `DEBUG`, which also enables
asserts and `exit()` on failure - not what a firmware bring-up wants.

## 2. Serial-number handling (`dlo_usb.c`)

`tuh_descriptor_get_serial_string_sync_utf8()`:

* retries with the device's first advertised language ID before giving up
  (upstream only asked for US English);
* reads the UTF-16 payload from `buf + 2` - upstream passed
  `(uint16_t *)buf + 2`, i.e. `buf + 4`, which dropped the first character;
* clamps the converted length to the caller's buffer.

`dlo_check_device()` no longer aborts when the serial string is missing or the
descriptor stalls; it synthesises `no-serial-<addr>` instead. Devices without
a serial descriptor are otherwise perfectly usable, and aborting there would
look like a protocol failure.

## 3. Endpoint discovery (`dlo_usb.c`)

`open_bulk_endpoint()` was rewritten to walk the descriptors that belong to the
vendor interface and stop at the next interface descriptor. Upstream assumed
exactly one class-specific descriptor between the interface and its endpoints
and started scanning from the *second* descriptor after the interface, so a
device with a different descriptor layout (a single endpoint, or several
class-specific descriptors) failed with `dlo_err_open` and no explanation.
The scan now records the bulk IN endpoint as well, for the diagnostic report.

## 4. Error propagation and instrumentation (`dlo_usb.c`, `dlo_mode.c`)

* EDID reads: the control-transfer result is checked and a failed transfer
  aborts the read. Upstream ignored the result, so a stalled transfer silently
  left the previous byte in the EDID buffer.
* Channel selection: `dlo_usb_chan_sel()` returns `dlo_err_usb` when the
  control transfer fails instead of always reporting success.
* Counters: `dlo_usb_bytes_sent_total()`, `dlo_usb_xfer_count_total()`,
  `dlo_usb_error_count_total()` and `dlo_usb_endpoint_busy()` expose the
  information phase 4 needs (bytes per frame, transfer errors, completion
  timing). `dlo_usb_endpoint_busy()` also lets the application wait for a
  bounded time instead of guessing at a delay.
* `DLO_LOG()` milestones at each init stage: status word and device type,
  interface/endpoint inventory, EDID result, programmed mode and framebuffer
  base.

## 5. `ep_in` field (`dlo_structs.h`)

`dlo_usb_dev_t` gained `ep_in` so the diagnostic report can print the bulk IN
endpoint. libdlo itself never reads from the device.

## 6. Readable USB error text (`dlo_usb.c`)

`dlo_usb_strerror()` returned `usb_err_str`, which the TinyUSB port never sets,
so every `dlo_err_usb` surfaced through `dlo_strerror()` as "Unknown error".
It now returns a fixed description of the one failure mode this port has.

## 7. Pointer arithmetic in `dlo_copy_host_bmp()` (`libdlo.c`)

The clipped-source-pointer arithmetic used `(unsigned long)`, which truncates a
pointer on any host where `long` is 32 bits (the host simulator under MinGW,
and LLP64 Windows in general). It now uses `uintptr_t`. The generated code on
the 32-bit target is unchanged; without this the host simulator crashes in
`scrape_24bpp()` on the bitmap-upload path.

## 8. Zeroed EDID on a new device node (`libdlo.c`)

`dlo_new_device()` left `dev->edid` uninitialised. Because `dlo_usb_open()`
still treats a failed EDID read as non-fatal, `dlo_mode_set_default()` would
then program "the monitor's preferred timing" from allocator residue - in
practice the previous session's EDID, because the freed device node is handed
straight back. The host simulator reproduced exactly that: with the simulated
monitor's EDID read forced to fail, the firmware still logged a 1280x800
preferred timing and never reached its own fixed-mode fallback.

`dev->edid` is now cleared, so a failed read leaves no timing, the mode set
reports no mode, and the application's fallback path runs and says so.

## 9. Plane-level experiment API (`dlo_experiment.h`, `dlo_grfx.c`, `dlo_mode.c`)

Compiled only when `DLO_EXPERIMENT_API` is defined, so the normal builds link
exactly what they linked before. It adds thin wrappers over the library's own
internals, for the 8-bit framebuffer experiment
(`docs/8BPP-EXPERIMENT.md`):

* `dlo_exp_get_bases()` / `dlo_exp_set_bases()` - read and move the two plane
  base addresses through the library's own `set_base()` (registers 0x20-0x22 and
  0x26-0x28 with its batch framing);
* `dlo_exp_set_color_depth()` - register 0x00, and it refuses any value other
  than 0 or 1 because no other value is documented in libdlo or udlfb;
* `dlo_exp_write_16bpp()` / `dlo_exp_write_8bpp()` - write one plane on its own
  with the library's existing encoders (`cmd_rlx16`/`cmd_raw16` and
  `cmd_rle8`/`cmd_raw8`), chunked to 256 pixels per command as the library's own
  callers do;
* `dlo_exp_low_bits_8bpp()` - the library's `rgb8()` packing, so the experiment
  cannot invent its own byte layout for the fine-detail plane.

Rationale for keeping this inside the library rather than re-implementing the
commands in the application: the experiment then cannot emit a byte sequence the
library would not, which is what the task's "no invented register values"
constraint requires. `tests/host_sim` (target `experiment`) decodes everything
the API emits and checks the addresses and register values.

## Not changed

* The DL-1x0/DL-1x5 register sequence, the mode scripts in `dlo_data.h`, the
  run-length encodings and the command framing are upstream code.
* Upstream's `tuh_mount_cb()`/`tuh_umount_cb()` are `weak`, so the firmware's
  own definitions replace them; the library still links, and the application
  drives `dlo_check_device()`/`dlo_claim_first_device()` itself.
* Upstream `dlo_claim_device()` ignores the result of `dlo_mode_set_default()`.
  That is left as-is because the application checks the resulting mode
  (`dlo_get_mode()`) and runs its own fallback path when no mode came out.
