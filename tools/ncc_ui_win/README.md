# ncc_ui - the Windows programming station for `ncc`

The panel `ncc` (`uCNC/src/modules/ncc/`) on the desktop, with its extracted
`nc_sender` source: the real screen code, the real keypad decode and the real
file API, rendered through a GDI backend instead of the HSTX scanout. It is
where the screen is checked before the machine sees it.

This is `tools/nc_ui_win/` for the third panel - the station is one per module,
the way `tools/leancam_win/` is - and it differs from nc2's only where `ncc`
differs:

| | nc2's station | this one |
| --- | --- | --- |
| the entries | `examples\presets\*.txt`, seeded onto the card | the module's own table in RAM; `examples\ncc_presets.txt` is a readable copy |
| `--seedtest` | the first start writes the card | the entries answer from RAM and the card is untouched |
| `--dump-presets` | a folder, one file per address | one file, the format `/D/ncc_presets.txt` reads |
| `--demo2test` | seeds the demo card *and* the entries | seeds the demo program only |
| the `#` key | the editor's accept | accept while a value is picked, the preview switch otherwise |
| the pane's top row | removed | the file's own row: selectable with `B`, `D` opens its folder on the file |

Everything else - `--dump`, `--dump-bench`, `--keys`, `--files`, the window, the
keypad, `--fstest`, `--streamtest`, `--keytest`, `--painttest`, `--edit2test`,
`--pad2test`, `--screen2test`, `--file2test`, `--emit2test`, `--run2test`,
`--runlock2test`,
`--manual2test`, `--tools2test`, `--block2test`, `--label2test`, `--pace2test`,
`--offset2test`, `--comp2test`, `--state2test`,
`--live2test`, `--brush2test`, `--frame2test`, `--glyph2test`, `--fps2test`,
`--scroll2test`, `--fault2test`,
`--present2test`,
`--pump2test`, `--contour2test`, `--case2test`, `--walk2test`, `--vocab2test` -
is the same code, so what nc2's README says about the layout, the keys and the
checks holds here too (`tools/nc_ui_win/README.md`).

The checks this station has and nc2's does not are the ones whose behaviour is
`ncc`'s own: `--preview2test` for `#`, `--firstline2test` for the legend's row on
line 1, `--gcode2test` for a typed G code, `--chamfer2test` for the pad's cut
corner, `--colour2test` for the word's box and its legend, `--chuck2test` for
`G972 C`, `--boot2test` for the start-up picture and its stamp, `--refuse2test`
for a cycle the panel's own generator refuses, and `--blockline2test` for the
line a block answers for. `--frame2test` pins G8's radius to G7's diameter.

## Building and running

`python tools\test_ncc.py` builds this station against `modules/ncc/` and runs
the module's checks; it is the check, not this Makefile. The Makefile is for a
person who wants the exe beside the release's `examples\`:

```powershell
python tools\test_ncc.py --brush2test
```

builds the station alone and runs just the flags named.

```powershell
cd tools\ncc_ui_win
mingw32-make
build\ncc_ui.exe --files build\ncc-files
```

The compiler needs its own `bin` on `PATH` (it finds `cc1`'s DLLs there), which
is also how `tools/test_ncc.py` sets it up.
