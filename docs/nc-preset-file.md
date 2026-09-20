# NC preset file

`nc_presets.c` owns `/D/presets.txt` and keeps compiled fallback presets when
the file is absent or malformed. The file supplies insert text only; G-code
meaning and validation remain with `nc_vocab`, the parser and G7x.

## When the file is resolved

The SD card is mounted from the main loop, not before module init, so the file
cannot be touched during `nc_visual_init()`. Resolving it is therefore lazy:

- `nc_presets_init()` (boot) loads the compiled presets and makes the first
  attempt.
- `nc_presets_sync()` is retried from the NC input path until the file is
  settled. It is a single flag check once that has happened.
- When the file is there, it is read once and its sections replace the compiled
  ones.
- When it is not there, the compiled presets are written to the card so there is
  a real file to edit. A failed write means the drive cannot answer yet (no
  card, or a card inserted later): the compiled presets stay in use and the
  next call tries again.
- A file that parses to no usable section (for example a damaged or hand-edited
  one) falls back to the compiled presets and is **left untouched**, so the
  operator's text can still be repaired. Only a missing file is created.
- A write that fails half way is removed again rather than left behind looking
  like the operator's own file.

No scanout/realtime path reads or writes it. The input path is the main-loop
task, the same one the file manager already uses.

## Format

```text
[41]
name=OD ROUGH
line=G71 U0 R0 X0 Z0 F0 P0 Q0 N0

[51]
name=THREAD OD
line=G76 X0 Z0 P0 Q0 F0 I0 L0 R0
```

- Section IDs are stable menu keys. Current defaults:
  - `10` setup
  - `21` line, `22` arc
  - `41` OD, `42` ID, `43` face
  - `51` thread OD, `52` thread ID, `53` tap
  - `61` drill, `62` peck, `63` dwell
  - `80` G80 end
- `name` is the section's short label. It is **required** - a section without one
  is skipped, and the compiled entry it would replace stays - but the floating
  3x3 helper still shows its own fixed slot labels. Showing the file's `name=`
  there is the open item in `uCNC/src/modules/nc/TODO.md`.
- Each `line=` is one inserted editor line, in order.
- Maximum 24 sections, 8 lines per section, and `NC_MAX_LINE_LEN` per line.
- Invalid sections are skipped. If no valid sections exist, the compiled
  fallback is used.

Editing the file on the machine, and semantic validation of what it contains,
are later work.

The file is reachable from the panel: the file list carries text files beside
the programs (`0` opens the list on EDIT, TOOLS and RUN), so `/D/presets.txt`
can be opened and saved as text. It is not a program: `nc_path_supported()` is
what decides that, so the preview does not parse it and RUN refuses it.

## Checks

`python tools\test_nc_ui.py` builds the NC screen for the desktop and then runs
the same binary with `--presettest`, which drives the firmware `fs_*` API in a
scratch root and asserts the contract above: a missing file is created, an
edited file is used, an unparsable file falls back and is left alone, every
`line=` of a section is inserted in order (through the same call the panel's OD
entry makes), and a section without a `name=` is skipped. Boot with a card,
without a card, and with a card inserted after boot remain bench items.
