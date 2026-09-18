# NC editor: adopt the Heidenhain TNC 415 field flow

This is a spec for the next NC editor pass, not a description of what exists.
It came out of using the Windows panel shell against the current editor.

## Why

Today a cycle is inserted fully prefilled, for example
`G71 U2 R1 X0.5 Z0.5 F120`, and the operator edits individual words afterwards.
Two problems follow:

- the prefilled line claims values the operator may not have chosen, and every
  word has to be revisited anyway;
- word editing has no per-letter rules. `nc_change_selected_word_value()` in
  `nc.c` formats any float into whatever word is selected, so the numeric part
  of `G72` can be rewritten to any value (for example `G99`) or made negative.
  Nothing rejects an unsupported command; the line is simply accepted.

The TNC 415 (and basically every control in that family) does it differently:
you start a function, the control asks for the **first required field only**,
shows its letter and a description on the top line, you type a value and press
Enter, and it moves to the next field. Pressing Enter with nothing typed leaves
that field empty and moves on. The function stays modal until you finish, so you
never type a field letter - only numbers.

## Target flow

1. Operator picks a function (from the 3x3/menu or a preset): `G71`, `G72`,
   `G76`, a plain move, a setup row.
2. The editor opens a **new empty line** for it and prompts the first field:
   field letter in the value cell, description on the top line
   (`Depth of cut U`, `Retract R`, `First block P`, ...).
3. Typing digits fills the cell. Digits, `.` and sign are the only inputs; the
   field letter is not typed.
4. Enter accepts this field and advances to the next one; Enter on an empty
   cell leaves the field empty and advances. Backspace edits; End commits the
   whole line; Esc/MODE aborts the function.
5. The function remains modal: pressing Enter on the last field commits the line
   and returns to normal line editing with the cursor on the new line.

## Word rules (validation, per letter)

### Applied when, not as you type

Today the draft is written into the program on every key stroke
(`nc_text_edit_apply` from the digit, backspace and sign paths), so a partial
value becomes the value. Two symptoms follow, and the rule for the TNC flow is
the opposite of both:

- deleting the last character used to leave `0` in the field (the empty draft
  was mapped to "0"). Empty now writes nothing, so the field keeps its value;
- leaving the field (Up/Down, mode change, another footer action) must keep the
  old value, not the partly typed one. That needs the draft to be applied only
  on accept: `nc_text_edit_handle_key` should stop applying per keystroke and
  apply once when Enter/End commits the field. The draft is already rendered in
  the value cell while active, so the operator does not need the program text to
  change under the cursor.

Validation belongs where every caller passes through - the selected-word edit
path - and it must be letter aware:

- `G`: positive integer, no sign, no decimals, and restricted to the codes the
  dialect actually supports. In particular the cycle family may only be
  switched inside its own set (`G71` <-> `G72`, `G76`, `G80`), and other
  families likewise - never an arbitrary number. This is the bug reported from
  the field: the `72` in `G72` was editable to anything numeric.
- Words that cannot be negative reject a sign; words that are always
  non-negative reject a negative value with a readable status message rather
  than writing it.
- `.` is only accepted where a fractional value is meaningful.
- Unknown letters are rejected instead of written.

Where the rules live: which words a cycle accepts, their order and their units
are G7x dialect knowledge (`g7x/README.md` documents the native contract). NC
should ask G7x for the field list of a cycle and own the input loop and the
messages. That keeps one owner per domain (see `../AGENTS.md`).

## Keys

- Up/Down: previous/next **field of the same letter** (`NC_VISUAL_KEY_FIELD_PREV`
  / `_NEXT`, implemented for both the desktop arrows and the RP2350 keypad): on
  an `X` you land on the next `X`, and the footer names the field and line.
  They never touch the value - an active draft is discarded, not applied. When
  no field of that letter remains, the key falls back to line stepping, and in
  the file list, menus or tool table it keeps the existing step behaviour.
- Left/Right: previous/next **word** on the line (already implemented as
  `NC_VISUAL_KEY_WORD_PREV`/`_WORD_NEXT`).
- While a field is being entered, Up/Down adjusts the value and flips its sign;
  the numeric pad's `-` key is then redundant and should not be needed.
- End: commit the field/line. Del: clear the current field. Both keys already
  exist on the pad and currently do nothing useful.
- `.` must either be used (decimal entry) or dropped from the pad; dead keys
  should not stay.

## 3x3 pad as a multi-level menu

The pad already mirrors the active mode's footer menu. The next step is two or
three levels, Heidenhain Pilot style:

- level 1: the mode's footer entries (`nc_menu_footer()`);
- level 2: the group that was picked - for example PRESET opens
  OD / ID / FACE / LINE / ARC / SETUP / END, TOOLS opens the tool list;
- level 3: any sub-choice a group needs;
- a visible back entry on every level, and the on-screen footer and the desktop
  pad render the same tree so the machine and the desktop cannot drift.

## Deliberately not planned yet

A dedicated `G` key plus `X`/`Y`/`Z`/`N`/`Q`/`U`/`R`/`F` letter keys. With the
TNC flow the operator never types a field letter, so these keys would mostly add
noise (and the note from the bench is that it would make the pad messy). Revisit
only if direct word addressing turns out to be needed.

## Verification when implemented

- Host tests for the validation rules: `G72` -> `G71` accepted, `G99` rejected,
  negative `G` rejected, negative `X` rejected where the word is unsigned.
- Host test for the field walk: a G71 function prompts U, R, then the profile
  range fields in the documented order, Enter on an empty field skips it.
- Panel shell: the same flow exercised through the 3x3 pad and the keyboard,
  with the frame dump used to check the prompt line.
