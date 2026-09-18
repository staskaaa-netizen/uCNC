# nc_ui - NC panel on Windows

Runs the NC screen exactly as the LVDS panel draws it, plus the proprietary key
row the machine has in hardware. This is a UI test bench: the firmware layout,
coordinate system, palette and bitmap fonts are used as-is.

## How the layout stays identical

`modules/nc/nc_visual.c` draws through a small primitive API (`lvds_hstx_*` and
`lvds_draw_*`). On the machine the RP2350/HSTX backend implements it; here
`lvds_host.c` implements the same API over a 32bpp frame that GDI blits into the
window, and the firmware bitmap fonts (`lvds_font_cond_6x8.c`,
`lvds_font_ibm_8x14.c`) render the glyphs. Nothing in the screen code changes,
so a host frame is what the panel shows.

The rest of the stack is the real firmware too, built for the virtual MCU the
same way the parser test suite builds it: core, parser, NC state/menu/run,
G7x, palette.

## Build and render a frame

```powershell
python tools\test_nc_ui.py     # builds tools\nc_ui_win and dumps tmp\nc-ui-tests\frame.bmp
```

or

```powershell
cd tools\nc_ui_win
mingw32-make
build\nc_ui.exe --dump frame.bmp
build\nc_ui.exe
```

## Window layout

The window is the emulated panel (800x600, exactly as the panel draws) with the
machine keys attached to the right side, the way the proprietary keyboard sits
next to the display:

| Keys | Function |
| --- | --- |
| F1-F6 | operation modes: MANUAL, EDIT, SIM, MDI, TOOLS, RUN |
| F7-F12 | soft keys (provisional mapping: ACCEPT, NEXT, PREV, FINISH, BACKSPACE, MODE) |
| 3x3 pad | digits 1-9, exactly the machine's numeric grid |
| 0 / ENTER / BACK / PAGE- / END / PAGE+ | 0, accept, backspace, previous, finish, next |

The PC keyboard maps the same way: F1-F12 as above, digits and the numeric pad
to the digit keys, Enter/Esc/Backspace/Delete/arrows to the control keys.

`nc_visual_select_mode()` was added to the NC module for this shell so the
F1-F6 keys jump straight to a mode instead of cycling with the MODE key; the
MODE key still cycles for the machine.

## Open questions

- The on-screen 3x3 mapping at the bottom of the panel is still drawn by the
  firmware. Whether the side pad should follow that mapping, or the screen
  should be changed, is the layout question this bench is meant to answer.
- Run currently exercises the virtual machine only. Wiring RUN to a real
  controller means giving `nc_run` a Grbl transport (see
  `tools/nc_sender/grbl_stream.c`) instead of the parser stream.
