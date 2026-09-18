# NC screen review before the next big step

Scope: the NC screens on both targets (RP2350 panel and the Windows shell), the
footer/menu model, the editor and the operator messages. Method: source review of
`nc_visual.c`, `nc_menu.c`, `nc_text.c`, `nc_run.c` plus use of the desktop panel.
Items marked *bench* can only be judged on the machine.

## The one defect class behind most of this

Almost every finding below is the same logical mismatch that produced the
Up/Down bug: **one key, or one entry, carries two meanings.**

- **Key overload.** `B`/`C`/`D` are footer actions while browsing and
  sign/point/accept while editing a value (`nc_visual_key_char` against the
  `nc_text_edit_handle_key` dispatch). Up/Down used to be the sign and the point.
- **Entry kind mismatch.** `HOLD` is a state presented as an action;
  `DIM`/`STOCK`/`PATH`/`ROUGH` are states that were highlighted like actions;
  momentary actions keep a sticky highlight that reads as a state.
- **Destination mismatch.** The cursor is both the browsing position and the
  armed run line with nothing distinguishing them; `#` RUN and `1` SINGLE are
  the same action under two names.
- **Hint mismatch.** The footer prints the key meaning for the *browsing*
  context, which stops being true the moment the context changes.

So this is not a list of unrelated defects to patch one by one. The remedy is a
single model, and it should be agreed before more keys or menus are added:

1. every pad entry has an explicit kind - **action** (momentary),
   **toggle** (state), **mode** (latching view);
2. one key has one meaning inside a context, and a context change swaps the
   whole map rather than re-purposing a few letters;
3. toggles show their state, actions never show persistent state, and a mode
   entry shows which view is active;
4. the footer shows the meaning of the *current* context only.

## 1. RUN and HOLD

What exists and works:

- the header shows the state (`RUN`, `RUN ACTIVE`, `RUN HOLD`, `RUN IDLE`) from
  `nc_visual_run_state_text()`, including the controller's own `EXEC_HOLD` flag;
- `HOLD` is a real toggle: `nc_run_toggle_hold()` plus a status line
  (`RUN hold` / `RUN resumed`);
- `FR OM`/`SINGLE`/`RUN` arm and start from the cursor line; `RESET` cancels.

What needs work:

1. **HOLD is a toggle drawn as an action** (instance of the defect class above):
   the entry stays labelled `HOLD` while the machine is held, so the only state
   indication is the header text. It should take a toggle kind and show
   `RESUME`/held state like the switches.
2. **`#` RUN and `1` SINGLE dispatch the same action** (also the class):
   `NC_FOOTER_ACTION_SINGLE` for both. Either RUN becomes its own action (run to
   end from the armed line) or the label stops claiming otherwise.
3. **No armed-line indicator.** The cursor moves and `FROM` uses it, but nothing
   on screen says "will start at line 12" after `FROM` is pressed, and the cursor
   highlight does not distinguish "selected to run from" from "just browsing".
4. **Stop/reset semantics need a pass on the bench**: stop mid-cycle, stop while
   queued after EOF, stop immediately after resume, and what the screen shows in
   each case. Software tests cover the state machine; the operator feedback is
   not yet described anywhere.
5. Hold state is not reflected in the footer at all, and there is no visual
   difference between "held by the operator" and "held by the controller".

## 2. Footer and menu model

- The same physical keys mean footer actions while navigating but `B` = sign,
  `C` = point, `D` = next word/accept while editing a value. Dedicated `-`/`.`
  keys and arrow keys now exist, which gives a safe path, but the ambiguity in
  the letters remains until the pad is redesigned.
- Switches (`DIM`, `STOCK`, `PATH`, `ROUGH`) now show their state; ordinary
  actions keep a sticky "last pressed" highlight, which can be mistaken for a
  state. Either clear it after dispatch or accept it as an "action" convention
  and say so on screen.
- The 3x3 pad mirrors the active mode's footer and switches to digits while a
  value draft is active; the multi-level menu is still to come
  (`docs/nc-editor-tnc415.md`).
- Footers have no legend for the colour language: key letters are always accent
  coloured, values are highlighted backgrounds. New operators cannot infer it.

## 3. Editor

- Word validation (letter aware), rejected-edit messages and same-letter field
  navigation are in place; the field walk is limited to editable views
  (EDIT/MDI/TOOLS) so RUN and SIM keep their own key behaviour.
- The root cause of the recent bug family (value `0`, dead navigation, arrow
  edits) is per-keystroke application of the draft: everything that leaves a
  field can re-apply a value. The TNC 415 flow in `docs/nc-editor-tnc415.md`
  (apply on accept only) removes that class and is the single highest-value
  editor change outstanding.
- Field prompts, per-case reasons (minus versus point versus unsupported code)
  and the letter keys are still future work, deliberately.

## 4. Messages

- One transient message rides in the existing footer status line and clears on
  the next key. Kinds (INFO/WARNING/ERROR) exist but are not rendered
  differently yet.
- Missing: per-case wording, and any history ("what did it complain about last
  time?") for a serious rejection.

## 5. State and persistence

- The preview switches (`DIM`, `STOCK`, `PATH`, `ROUGH`) live only in RAM; they
  reset on reboot. If the operator's last view is expected to survive, it needs
  the state save.
- Cursor/line persistence across reboot is an open NC TODO item.
- Settings/alarm guidance in the header already distinguishes reset, alarm,
  door and untrusted position; keep that as the model for other blocking states.

## Priority

| P | Item | Size |
| --- | --- | --- |
| 1 | Agree and apply the key/entry model: kind per entry, one meaning per key per context, state shown for state entries | medium |
| 1 | Instances to fix with it: HOLD as toggle, RUN versus SINGLE, sticky action highlight | small each |
| 1 | Editor: apply the draft on accept only (removes the re-apply bug family) | medium |
| 2 | Armed start-line feedback after FROM, distinct from the browsing cursor | small |
| 3 | Persist the preview switches with the rest of the UI state | small |
| 3 | Pad: `-`/`.` buttons and the multi-level menu | medium |
| 3 | Message types rendered differently; last rejection kept visible | small |
| 4 | Footer colour legend on a help/status line | small |
| bench | Stop/hold/reset feedback through a real cycle, including after EOF | - |

## Verification

Automated today: word-value validation, same-letter field navigation, keyboard
map sanity, panel layout frame dump (`python tools/test_g7x.py all`,
`python tools/test_nc_sender.py`, `python tools/test_nc_ui.py`). Everything
listed as *bench* needs the machine and belongs in `nc/TESTING.md`.
