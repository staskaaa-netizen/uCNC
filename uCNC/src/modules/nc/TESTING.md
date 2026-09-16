# NC Pre-Alpha Testing

Use this as a short hardware pass list while NC is still pre-alpha.

## Boot and Stability

- With invalid saved settings, every screen shows the settings/reset guidance
  without requiring a serial terminal; ordinary navigation must not hide it.
- After an intentional settings reset, the banner clears when the settings
  error clears. No UI action should reset settings automatically.
- Submit an invalid RUN/MDI parameter: check readable error, numeric code and
  correct source line; correct/retry and confirm the old error clears.
- A runtime alarm, door or untrusted-position lock updates the header even when
  the current screen is otherwise idle.

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

Automated software checks: `python tools/test_g7x.py all` with MinGW GCC on
PATH. The parser fixture intercepts G33; it does not validate spindle timing.
The following machine checks remain open:

- `G71/G72` collect contour until `G80`.
- Source contour lines do not execute directly during RUN.
- Generated rough and finish blocks execute through parser helper.
- Finish ends at first contour point plus clearance in the opposite first-vector direction.
- Bad contour/status paths return useful errors without locking the UI.
- Queue a command after G80 and verify the whole generated cycle precedes it.
- Inject a generated-motion failure and verify no later source line is consumed.
- Hold/resume and Stop during a cycle, during normal motion, and after the last
  source line has been read but motion remains queued. Check that Stop also
  works immediately after resume, before motion restarts.
- Reset during contour collection and threading; no stale cycle may resume.
- Check G76 with G7/G8, G20/G21 and nonzero work offsets using the documented
  native contract in `../g7x/README.md`.
- Check spindle index/phase repeatability across all G76 passes, physical pitch,
  lead-in/out clearance, spindle loss and G33 error handling before cutting.
- G76 preview is currently unsupported and must show an explicit error.

## Tools

- TOOLS mode edits only the active `.t` tool file.
- Programs link tools by `Tn`; adding/editing tools does not append rows to the active NC file.
- Tool glyphs for one-digit and three-digit orientations are centered and readable.
