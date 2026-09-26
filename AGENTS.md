# AGENTS.md - working rules for this repository

These are hard rules, not style preferences. Every one exists because ignoring it
already cost this project real debugging time. The rationale and the incidents
behind them are in [docs/history-project.md](docs/history-project.md) and
[docs/history-leancam-to-nc.md](docs/history-leancam-to-nc.md).

If a rule blocks the task you were given, say so and ask. Do not quietly work
around a rule; that is how the failures below happened in the first place.

## 1. Never mask a realtime fault

Applies to: `uCNC/src/modules/lvds_renderer/`, the HSTX/LVDS scanout path, Core1
line preparation, DMA descriptors, the PSRAM draw buffer, stack sizes, and any
code that runs while the panel is scanning out.

Keep realtime memory static, aligned and boring. A stack overflow, memory
overlap or descriptor ownership bug is an architecture fault: fix the
architecture. Do not add a fallback, retry loop, health poll, auto-recovery
path, watchdog, chunked workaround or "safe mode" that hides it.

Evidence: `uCNC/src/modules/leanCam/docs/architecture.md` ("Historical
Diagnostic Lessons") and `uCNC/src/modules/lvds_renderer/README.md` ("Historical
Warnings", "Torture History").

## 2. Do not revive a retired workaround as a feature

Disabled bring-up workarounds (long filenames on SD, SPI DMA, direct scanout
from the draw path, PSRAM scanout, DMA present into the scanout framebuffer,
ping/pong handoff, chunked present) were switched off for measured reasons.
Re-enabling one requires the bench checklist and a rollback baseline, not a
convenience argument.

Evidence: `docs/sd-card-history.md`, `uCNC/src/modules/lvds_renderer/README.md`.

## 3. Keep experimental paths out of release code

Torture loops, fault injection, canaries, health polling and timing meters belong
in history. If you add one to diagnose something, remove it before the change is
done and record what it measured in the module README.

Evidence: the torture-harness section in
`uCNC/src/modules/lvds_renderer/README.md`.

## 4. One owner per domain

G7x owns contour and cycle semantics; the core parser routes and executes; NC
owns files, editor, preview and controls; SD has exactly one owner at a time;
the protocol client owns the wire. Consumers compile the owner's sources - never
copy them, never grow a second generator or a second dialect rule.

Evidence: `uCNC/src/modules/g7x/README.md`, `uCNC/src/modules/nc/TODO.md`.

## 5. Do not claim hardware validation from software tests

The suites intercept G33, so they prove targets, ordering and error propagation,
not spindle phase, pitch or spindle loss. Feed hold, Stop during queued motion,
SD cold-start mounting and panel readability are bench items. Write tests as
"software-verified" and leave the bench checklist for what it is.

Evidence: `uCNC/src/modules/g7x/TESTING.md`,
`uCNC/src/modules/nc/TESTING.md`, `uCNC/src/modules/rp2350_pio_encoder/README.md`.

## 6. Keep the module docs with the module

When you change behaviour, update that module's README/TODO/TESTING in the same
change. Rules that live only in a UI string or a status line are not rules.

## 7. Host tools: static, scripted, no surprises

Desktop tools build with the module sources and link statically so a shipped exe
has no toolchain DLL dependency. Give each tool a script that builds *and* runs
it (`tools/test_*.py`), and keep its Makefile free of hand-written copies of
another module's file list.

## 8. Size trigger

At roughly 10k lines in a "simple" module, or about 2k lines in one file, stop
patching: extract the domain into its own module, write the ownership boundary
down, give it a standalone test target, and budget the consumer rewrite as part
of the split.

Evidence: `docs/history-project.md` (LeanCam at 10,879 lines versus NC at 6,638
with G7x extracted).

## 9. A small change does not earn a picture

Applies to: anything whose result can be read in the source - a constant, a
colour, one string, a flag, one line in a table, a one-word label.

Compile it, run the tests, upload, and let the operator's eye on the glass be
the check. Do not render frames for it, convert them, crop them, zoom them,
build a contact sheet, or make a before/after picture. A wrong pixel in a change
like that is a one-line fix on the next upload - it is fine for the first try to
be wrong - while each picture costs the same attention as several one-line
mistakes, and being handed a wall of images for a one-line change is its own
kind of failure.

Frames are for what the source cannot answer: a layout that moved, an element
that was added, a redraw or a clear that leaves something behind, geometry that
is computed rather than written down. Then render once, and look only at the
region the change can reach - the strip, the pane, the corner - not the whole
panel, and not a set of variants of it.

Automated frame *comparison* is not affected: hashing frames, counting differing
pixels or bounding the rows a change touched is cheap, runs unattended, and
belongs in the verification - keep it for moves, renames and anything claimed to
be pixel-identical.

Evidence: the footer chamfer (2026-09-21) - a quarter of the key height, one
line of geometry in one function - was checked with four generated pictures (a
strip, a 4x zoom, a before/after pair and the filled-key strip) and three image
inspections on top of the frame dumps. The operator's remark: this was a simple
one-line change.

## Where to look before changing something

| Area | Read first |
| --- | --- |
| Display, memory, HSTX, DMA, PSRAM | `uCNC/src/modules/lvds_renderer/README.md`, `uCNC/src/modules/leanCam/docs/architecture.md` |
| SD card, mounting, short filenames | `docs/sd-card-history.md` |
| G71/G72/G76, P/Q, dialect | `uCNC/src/modules/g7x/README.md`, `g7x/TODO.md`, `g7x/TESTING.md` |
| NC screens, files, editor, RUN | `uCNC/src/modules/nc2/README.md`, `nc2/TESTING.md` (the retired `nc` is `nc/TODO.md`) |
| Spindle sync, encoders, G33/G76 | `uCNC/src/modules/g33/README.md`, `uCNC/src/modules/rp2350_pio_encoder/README.md` |
| Cutter compensation, lathe mode | `docs/lathe-cutter-comp.md` |
| Desktop tools | `docs/desktop-sender.md`, `tools/*/README.md` |
| Why things are the way they are | `docs/history-project.md`, `docs/history-leancam-to-nc.md` |

## Verification before you call something done

```powershell
python tools\test_g7x.py all        # generator, emitter, parser, standalone
python tools\test_nc_sender.py      # expansion + Grbl protocol, fake controller
python tools\test_nc_ui.py          # the station: build, frames, every check
python tools\test_nc2.py            # the panel module on its own (nc2)
pio run -e RP2350-LEANCAM-LVDS      # machine firmware
pio run -e RP2350-G7X-MODULE        # NC-free G7x build target
```

State plainly what you verified and what still needs the machine.
