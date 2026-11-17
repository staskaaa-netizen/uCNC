# nc_sender

`nc_sender` owns the program-to-controller-block answer and its pacing for the
NC panels.

It reads program text through G7x's generic `g7x_doc_t` line provider,
expands G70/G71/G72/G76 blocks, resolves Fanuc `U`/`W` increments, applies the
controller's line filter, reports the program line behind every emitted block,
and paces hand-over one unit at a time. It does not own a screen, a document
type, a keypad, a file or the tool table.

## Boundary

Input:

- `g7x_doc_t`: bounded random access to program text.
- optional status and tool-radius callbacks.
- starting program line.

Output:

- one controller block at a time;
- the source line and block line that answer for it;
- a bounded G7x error state;
- run start/stop/hold and execution-line state.

A fresh `FULL` or `FROM` run clears a controller HOLD left by the previous run,
unless a door or feed hold is still active. HOLD does not count as execution for
the caller's cursor movement: a held RUN screen can still select the row the
next `SINGLE` or `FROM` will use.

The caller keeps:

- `U`/`W` display and editor rules;
- the tool table and its radius lookup;
- status words and screen text.

That keeps the dependency direction `ncc -> nc_sender -> g7x`; the sender never
includes an `ncc` header.

## Files

| file | owner |
| --- | --- |
| `nc_sender.h` | the public source/stream and pacer/run API |
| `nc_sender.c` | line filtering, `U`/`W`, G7x expansion and source-line reporting |
| `nc_sender_run.c` | one-unit pacing, queue, N-number mapping and run state |
| `tests/nc_sender_host_test.c` | standalone emitter host checks |

`tools/test_nc_sender_module.py` builds and runs the standalone check without
the panel, filesystem, framebuffer or controller.
