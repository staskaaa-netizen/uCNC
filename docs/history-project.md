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
