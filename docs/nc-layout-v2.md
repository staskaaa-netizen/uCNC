# NC panel layout v2: no header, no footer, dedicated 3x3

Target layout for the 800x600 LVDS panel (and therefore for the Windows shell,
which renders the same screen).

## What changes

- **Header is removed.** It held the mode/title, the settings and alarm banner,
  the fps counter and the preview legend.
- **Footer is removed as a key strip.** It was taller than needed and also
  carried the operator message.
- **A bottom bar is added** across the full width, with two areas:
  - **left: DRO and status** - position, feed/spindle, mode, run state, and the
    operator message line (alerts, rejected edits, field reasons);
  - **right: a dedicated 3x3 key grid**, no longer drawn inside the panel body.
- **Code view shows 14-16 rows** (the header/footer space goes to the code
  pane).
- **Tool description moves** to a strip at the bottom of the graphic pane.
- **File name is removed** from the code pane.

## Wireframe

```text
 0                                                              800
 +---------------------------------------------+----------------------+  0
 |                                             |                      |
 | graphic pane (preview / live)               | code pane            |
 |                                             |                      |
 |                                             | 14-16 rows           |
 |                                             |                      |
 |                                             |                      |
 | [ tool description strip ]                  |                      |
 +---------------------------------------------+----------------------+ 480
 | DRO / status / message                      |  3x3 key grid        |
 | X 0.000  Z 0.000  F 0.0  S 0                |  [1] [2] [3]         |
 | mode + run state + alerts                   |  [4] [5] [6]         |
 | operator message line                       |  [7] [8] [9]         |
 +---------------------------------------------+----------------------+ 600
```

The bottom bar takes roughly the last 120 px, which leaves 480 px for the panes.
The code pane keeps its current row height, so 16 rows fit with a small title
strip; the graphic pane keeps its current width and gains the tool strip.

## Where the removed content goes

| Was | Goes to |
| --- | --- |
| mode / title | bottom-left status, first line |
| settings / alarm / reset banner | bottom-left status, error colour, first available line |
| fps and draw timing | debug only (`NC_UI_DEBUG_TIMING`), no permanent space |
| preview legend | graphic pane caption area |
| footer key letters and labels | the 3x3 grid area (its labels), or the status line for the current context |
| operator message | bottom-left status, message line |
| tool description | strip at the bottom of the graphic pane |
| file name | removed from the code pane (see open points) |

## Key grid

The 3x3 area is the dedicated home for the current mode's entries. It is the
place the key model from `nc-ui-review.md` should land:

- entries have a kind (action / toggle / mode);
- toggles show their state, actions do not;
- the labels are the current context's meanings only (no reuse of `B`/`C`/`D`
  letters for sign, point and accept while editing - the dedicated keys exist
  for that).

The grid must stay a 3x3 even when a mode needs more entries: the extra entries
become the second level of the menu, not a wider strip.

## Open points to confirm

1. **Alarms when the header is gone.** The settings/reset and alarm guidance is
   currently the most prominent thing on the panel. In v2 it lands in the bottom
   left, which is quieter. Should a serious alarm also take the whole status area
   or overlay the code pane?
2. **File name.** Removing it from the code pane leaves no indication of the open
   file outside the file list. Keep a small dim line in the status area, or drop
   it entirely?
3. **Message line versus DRO.** Both live in the bottom left. Confirm the
   precedence: a rejected edit or an alarm should take the line, the DRO returns
   when it clears.

## Implementation notes

- All coordinates in `nc_visual.c` derive from named constants
  (`NC_FOOTER_Y`, `NC_SPLIT_X`, `NC_RIGHT_PANE_X`, ...). The change is best done
  by replacing those with the v2 region constants once, then adjusting the draw
  functions to them, rather than by editing each draw call.
- The header draw function is removed with its callers; the notice text it
  rendered becomes a status-area line with the same content.
- The frame dump test (`python tools/test_nc_ui.py`) is the cheap way to check
  the new layout before flashing: 16 rows, the 3x3 block, the tool strip, and
  the status area all visible in the dumped BMP.
