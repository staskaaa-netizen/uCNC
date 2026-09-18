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

Decided:

1. **Filename and alarm text live in the bottom-right pane**, in the same area
   as the 3x3 grid (above/around it, sharing that pane). The code pane no longer
   shows the file name.
2. **Weird alarms float.** An unexpected or serious alarm may be drawn as a
   centred overlay with the alarm text, dismissed with **Enter**. Ordinary
   settings/reset guidance stays in the bottom-right pane with the file name.
3. **DRO occupies one column, stacked vertically** in the bottom-left pane:
   one line per value (X, Z, F, S - and Y when the machine has it). The operator
   message keeps the same pane below the DRO, so a rejected edit or a message
   does not fight the numbers for horizontal space.

Still open:

- exact precedence between a floating alarm and a normal message (assumption:
  the overlay wins while present, the message returns after Enter dismisses it).
- which alarms qualify as "weird" (candidate: settings invalid, position
  untrusted, controller fault - the ones that stop the machine rather than
  report a rejected edit).

## Implementation notes

- All coordinates in `nc_visual.c` derive from named constants
  (`NC_FOOTER_Y`, `NC_SPLIT_X`, `NC_RIGHT_PANE_X`, ...). The change is best done
  by replacing those with the v2 region constants once, then adjusting the draw
  functions to them, rather than by editing each draw call.
- This is one commit, not a series of small patches: between the constant swap
  and the draw-function updates the panel does not lay out correctly. Do not
  flash or rebuild the panel half way through; use the frame dump to check the
  result first.
- The header draw function is removed with its callers; the notice text it
  rendered becomes a status-area line with the same content.
- The frame dump test (`python tools/test_nc_ui.py`) is the cheap way to check
  the new layout before flashing: 16 rows, the 3x3 block, the tool strip, and
  the status area all visible in the dumped BMP.

## Implementation order

1. New region constants for the v2 layout (pane bottom, bottom bar top, 3x3
   area, status column, tool strip) and the code-pane row count.
2. Header draw call and its callsites removed; notice text routed to the
   bottom-right pane.
3. Bottom bar: left status column (stacked DRO lines, then the message line),
   right pane (file name, alarm text, 3x3 grid).
4. Tool description strip at the bottom of the graphic pane.
5. Floating alarm overlay with Enter to dismiss, drawn last so it sits on top.
6. Frame dump check, then the machine build.
