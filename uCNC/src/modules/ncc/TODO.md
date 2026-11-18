# ncc status and remaining work

The panel is built and in use on the machine (2026-09-28). What is left is
mostly a matter of moving things to the module that should own them - see
`README.md`'s "The scaffolding" for the list of what this panel keeps for
itself today.

- [x] **The framebuffer view moved out of the controller** (2026-11-09).
      `ncc_visual.c` now owns input, the document, screen selection and status;
      `ncc_view.c` owns layout, invalidation and drawing; the read-only
      boundary is `ncc_visual_internal.h`. The station check enforces the
      2,000-line source trigger so the split does not silently undo itself.

## Pictures on the pad's keys

- **Do not port the Sierra AGI engine itself.** The RP2350 Frank-Quest build
  demonstrates that the approach runs on this class of hardware, but its
  `PictureMgr` owns a virtual picture surface and AGI-specific brush/fill
  machinery. LeanCam already has the scanout framebuffer and drawing primitives
  in `lvds_renderer`; a future glyph or vector-object format should borrow the
  compact opcode-stream idea and render through that owner, not add a second
  picture surface or a Sierra compatibility layer.

- [ ] **An image for every key of the 3x3** (asked 2026-09-28: *"i will like in
      some future to have an ability to load glyphs/images for each presets
      thing on 3x3"*). The nine keys should be able to carry a glyph or a small
      picture of their own - the entry's own mark - so a key is read by its
      picture rather than by its words, and a set of entries can be recognised
      at a glance.

      That needs the boot splash's machinery **moved out of this module and into
      the renderer** (`lvds_renderer`), which already owns the palette, the
      drawing primitives and the scanout, so that this module only says *which*
      image goes *where*:

      - what moves: the run-length decode and the draw loop in `ncc_boot.c`, the
        colour-to-palette mapping (`lvds_hstx_rgb()` per colour), and the taking
        and handing back of the palette (`lvds_hstx_palette_reset()`);
      - the shape to aim for: an image type the renderer owns - its runs, its
        own colours, its size - with `lvds_image_draw(&image, x, y)`, so a caller
        with an image in hand has nothing else to know. `ncc_visual` then says
        `lvds_image_draw(&entry_glyph[i], key_x, key_y)` and nothing more;
      - the palette is the constraint, and the splash already taught it: the
        sixteen slots are shared with the panel's own colours, so the library
        has to own the bookkeeping (an image's colours take slots while it is
        up, and are handed back) rather than each caller inventing it;
      - where the images come from is this module's business (the card, the
        entries' own table, or the build), not the library's.

      The pilot is the splash itself: converting `ncc_boot.c` to the library is
      the first user, and it is already a passing check (`--boot2test`).

## Stock depth presentation modes

- [x] **OD pseudo-3D shading is the current machine behavior.** The live stock
      maps remaining OD material depth through four amber anchors and a static
      4x4 ordered dither. Untouched stock is darkest; cut columns redraw from
      their current surface toward the centre. The pattern and geometry are
      static/cached and must not become lighting or camera-dependent.

- [ ] **When ID shading is added, OD must become solid.** An internal diameter
      has to read as the actual cut-out of the part. Dithering both OD and ID at
      once makes the bore boundary ambiguous, so selecting ID depth replaces the
      OD depth texture with one solid OD stock colour while the ID surface uses
      the depth texture.

- [ ] **Keep three explicit stock presentation states** for later UI wiring:

      1. **Solid** - one stock colour, no depth dithering.
      2. **OD depth** - current behavior: OD uses depth shading, ID remains a
         geometric cut-out.
      3. **ID depth** - ID uses depth shading, OD is forced solid so the bore is
         read as a real opening.

      The key/mode used to select these states is not decided yet. Do not infer
      it from `#`; that remains the DIN/full versus stock-plus-path switch.

- [ ] **Dither budget for later ID work.** The hardware framebuffer is 4bpp and
      the HSTX palette has 16 real entries; extra surface levels are spatial,
      not extra palette colours. OD and ID can each have 16 logical levels while
      sharing the same four amber anchors, with different dither phase or
      orientation. Two separate four-anchor OD/ID families need eight stock
      entries; the current table leaves seven after the nine general UI colours,
      so one existing role would have to be reclaimed first.

## Keeping the box honest

## The TOOLS screen's editor does not show the word being typed

- [x] **Reported 2026-09-28: *"tool edit is broken it does not mark current
      editable item value"*** - fixed 2026-10-17, and it was the drawing, not
      the keys: `ncc_draw_row()` gated the field box on
      `g_mode == NCC_MODE_PROGRAM`, so TOOLS - which draws the same document
      through the preview pane - could never show it, while the pad's label
      (the legend) did say which word was picked. The gate now includes
      `NCC_MODE_TOOLS`. The host check still to add: walk the table in
      `--tools2test`, press `D`, and read the box off the pane.

## The tool table without the orientation

- [x] **The orientation and the insert picture are removed** (2026-10-17). The
      row keeps `T R F Q D E S X Z`; the `O` field, the TOOLS insert picture and
      the orientation rays are gone. The run glyph is the nose circle only -
      flanks, polygon and fill were removed after they read wrong on the glass
      and took the panel from 40 fps to 20. The operator checks the insert
      itself; `R` stays as the brush radius and the cycle compensation radius.

- [ ] **Bench:** read the TOOLS screen without the picture - the row values and
      the pad are the screen now; confirm that is enough for the operator and
      that the frame meter is back near 40.

- [ ] **G8 in the sender cycles** (unchanged): direct `G0`/`G1` rows honour
      `G8`, but `g7x_stream_add_line()` still halves contour X as a diameter.
      A `G8` program with a `G71`/`G72` range needs the sender-side change.

## Orientation, when it comes back (parked 2026-10-18)

- [ ] **An orientation word for direction, not for the insert picture.** The
      old `O` code is gone with the insert drawing. A future orientation has a
      different job: which **cutting direction** the tool is allowed to take and
      which direction it **cleans the stock** in. Two candidate sources, both
      parked until the meaning is settled:

      - a small **text type** on the tool row - for example `RL` or `IE` - that
        the operator reads and edits directly, separate from the numeric insert
        code; the row would stay `T R F Q D E S X Z` plus that one type word;
      - derive it from **G7x's own words** (`G41`/`G42`, the cycle type, the
        travel direction). That is possible, but messier: the property is
        spread across the program and G7x's state machine, while the operator
        reasonably expects a tool property to live in the tool table.

      Do not add the field or change the table yet. The current table remains
      `T R F Q D E S X Z`; an old `O` word is still ignored by the parser and
      skipped by the pad's word mapping. Decide the exact two directions and
      their names first.


## The offset path, drawn (asked and done 2026-09-28)

- [x] **A green line in EDIT showing the path the tool's centre will follow when
      compensation is on.** `ncc_offset_preview()` (`ncc_preview.c`) walks the
      sender a second time with the compensation forced to the tool table's `R`
      for the tool in play and draws every move in the panel's green over the
      programmed profile; a program carrying its own `G41`/`G42` overrides the
      direction as the walk meets it, and a table with no radius draws nothing
      rather than a shift that cannot happen. Left for later: a *dedicated*
      palette entry for the debug line (it borrows the run's green today), and
      drawing the offset of the *cycle's* passes rather than the sender's own
      moves once the preview asks G7x for the compensated region.

- [x] **The sender and pacer moved to `nc_sender`** (2026-11-09).
      `nc_sender` now reads generic `g7x_doc_t` lines, expands cycles, resolves
      `U`/`W`, filters controller lines, reports the source line for each block
      and paces their hand-over. It has no dependency on the panel, its document
      type or its tool table.
- [ ] The mark's fallback bookkeeping in `nc_sender_run.c` (the unit in play,
      and "a unit the run has passed does not take the mark back") is only what the
      pane shows before the first block is executing - the core's own
      `itp_get_rt_line_number()` answers it otherwise. Delete it once the
      fallback has been quiet for a while (item 2 of the same list).
