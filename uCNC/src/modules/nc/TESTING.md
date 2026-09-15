# NC Pre-Alpha Testing

Use this as a short hardware pass list while NC is still pre-alpha.

## Boot and Stability

- Reflash, boot, and verify activity/status keeps updating.
- Reboot with last EDIT/SIM/RUN/TOOLS files stored and verify paths reopen.
- Switch modes with `A` through MANUAL, EDIT, SIM, MDI, RUN, TOOLS.

## Files and Text

- File manager lists only valid files/folders.
- Open NC file in EDIT, SIM, RUN independently.
- Long lines wrap/read cleanly.
- `B`/`C` line movement stays sticky up/down in RUN and TOOLS.

## Preview

- `G970 X/U/Z/W` stock/setup gives expected stock size and origin.
- Chuck/stock holder is visible and does not cover the working contour.
- Contour labels `C1`, `C2`, ... stay readable and follow corners.
- Rapid lines are dashed and visible; feed/finish lines are distinct.
- Future: zoom/pan has a visible cursor/anchor.

## G7x Runtime

- `G71/G72` collect contour until `G80`.
- Source contour lines do not execute directly during RUN.
- Generated rough and finish blocks execute through parser helper.
- Finish ends at first contour point plus clearance in the opposite first-vector direction.
- Bad contour/status paths return useful errors without locking the UI.

## Tools

- TOOLS mode edits only the active `.t` tool file.
- Programs link tools by `Tn`; adding/editing tools does not append rows to the active NC file.
- Tool glyphs for one-digit and three-digit orientations are centered and readable.
