# Working SD-card integration: history and update constraints

Audited 2026-09-16 after fetching upstream core and modules master. Latest
modules master remains `66063076` (2026-09-03), already integrated locally in
`a84b783c`. This audit changes no driver, wiring or mount behavior. The current
board/card combination is reported working by the user; no new hardware tests
were performed during this audit.

## Relevant history

| Commit | Change |
| --- | --- |
| Upstream `e68ee3db` (2025-05-04) | Configurable SPI DMA and startup automount. |
| Local `29495c97` (2026-05-01) | Fixed directory attribute bit test and directory-entry size/timestamp output. |
| Local `db01ca1d` (2026-05-14) | Added an opt-out for automount. |
| Local `dace1de9` (2026-05-19) | Explicit CS output/high initialization, boot delay, mount retries and detailed bring-up diagnostics. |
| Upstream `66063076` (2026-09-03, PR 111) | Advance buffer between written sectors; boot/CMD0 delays; directory pointer checks; force CS setup; default to short filenames because long names were not working. |
| Local integration `a84b783c` | Retained local fixes and board profile while applying upstream improvements; added CS definitions/include and custom-driver guard needed by the integrated module. |

## Preserve this working combination

- Software SPI (`SD_CARD_INTERFACE=0`); this onboard pin assignment is not a
  valid single RP2350 hardware-SPI pin group. CLK GPIO30, MOSI GPIO31, MISO GPIO40,
  CS GPIO43, as configured in `uCNC/boardmap_overrides.h`.
- DMA disabled; card-detect pin undefined (255); startup automount enabled.
- Local boot delay 500 ms, up to three mount attempts, 250 ms retry delay.
- NC uses the module's mounted `/D` filesystem and its own file UI; retain
  `SD_CARD_NO_SYSTEM_MENU`.
- Retain local directory attribute/metadata fixes and optional bring-up logs.
- Short filename default (`FF_USE_LFN=0`) is an intentional upstream change.
  Do not silently re-enable long filenames as a UI enhancement.

A blind replacement with upstream's directory would discard local fixes.
Compare future upstream changes from `66063076` and integrate selectively.
Some bundled README guidance is stale: its software-I2C description is wrong
for this SPI driver and its long-filename default no longer matches ffconf.h.

## Checks before calling any future SD change validated

- Cold power-on repeatedly with the current card; repeat with no card.
- List nested directories and verify file/directory types, sizes and names.
- Save/reopen an NC file larger than 512 bytes; compare complete contents,
  including across multiple sectors. Reboot and reopen it.
- Verify short-name creation and existing filenames in the NC file manager.
- Exercise explicit unmount/remount only while no file operation is active;
  do not claim automatic removal detection with no detect pin configured.
- Check failures are visible without discarding an unsaved editor document.

The settings-invalid lock seen after updating firmware is a settings validation
failure. A working SD mount alone neither explains nor repairs it; this audit
does not establish that SD corruption caused that incident.
