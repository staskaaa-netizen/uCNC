# Project history: ESP32/LeanCam to RP2350/NC+G7x

The whole arc, not just the desktop tools. Sources are the repository itself:
`git log`, the module READMEs (`encoder.md`, `g33/README.md`,
`rp2350_pio_encoder/README.md`, `lvds_renderer/README.md`, `g7x/README.md`),
`docs/upstream-integration.md`, `docs/sd-card-history.md`, and the LeanCam
architecture notes. The tool-focused review is in
[history-leancam-to-nc.md](history-leancam-to-nc.md).

## Timeline

| Date | Step | Evidence |
| --- | --- | --- |
| 2026-04-30 | LeanCam on ESP32 with the RA8876 TFT: preview, live simulation, cycle defaults, sim corner labels | `Checkpoint RA8876 LeanCam preview`, `Pre-alpha LeanCam RA8876 snapshot` |
| 2026-05-01 | First desktop app: LeanCam Windows editor and sender | `Add LeanCam Windows editor and sender` |
| 2026-05-04..05-11 | ESP32 PCNT encoder work: index hunt diagnostics, virtual modulo index, PCNT counting modes for G33, RPM from position deltas | `Add PCNT encoder index hunt diagnostics`, `Add selectable PCNT encoder counting modes for G33 spindle synchronization`, `Final commit of parser_g33.c and esp32_pcnt_encoder.c` |
| 2026-05-14/15 | RP2350 bring-up starts on HDMI, not LVDS | `Checkpoint RP2350 LeanCam HDMI bringup`, `Checkpoint semi-working RP2350 LeanCam HDMI` |
| 2026-05-19/20 | LVDS renderer checkpoint, tuning, working-state backup | `Checkpoint RP2350 LVDS renderer`, `Backup working RP2350 LVDS state` |
| 2026-05-21 | LeanCam tool editor working on the LVDS stack | `Checkpoint working LeanCam LVDS tool editor` |
| 2026-05-27 | Virtual index generalized into the common encoder module; generated preview stabilized | `Add common encoder virtual index support`, `Stabilize LeanCam LVDS generated preview` |
| 2026-06-03 | LVDS stack cleanup and generalization | `Clean up RP2350 LeanCam LVDS stack` |
| 2026-06-08 | NC module born: thin checkpoint, RUN streams source lines | `Add thin NC module checkpoint`, `Make NC run stream source lines` |
| 2026-06-14 | G7x module: LVDS G7x backend integration, thread stream, Fanuc G76 semantics | `d2f393e3`, `374e55e3`, `336731ba` |
| 2026-09-15/16 | Upstream core/modules merge, ownership audit, SD initialisation trial with rollback | `docs/upstream-integration.md`, `docs/sd-card-history.md` |
| 2026-09-18 | P/Q ranges, Fanuc two-line headers, desktop sender and panel shell | this session's commits |
| 2026-09-27 | `ncc` panel (nc2's screens, entries in RAM, two shapes, `#` preview), 3x3 key geometry, boot picture, and the day's real fault: the RP2350 core's newlib-nano `snprintf` **drops float conversions** unless the formatter is linked, so the panel handed the machine `G0 X` where it meant `G0 X55.000` and the controller answered `error 2` - found only after the fault was made to name the block it refused (`ncc`, `tools/test_ncc.py`) and fixed in `rp2350.ini` (`-Wl,-u,_printf_float`) | `ncc/README.md`, `ncc/TESTING.md`, `rp2350.ini` |
| 2026-09-28 | The `ncc` pad's word rule, decided on the bench: an entry's rows are either **joins** - a leading space finishes the line the press began on, ` F`/` C0`/` R0` at its end and `N` at its front - or **lines of their own**, so the one table says both the block key (`[11] SETUP`, four rows) and the word keys, while a card row that starts with a space still continues the row above it at load time. `ncc_pad_write()` had never joined the *operator's* line (it wrote the word into a line of its own), and the host suites still pinned the pre-rearrangement table. The same bench pass found the rest: `Tn` was a join when a tool call is a line of its own, a press that wrote left the pad up with `A` as the only way out - nc's menu cancelled itself before every entry, so the port's pad now ends when a row lands - and a landed row picked its first word, so `B`/`C` edited the `G41` that had just been written instead of moving the cursor (nc left nothing picked for a row it wrote; the port now picks the first *value* of the row, and nothing at all when the row is only its command). The last of the three was the same symptom from the other side: the G-code resolve ran on *every* key the editor took, so the cursor travelling over a bare `G71` expanded it into its template and a `G41` was picked on the way past, while a lone `M` word - which the vocabulary does not hold - was left alone; the resolve now fires only when a code was just typed (the accept, or `D` stepping off a word something was typed into) | `ncc/README.md`, `ncc/TESTING.md`, `ncc/ncc.c`, `ncc/ncc.h`, `ncc/ncc_presets.c`, `ncc/ncc_visual.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/tests/ncc_host_test.c` |
| 2026-09-28 | The tool picture, corrected on the bench: the nose radius is the circle **tangent to both edges** the orientation names, carried as the arc between the two points where it touches them, and drawn as a line *over* the tool - never punched into it with the pane's own colour, which put background into standing material and read as a row of marks (bench: *"add radius not as some background dots but as a real radius so background is not present where it should not be"*). The tip is where the flanks would *cross*, so it lies outside that circle, and a circle drawn through it (the first attempt) or centred on it (the port, and nc) is not that corner; codes that name no corner draw no arc. With it, the tool view's crosshair became the **orientation keypad** - four lines through the tip, 0/45/90/135 degrees, the bench's snowflake, with each of the eight rays carrying its key's number, and the tool drawn with its tip on the middle key - and the TOOLS pad became a row editor: `1` writes a new row with its tool number picked and the keys after it pick the row's words, `2` the radius and `3` the orientation, replacing the nine fixed `ADD T<n>` templates (bench: *"it is wrong ... on cross hair darw keys where needed ... with all numbers on according corners"*, then *"lines shoud form a snowflake ... ie -45 0 45 90 etc degree"*, *"radius should be drawn only on tangents"*, *"remove this add t1 add t7 etc."*) | `ncc/ncc_draw.c`, `ncc/ncc_draw.h`, `ncc/ncc_presets.c`, `ncc/ncc_visual.c`, `ncc/ncc.c`, `ncc/ncc.h`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | Two more turns of that picture, both about the *scale*: the sliver **outside** the radius - between the tip and the two tangency points - is cleared to the background, so the corner is drawn as the rounded corner the insert has rather than a sharp corner with a radius line on it (*"part outer on radius should be cleared too"*), and the tip marker wears the drawing's own ink instead of the tool's, because a tool-coloured dot there was a lump of material in front of the nose (*"still this extra yellow is on its front"*). The tool is then drawn **at the scale of the drawing it is on**, with the radius setting it: the body is five nose radii across, so a run shows an `R10` tool as a big tool and an `R0.8` one as a small one. nc capped the radius at a third of a fixed-size body, which is what made a big radius read as a tiny tool (*"for R10 and r5 of tool i see tool really tiny one"*). The live tool's take-back box follows the size it was drawn at, so a changing radius leaves no trail | `ncc/ncc_draw.c`, `ncc/ncc_draw.h`, `ncc/ncc_preview.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And two clean-ups of that: the radius is the **cleared sliver itself**, with nothing stroked over it (the arc line and the clear were two pictures of one corner - *"now it is two fights in parallel"*), and the live tool is no longer held inside the pane **by its own size**, which with a size that follows the radius pinned a big tool away from the work and made it jump (*"all drawing seems fuzzy ... yellow one is jumping around"*). The tip is drawn where the machine is; only a tool off the drawing entirely is held on the border by its tip, and the box that takes it back is the part of its square that landed in the pane | `ncc/ncc_draw.c`, `ncc/ncc_preview.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And then the shape itself is parked: the tool is drawn as **one circle** of the row's own radius, at the drawing's scale, in the tool's colour, centred a radius along the material's direction so the point the program addresses is on it - no body, no arc, no clearing, the drawing's own background where a body used to be (*"again tool is jumping. skip it for now. only use circle in proper background color. with radius as per tool setting"*). The orientation rays and the TOOLS row editor stay; the tangent nose geometry (`ncc_tool_nose()`) and its checks stay too, because that is what the insert's picture has to be built on. The tool is held inside its pane by its own drawing's margins rather than by its size, and the box that takes it back is exactly that box, so nothing is left on the chrome below the pane | `ncc/ncc_draw.c`, `ncc/ncc_draw.h`, `ncc/ncc_preview.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And then the whole question was put right: the bench meant **the brush that clears the stock in the framebuffer**, not a marker - *"the shape used in framebuffer to draw remaining of stock by traveling along path of machine by drawing bg color other[ than] stock color"*. So the tool is **not drawn anywhere** (nc's glyph, the ring, the body shapes and one circle at the machine's position were each parked - *"do not draw any circle yet"*), and the live stock is taken off by a brush that is a **circle of the row's own `R`**, walked along the machine's path with its lowest point on the point the program addresses: the material under the circle goes, so a cut ends in the tool's own radius instead of a square bite (*"you still clear stock using some square. not circle"*). The insert's tangent geometry is still parked and still asked by `--tools2test`, for when the picture comes back | `ncc/ncc_draw.c`, `ncc/ncc_draw.h`, `ncc/ncc_preview.c`, `ncc/ncc_visual.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And then three more turns to finish the day: the brush's bound is the nose circle's **inner** arc - the one whose deepest point is the tip and which rises on either side - because clearing from the *outer* half left the cut's ends flaring deeper and still read as a square block (*"just brush is still square. not round"*), the material the tool's body displaces going with it. The **TOOLS screen draws the shape again** - the box or polygon the row's orientation code names, corner on the middle key, inside the orientation rays (*"show some glyph in tools. now we will tune brush and then give back tool shape again"*) - and the run's own drawing gets the **tip glyph** back, gently: the nose's circle and a mark on the tip, with the tip on the pane's mapping of the machine's position and the circle centred one radius outward, which is exactly the circle the brush clears with, kept inside the pane by its own radius and taken back with that same circle (*"gently reattach tool tip glyph. do you know where it should land?"*) | `ncc/ncc_draw.c`, `ncc/ncc_draw.h`, `ncc/ncc_preview.c`, `ncc/ncc_visual.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And the tip glyph stops being slid: it is drawn **where it lands** and the part that falls outside the drawing pane is simply not drawn (*"hide part of glyph if you cant draw it. but do not slide it on side"*). The renderer clips to the screen rather than to a pane, so the circle and the tip's mark are drawn by hand - two outline points a column, each asked against the pane - and the take-back is the same clipped circle, so nothing is left behind and nothing is held away from the work it is cutting | `ncc/ncc_preview.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And that glyph becomes the plain thing it should have been: the nose's **disc, filled with the insert's own colour** (*"make it fully of insert color"*), drawn full and *copied* row by row into the pane, so the pane does the clipping rather than a hand-drawn outline (*"why you need to hand draw it? we still have a frame buffer so just draw it here full and cope here required part"*). The row is the clip - which is what the renderer's one-target draw allows, since it clips to the screen and not to a pane - and the take-back copies the same disc back to the ground | `ncc/ncc_preview.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And a real bug behind that picture: the preview drew **every emitted move as a straight chord**, so a programmed corner radius (`R`) came out as a chamfer in both the profile and the green compensated-path line, even though G7x emits a true `G2`/`G3` with `I`/`K` for it (bench: *"gren line of ofssete path for rnd shows chamfer path"*). The preview now walks the arc, point by point, through the same axis mapping as everything else; the emitted data was never wrong, only its drawing. The day's scaffolding and size review is in `ncc/README.md`: the sender is 1,424 lines here and 3,851 across the three panels, and seven exported functions the panel no longer calls (~45 lines) are listed there as candidates to delete - one of them, `ncc_file_new_backspace()`, should be wired instead | `ncc/ncc_preview.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And three corrections to that arc, all found on the bench: the compensated arcs arrive with **`R`** rather than `I`/`K`, so the preview has to take the centre from the radius and let the commanded turn pick which side; **`I` and `R` are radius values while the program's `X` is a diameter** (Fanuc's own rule - the cycle emits `I5.000` for a five-millimetre corner), so the arc's geometry belongs in the radius plane and is doubled back when mapped; and the turn's direction maps one way round, not the other. Getting those wrong drew the corner as a wide loop over the profile or as the chord it used to be (*"seem still straigh line in panel"*). With it, a marking nc had and the port had dropped comes back: the contour point of the **row the cursor is on is filled** and that row's emitted motion is drawn three pixels wide (*"we have missied in some point marking active select line in edit to line on preview screen"*) | `ncc/ncc_preview.c`, `ncc/README.md`, `ncc/TESTING.md` |
| 2026-09-28 | And where the debug line's radius comes from: the offset walk asks the tool table for the tool in play, and the table was read only on the screens that *show* it - TOOLS, and RUN for the tip that rides the cut - so EDIT drew the three shifts at the built-in row's `R0.8` whatever the operator had measured, and a card whose tool really said `R0` still got a green line (*"only line is seen here? or its takes no tool without running?"*). The program screen reads the table now, at boot and on every walk to EDIT, so walking TOOLS and back brings the edit with it; `--offset2test` pins it (R0 draws nothing, R3 draws the line off the profile). The other half of the answer is geometry, not a bug: the tool's centre is one **radius** out, which is `2R` in the X a lathe programs - the generator's own test says it in one line, `r=1` on a programmed `X50` cutting `X52` | `ncc/ncc_visual.c`, `ncc/ncc_preview.c`, `tools/ncc_ui_win/host_tests.c`, `ncc/README.md`, `ncc/TESTING.md`, `tools/test_ncc.py` |
| 2026-09-28 | And the run's own file: leaving RUN for EDIT while the machine was cutting **re-read the card under the run** - the mode key gave the program back the way TOOLS needs it, but coming from RUN the buffer *is* the program, so the reload swapped the program the machine was half way through (and a FAT read inside the frame loop is exactly the delay the tool table is loaded once to avoid). The document is given back by TOOLS only now. With it comes the safety the bench asked for (*"if i go from run while it is runnig to edit i am stuck here + its need a safety not to edit [a] running file"*): while the pacer is feeding it, the program is the run's - EDIT keeps the keys that only look (`B`/`C`, `#`, `A`/`0`) and answers every key that would write the text, or swap the file, with *"Program running - the file is the run's"* (nc's own sentence for the idea was "RUN owns the line"). `--runlock2test` gives the card a *different* program behind the panel's back after the run is armed, so a switch that re-reads it is visible in the machine's own end position | `ncc/ncc_visual.c`, `ncc/README.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c`, `tools/ncc_ui_win/main.c`, `tools/ncc_ui_win/README.md`, `tools/test_ncc.py` |
| 2026-09-28 | And one attempt at the live cut's offset, **taken back**. On a compensated run the emitted path - and so the machine - is the tool's *centre* path, one nose radius outside the surface the nose cuts, while the preview's three lines read as the *surface*, so the drawn cut sat a radius out in Z and a **diameter** out in X: *"on run it sime lik it is not being correctly compenstated on z axis ... on g40 path is exactly as points in graphic and code. but on run they are offsetted by diameter"*. Two shapes were tried - the machine's point walked back along the travel's normal, then (with a check that reads the material's edge off the glass, `--livecomp2test`) the same point with the brush made the nose's own disc and the tip glyph that disc, which is the *fixed* offset the panel is already compiling with. The disc version was right on the glass - the same bar's edge lands on one row whether the machine is on the surface, a radius out, or reaching the same place from the other side of Z - but the bench still saw the tip **jump** (*"and it is jumping"*, *"make back as it was before red alert and we will think of more proper test"*), so the whole change came out again: the live stock and the tip are the machine's point, the tip's disc hanging a radius outward of it, as before, and what makes the tip move the way it does is still open - it needs a test that records the machine's own positions across a run, which is the next thing to build | `ncc/ncc_preview.c`, `ncc/ncc_preview.h`, `ncc/ncc_run.c`, `ncc/ncc_run.h`, `ncc/ncc_visual.c`, `ncc/README.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c`, `tools/ncc_ui_win/main.c`, `tools/ncc_ui_win/README.md`, `tools/test_ncc.py` |
| 2026-09-28 | And the DIN layer is drawn on the **run** screen too - the rulers, the centrelines and the stock's dimension callouts, while the machine cuts and not only when it is parked (bench: *"enable din in run preview i think i see the problem"*) - and with them the **rows' own points and dimensions**: each row's contour point, its X/Z callout and its `C`/`R` label (bench: *"and dimensions too ?"*). They are redrawn every frame, so a live frame's band repaint cannot leave a piece of a callout behind. The cut is only readable against those numbers. What the run screen still leaves for the parked frame is the program's *path* - the emitted moves and the three offset lines - because the glass is already showing the cut itself | `ncc/ncc_preview.c`, `ncc/README.md` |
| 2026-10-04 | And with the numbers on the glass the drawing's own offset could finally be measured: with a compensation on, the brush's bound was drawn through the **machine's** point - which is the tool's *centre* - so a `G41` OD left its material edge on the machine's X, **26.03 where the program's row says 20** (two nose radii of diameter), while the faces came out right because there the disc's trailing side is what touches. The bench's words were *"only drawing seems to be wrong. ptah preview lines are in place as then need to be. so check position of actual clening brush vs sendded coordinates and real mashien coordinates"*, and the measurement said exactly that: the brush is now the nose's own disc **around** the machine's point when a shift is on (`ncc_live_clear()`'s `deep`, with the tip glyph drawn as the same disc, `ncc_tip_rows()`), so the cut's edge lands on the row the program wrote - an OD's surface, a shoulder's face, an arc's flank - and a `G40` run is unchanged. `--brush2test` is the check: `G40` and `G41` paths, the edge read off the glass both times. The disk here is *fixed* - the radius the panel is compiling with - not a direction read from the machine's travel, which is the shape that jumped | `ncc/ncc_preview.c`, `ncc/ncc_preview.h`, `ncc/ncc_visual.c`, `ncc/README.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c`, `tools/ncc_ui_win/main.c`, `tools/ncc_ui_win/README.md`, `tools/test_ncc.py` |
| 2026-10-04 | And the panel's **frame close**, open in the LVDS notes since the marker runs, was run down to ground by probes the bench set up - *"so two experiments - one add 4 fake lines. so we willl see is it our software or bogus receiver"*. Four extra **blanking** lines (the ring's guard descriptors, reached sooner: the frame's total grows, the active area does not) changed **nothing**; four extra **active** lines put the picture's last lines **right** - so neither the frame's total nor the clock. The blanking line before the active area was the next suspect (`lvds_hstx_line_addr_for_v()` handed it an active-encoded, *stale* buffer) and its panel check came back **negative** too, so it went back as it was. What settled it was the marker probe the bench shaped itself (*"make it from bottom - white - red green blue black. top should be left clear"*): five **known** colours, one per line, on the frame's last five scanlines, with a break every 32 rows. The reading was *"and red is twice"* - our own known content, one line of it **shown twice** - so the close of the active area **repeats a line**, no matter what is on it: not the content, not the blanking, not the clock, but the panel/link at the end of the active area. The fix is to give that end its own room: the frame carries `LVDS_PANEL_ACTIVE_HEIGHT 604` (the *frame*'s number, its own define) against a glass of 600, the view - and the layout, and the station - stay the glass's own 600x800 mapped onto the *first* 600 active lines, and the last four active lines are the frame's blank tail, so the repeated line lands there instead of on the picture. (The blue the marker list needed came from the palette's one free slot: `yellow_pale` was a second copy of `yellow` no element used.) The bench read the spare-line build back clean - *"yes now it is ok"* - the marker probe came out of the tree again, and the spare tail is load-bearing. How much of it, the bench measured by walking the active height down (*"now 603 or 602 land extra black line. 603 is fine"*): at 602 an extra black line shows, 603 and 604 are clean, 600 puts the repeat back on the picture - so the close needs at least three tail lines and the frame keeps four. This is containment, not a root cause: the panel or link repeats or eats a line at the close of the active area, the cause is still open, and the tail is what absorbs it | `uCNC/cnc_hal_overrides.h`, `uCNC/src/modules/lvds_renderer/lvds_hstx.h`, `uCNC/src/modules/lvds_renderer/lvds_hstx.c`, `uCNC/src/modules/lvds_renderer/lvds_palette.c`, `uCNC/src/modules/lvds_renderer/lvds_palette.h`, `uCNC/src/modules/lvds_renderer/README.md` |
| 2026-10-04 | And the nose radius a cycle cuts with: it belongs to the **contour the cycle builds**, not to the `G41`/`G42` word that asked for it. The bench's own program writes the word *above* the tool call (`M3 S450` / `G42` / `T2` / `G0` / `G71`), so reading the radius at the word - a document scan, which fell back to the **table's first row** when the program named no tool above it - cut the `G71` a millimetre shallow while the `G70` came out right (bench: *"still takes first tool radius for g71"*, *"for x20 in diameter mode its wants an x13 for r3. but now it is only x12 ... for g70 it is ok for g71 it is bad"*). The word names the **side**; the radius is read from the table by the number of the tool the sender is cutting with (the run's own `T` record, `ncc_emit.c`'s `stream->tool`) and resolved at the cycle, where the tool in the spindle is the one that cuts. `ncc_tools_number()`'s tool-1 fallback and `ncc_tools_active()`'s default-to-`T1` are gone for the same reason: a number the table does not carry has no nose radius and the run **refuses** (at the word when the tool is already known, at the cycle when it is not), rather than cutting with a row the operator never wrote. `--comp2test` recreates it (`T1 R2` / `T2 R3`, `G42` above `T2`, both cycles' finish contours on `X14`) and `--offset2test`'s program now names its tool, since a program that names none draws no shift line | `ncc/ncc_emit.c`, `ncc/ncc_tools.c`, `ncc/README.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c`, `tools/ncc_ui_win/README.md`, `tools/test_ncc.py`, `docs/lathe-cutter-comp.md` |
| 2026-10-04 | And the tool and the compensation a screen **wears** became the machine's own line - read by **G7x, and only by it**. The sender is a block ahead of the machine by design (it has to be, to expand a cycle), so the strip read `G42` while the `G41` part of the program was still being cut, and a run **started inside** a compensated portion (`2 FROM` on a later cycle) armed from a global that `ncc_run_start()` had just cleared - it cut that cycle with `G40` (`X20` where the shift is `X16`) and said `T-1 G40` (bench: *"now g42 still appears while g41 portion was not finished. so make it very state machine in g7x for tool and for offset"*, then *"it should work directly from g7x and only from it. remember g7x is on its own too"*). Both words are the dialect's, so the state machine lives in `g7x_blocks.c` - `g7x_state_at()` (the last `Tn` and the last `G40`/`G41`/`G42` at or above a line) and `g7x_line_tool()` (the tool word, read the way `g7x_line_number()` reads `N`) - over rows a caller hands in as `g7x_doc_t`, with no screen, no tool table and no NC source linked; `ncc` only says *where* the machine is (`ncc_run_executing_line()`) and caches the answer for that line. Nothing is carried between blocks, so a re-arm, a step and a finished run (which keeps its last answer, and the parked picture with it) agree; the panel's own reads of the program's `T` (`ncc_tools_line_tool_number()`, `ncc_tools_active()`'s scan) are gone, and the nose radius is resolved **at the contour every time**, never at the word, because the program may change tools between the word and the cut | `g7x/g7x_blocks.c`, `g7x/g7x_blocks.h`, `g7x/g7x_contour.c`, `g7x/g7x_contour.h`, `g7x/tests/g7x_host_test.c`, `g7x/README.md`, `g7x/TESTING.md`, `ncc/ncc_run.c`, `ncc/ncc_emit.c`, `ncc/ncc_tools.c`, `ncc/ncc_tools.h`, `ncc/README.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c`, `tools/ncc_ui_win/README.md`, `tools/test_ncc.py` |
| 2026-10-04 | And the `G40` cut came back: the pane was taking the material off as if `G41` while the program cut plain (bench: *"g40 stock removing is overcompensated on x axis. log show 20 but in fact is same as g41"* - the log named the coordinates the machine was given, the picture was one radius deeper). The cause was the state machine counting a **compensation word written among a cycle's contour rows**: those rows are the profile the generator reads, so the cut never shifts for one, and `g7x_state_at()` now leaves them out of the words (`g7x_doc_line_is_any_contour()`, a rule of the blocks it already owns) - a tool written there is still the tool. The reproduction is in `state words` (g7x, standalone) and in `--state2test` (a word among the contour rows changes neither the cut nor the words after the block, and a plain program's cut leaves the material's edge **on the row**, read off the glass at `X20.25` for `X20`) | `g7x/g7x_blocks.c`, `g7x/tests/g7x_host_test.c`, `g7x/README.md`, `g7x/TESTING.md`, `ncc/TESTING.md`, `tools/ncc_ui_win/host_tests.c` |

## Phase 1: ESP32, RA8876 and conversational LeanCam

The machine was an ESP32 driving an RA8876 parallel TFT through the
`ra8876_display` module and its ER_TFTM101 driver ports, with `ra8876_test`
sketches kept for direct register work and `cam_keyboard` for the operator keys.
The application was LeanCam: a conversational lathe programmer with operations,
tool catalog, schema, expression evaluation, preview and DXF import, generating
G-code and showing it back on the same screen.

Two things from this era still matter:

- the display and the keyboard were modules, so the application never talked to
  the panel directly. That is the seam the later LVDS port and the 2026-09
  desktop panel both reused;
- the palette and the preview layout were designed here. LeanCamWin's README
  still describes using the same RA8876 RGB565-derived palette as the ESP32
  renderer, and the LVDS palette module kept the RA-style element names.

The cost that shows up later: cycle geometry grew inside the LeanCam generator.
G71/G72 contour handling and the older G76 stepping lived in `leancam_gcode.c`,
which became the largest C file in the tree. Phase 5 had to undo that.

## Phase 2: spindle synchronisation, PCNT, PIO, G33 and G76

This is the most hardware-entangled story in the project, and its READMEs are
the record.

On ESP32 the spindle encoder was the PCNT hardware unit
(`esp32_pcnt_encoder`): A/B counting, a configurable modulo boundary that emits
the encoder's normal index hook ("virtual index"), and RPM derived from position
deltas. The diagnostics show three competing index sources being hunted on the
bench - a physical GPIO ISR, a software poll, and the virtual modulo - before
the virtual modulo was made the active hook. In the same window: selectable PCNT
counting modes specifically so G33 could synchronise, and RPM sampling moved out
of G33 into the encoder module.

`modules/g33` is the parser module that makes a move follow the spindle. Its
contract, from its README: an encoder must be configured (`G33_ENCODER`), either
a physical index pin or a virtual index provides the update,
`ENABLE_RT_SYNC_MOTIONS` plus the main-loop and parser module options must be on,
and the index ISR can fall back to fixed point
(`G33_REPLACE_FP_OPERATION_IN_ISR`) if float in the ISR misbehaves on a target.

On RP2350 the encoder became a PIO state machine (`rp2350_pio_encoder`,
`ENC_TYPE_CUSTOM`). Its README documents the original failure mode, and it is
the best bug story in the repository:

1. the symptom looked like "custom encoder not initialised", because the generic
   module prints `[EC:... RPM:...]` regardless of the backend. The RP2350
   virtual-index line existed but was never printed;
2. the virtual-index task jumped straight to the newest crossed slot, so at
   about 200 RPM with ten virtual indexes per revolution several slots could
   pass between calls. G33 saw `LAST:1600` or `LAST:2000` instead of `LAST:400`;
3. G33 could start synchronised motion while the interpolator was still empty:
   `G33 start ... empty=1`, then `st=2` forever, stuck in `SYNC_STARTING`.

The fixes: print the debug line, emit bounded catch-up indexes instead of
collapsing slots, seed the G33 feed from encoder RPM or the raw hardware counter
rather than virtual-index timing, and defer `itp_start(false)` until the
interpolator is not empty. The recommended RP2350 setup is
`G33_FEEDBACK_LOOP_USE_HW_COUNTER`: the PIO count is the position truth and
virtual indexes are only synchronisation triggers.

G76 is built on top of this. Thread expansion lives in the G7x module (native
single-line G76: radial first cut, decreasing cuts with a Q/4 minimum, `R`
finish allowance, `I` taper, `L` spring passes) and emits ordinary `G0` and
`G33` blocks. The honest status is written in three places: the virtual tests
intercept G33, so they verify targets, ordering, pitch and error propagation but
not spindle phase, physical pitch or spindle-loss handling.

The architectural lesson was applied deliberately on 2026-05-27: what worked on
ESP32 first (virtual index) was generalized into the common upstream encoder
module instead of staying board-specific, and the RP2350 backend then plugged
into the same hook. Board-specific hardware stays in the backend; the contract
stays common.

## Phase 3: ESP32 to RP2350, the HDMI detour, then LVDS

The RP2350 move started as HDMI bring-up (checkpoints on 2026-05-14/15,
"semi-working"), then switched to the LVDS path that shipped: a Sharp
LQ121S1LG44 800x600 single-channel panel driven by HSTX through a PicoLVDS-style
7x256 scanline LUT, a 4bpp indexed SRAM scanout framebuffer, Core1 line
preparation, and a DMA descriptor ring feeding the HSTX FIFO. The renderer
README keeps the panel's real quirk that it locks below its nominal 40 MHz pixel
clock, with working clock test points.

This phase also produced the project's best-documented "do not do this again"
list (`lvds_renderer/README.md`, `leanCam/docs/architecture.md`):

- the "display froze while uCNC and motion kept running" class was a stalled
  HSTX DMA/control path, not a core crash. The descriptor-ring backend replaced
  the old ping/pong handoff, and the release recovery code went with it;
- PSRAM scanout was slow and lost sync; DMA present from PSRAM into the SRAM
  scanout framebuffer measured about 7.3 ms for 240,000 bytes and still produced
  garbage, even with HSTX DMA priority and paced present DMA; chunked present
  and direct scanout from the draw path are gone. `lvds_hstx_present()` stays on
  the CPU copy path;
- `LVDS_HSTX_TORTURE_TEST` (program-run churn, snapshot churn, present loops,
  line-repeat injection, stack and canary diagnostics) is history, not release
  configuration;
- the memory model is stated plainly: realtime memory stays visible and boring,
  and a stack overflow, memory overlap or descriptor ownership bug is an
  architecture fault, not something to mask with fallbacks.

The renderer stayed a backend behind a small draw API, which is what made the
2026-09 desktop panel possible: the same `nc_visual.c` runs on Windows against a
GDI implementation of that API, using the firmware fonts.

## Phase 4: LeanCam to the NC module

LeanCam programs operations conversationally; NC edits G-code text. They were not
merged, they were layered: 2026-06-08 added the NC module ("thin checkpoint",
then "RUN streams source lines"), giving the machine a G-code-centric screen,
editor, file manager and state store that does not depend on the conversational
layer.

What NC owns today: the document and editor (`nc.c`), files (`nc_files.c`),
remembered state and paths (`nc_state.c`), menus and mode switching
(`nc_menu.c`), the preview emitter (`nc_emit.c`), the RUN glue (`nc_run.c`), the
screens (`nc_visual.c`), plus sim, presets, tools and vocab. The SD-card history
belongs to this layer: one owner at a time, short filenames because long ones
did not work, no detection claims without a detect pin, and a baseline to roll
back to.

The split mattered because LeanCam had become the place where everything lived.
Measured today: LeanCam is 10,879 lines across 21 C files, with
`leancam_bridge.c` (2809 lines) and `leancam_gcode.c` (2783 lines) carrying UI
state, IO and generation together; NC is 6,638 lines across 16 files with a
single job per file.

## Phase 5: G71 leaves LeanCam, the G7x module

The conversational generator had accumulated the real lathe domain logic:
contour storage, monotonic validation, corner radius and chamfer expansion,
roughing passes, finishing, and threading. That is neither conversational logic
nor screen logic, so it was extracted into its own module with an explicit
ownership statement in `g7x/README.md`:

- G7x owns contour storage and validation, cycle semantics, allowances,
  approach and retract, pass generation, retained contour and depth override;
- the core parser routes commands and executes generated blocks;
- NC owns files, editor, the program-source adapter, preview and operator
  controls.

The extraction was then hardened over the following weeks: the shared stepped
generator for execution and preview, generated blocks submitted through the
parser with ordering and error cleanup, the thread stream for G76, Fanuc G76
semantics, both-axis approach, finish and return clearance, one-line P/Q
numbered ranges, Fanuc two-line headers, and a standalone parser test target
that links no NC source. The module can be built and tested with the UI absent,
which the LeanCam generator never could.

Two rules came out of this phase and are now written into the module docs: NC
supplies document access and UI, never a second cycle generator; and a numbered
range must fail loudly (missing, evicted or ambiguous) instead of guessing a
profile.

## Phase 6: desktop tools on top of the modules

The May desktop app (LeanCamWin) predates the split and re-implemented what it
needed. The September tools are the opposite: `tools/nc_sender` compiles the NC
and G7x sources and adds expansion plus a Grbl protocol client;
`tools/nc_ui_win` compiles the real screen against a host LVDS backend. Both are
about 1.8k lines of glue for functionality that already existed.
[history-leancam-to-nc.md](history-leancam-to-nc.md) has the detailed comparison
and the do/don't list.

## Consolidated do / don't

Do:

1. Put hardware behind a small module API: display draw calls, encoder
   `ENC_TYPE_CUSTOM`, `grbl_port`. Every port - RA8876 to LVDS, ESP32 to RP2350,
   machine to desktop - reused that seam.
2. Generalize a board trick into the common layer once it works twice (virtual
   index moved from the ESP32 PCNT module into the common encoder).
3. Instrument the path you cannot see, then retire the instrument (virtual-index
   debug line, index hunt diagnostics, torture harness), keeping what it
   measured in the README.
4. Pin a working hardware state with a rollback baseline before touching
   bring-up (LVDS backup, SD checkpoint plus baseline UF2).
5. Give timing-sensitive starts explicit state and test them (the G33
   `SYNC_STARTING` race).
6. Keep the ownership boundary in a README next to the code, and give each
   module a target that runs without the rest of the system.
7. Apply the size trigger: at roughly 10k lines in a "simple" module, or about
   2k in one file, extract the domain and rebuild the consumers.

Don't:

1. Don't diagnose a subsystem from an aggregate status line printed by another
   layer (the `[EC/RPM]` red herring).
2. Don't collapse or skip intermediate timing events; emit bounded catch-up.
3. Don't mask realtime faults with fallback paths.
4. Don't keep experimental display or memory paths in release code.
5. Don't claim spindle or threading validation from tests that intercept G33.
6. Don't re-enable a disabled bring-up workaround as a feature without the bench
   checklist.
7. Don't grow a generator, its UI and its IO in one file: that is the mistake
   Phases 4 and 5 had to undo.

## Where things stand

- Upstream-owned: core, kinematics, tools and the common encoder with virtual
  index support, merged with the module baseline recorded in
  `docs/upstream-integration.md`.
- Local modules: NC, G7x, LeanCam, the display backends, `cam_keyboard`, and the
  board-specific encoder backends.
- Software-verified: cycle generation and expansion, preview and emitter
  agreement, Grbl protocol behaviour, panel layout rendering.
- Bench-only: spindle phase and pitch through G33/G76, feed hold, resume and
  Stop during queued motion, SD mount on a cold start, panel readability.

## Verification

```powershell
python tools\test_g7x.py all        # generator, NC emitter, parser, standalone
python tools\test_nc_sender.py      # expansion + Grbl protocol, fake controller
python tools\test_nc_ui.py          # panel shell + headless layout frame
pio run -e RP2350-LEANCAM-LVDS      # machine firmware
pio run -e RP2350-G7X-MODULE        # NC-free G7x build target
```

## Open threads

- G33/G76 physical validation: index stability, pitch, spindle loss.
- LeanCam still carries its own G71/G72/G76 stepping; wiring it to G7x is the
  last place the "one owner" rule is not applied.
- NC RUN on the desktop drives the virtual machine; a Grbl transport for real
  hardware is the next integration step.
- The panel key layout question is settled: the desktop shell carries the
  machine's own key row (F1-F5 modes plus the 4x4 keypad `cam_keyboard.c`
  decodes) and asks the active screen what each key means, so the bench cannot
  disagree with the keyboard; the panel's on-screen 3x3 stays the firmware's.
