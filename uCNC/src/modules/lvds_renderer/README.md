# RP2350 LVDS Renderer

This module is the RP2350/LVDS pixel backend for uCNC. It owns panel timing,
framebuffer scanout, HSTX setup, DMA descriptor progression, palette/LUT
expansion, PSRAM draw-buffer presentation, and primitive draw guards.

It does not own LeanCam program state, files, menus, G-code generation, tool
catalogs, or machine safety decisions. Higher layers may draw through the
renderer API, but they must not reach into HSTX internals.

## One Copy: A Module, and a Library

This directory is the renderer's only copy, and it is consumed three ways - none
of them a fork of the sources:

- **the uCNC module**: `custom_ucnc_modules` names `lvds_renderer`, the RP2350
  environments compile it with `+<src/modules/lvds_renderer/>`, and
  `LOAD_MODULE(...)` wires it in. The module is published from this directory to
  the `uCNC-modules` repository's `pre-alpha` branch, which is the archive the
  config builder downloads modules from;
- **the desktop station's drawing core**: `tools/nc_ui_win` compiles
  `lvds_draw_api.c`, `lvds_palette.c` and the two fonts *unmodified* and gives
  them a host backend (`lvds_host.c`) implementing the same primitives over a
  GDI frame, so the bench and the machine draw one picture from one source;
- **a library**: `library.json` is the PlatformIO manifest for a project that is
  not uCNC - point `lib_extra_dirs` at this directory or take it as a submodule.
  The palette, the draw API and the fonts need nothing but `lvds_hstx.h`; the
  HSTX, PSRAM and boot files need the Pico SDK and four uCNC entry points
  (`mcu_millis`, `cnc_delay_ms`, `proto_info`, `cnc_is_file_io_critical`), which
  a non-uCNC consumer has to define for itself today.

What a consumer needs before using any of it - the panel, the load-bearing
spare active lines at the frame's close, the sixteen-slot palette, the realtime
memory rules and what still needs the bench - is in `LIMITS.md`.

## Current Hardware Path

- Sharp LQ121S1LG44 800x600 single-channel LVDS panel.
- RP2350 HSTX drives LVDS pairs through a PicoLVDS-style 7x256 scanline LUT.
- Core1 prepares active line buffers from the 4bpp indexed scanout framebuffer.
- DMA channel 10 feeds prepared line data to the HSTX FIFO.
- DMA channel 11 advances channel 10 through a static SRAM descriptor ring.
- Scanout framebuffer and realtime line/descriptor state live in SRAM.
- Optional PSRAM draw backbuffer is copied into SRAM by `lvds_hstx_present()`.

## The Panel's Mounting, and the Turn

The glass is 800x600 and the machine is built with it **turned a quarter**: the
operator reads a 600x800 picture. That turn is this module's business - it owns
the pixels - and it changes nothing about the scanout:

- the HSTX timing, the active-line LUT, the DMA ring and
  `lvds_hstx_present()` are untouched: the same 800x600 frame goes to the panel
  at the same rate, and Core1 still prepares the same lines;
- what turns is where a *drawing* puts its pixels. `lvds_hstx_pixel()` and
  `lvds_hstx_fill_rect()` are the only two primitives that write, and both map
  the picture's coordinates onto the glass through `LVDS_PANEL_X/Y` and
  `LVDS_PANEL_RECT_*`. Lines, rectangles, ellipses and **text** reach the glass
  through those two, so they turn for free - a glyph is filled rectangles, and
  the text code never touches a pixel itself;
- `LVDS_VIEW_WIDTH`/`LVDS_VIEW_HEIGHT` (600x800 while turned) is what the
  screens draw in, and `LVDS_PANEL_TURN` (90 by default) says which way round
  the glass is bolted. `270` is the other way round, `0` is an unturned panel;
  a build can set it with `-DLVDS_PANEL_TURN=...`.

The rule that keeps this honest: the turn is stated **once**, in `lvds_hstx.h`,
and every backend and every reader goes through it. The station's own backend
(`tools/nc_ui_win/lvds_host.c`), its window and its frame dumps all read that
one statement, so the bench cannot drift from the machine. A picture that comes
out upside down is `LVDS_PANEL_TURN` set to the wrong quarter - one number.

## PlatformIO Target

- env: `RP2350-LEANCAM-LVDS`
- UF2: `.pio/build/RP2350-LEANCAM-LVDS/firmware.uf2`
- board map: `src/hal/boards/rp2350/boardmap_waveshare_pizero_minimal.h`

The target name mentions LeanCam because that is the current product build
using this renderer. The LVDS renderer itself remains a hardware/display
backend.

## LVDS Pinout

- GP12 D0+
- GP13 D0-
- GP14 D1+
- GP15 D1-
- GP16 D2+
- GP17 D2-
- GP18 CLK+
- GP19 CLK-

## Panel Clock Notes

The Sharp LQ121S1LG44 nominal pixel clock is 40 MHz, but this panel locks below
that in practice.

The current HSTX packing emits one pixel clock for each 7 serial transfer
phases, so the useful bring-up estimate is:

```text
pixel_clock_mhz = LVDS_SYS_CLOCK_KHZ / 1000 / LVDS_HSTX_CLOCK_DIV * 2 / 7
```

Known working test points:

- `280000 / div 2` -> about 40.00 MHz, current configured point for
  `RP2350-LEANCAM-LVDS`
- `370000 / div 3` -> about 35.24 MHz
- `270000 / div 3` -> about 25.71 MHz
- `270000 / div 4` -> about 19.29 MHz
- `370000 / div 4` -> about 26.43 MHz
- `280000 / div 3` -> about 26.67 MHz, older stable empirical point

`420000 / div 3` did not produce video in testing. Treat that as an
RP2350/board overclock, voltage, PSRAM, or flash stability boundary, not as a
panel timing requirement.

When testing clocks, change only the clock path values:

- `LVDS_SYS_CLOCK_KHZ`
- `LVDS_HSTX_PLL_KHZ`
- `LVDS_HSTX_CLOCK_DIV`

Do not mix clock tests with scanout architecture changes.

## Panel Alignment

Framebuffer row 0 lands on the panel's top row with the vertical values below
unchanged, and moving the back porch does not move the picture on the glass -
the panel follows the active lines rather than counting from the sync. Measured
with single-row markers drawn on the first and last framebuffer rows.

Open problem: the **close of the frame does not come out clean on the panel**.
With a marker on the last framebuffer row the glass showed it about two to three
rows above the bottom edge, with fragments below it, and a two-row marker read
as two separate lines. Only the last rows are affected; the rest of the frame is
right. Both porches were tried and neither altered what the panel showed, so
they are back at their original values. Until it is understood, the UI keeps the
bottom few rows of the panel as background (see `nc/TODO.md`) instead of drawing
a border into them.

**Two probes (bench, 2026-10-04), and what they narrowed it to.** The bench put
the question as *"is it our bad timing or wrong closure of frame ... so we will
see is it our software or bogus receiver"*, and ran two experiments:

1. **Four extra *blanking* lines** - the ring's own guard descriptors, reached
   four lines sooner, so the frame's *total* grew but its active area did not:
   **nothing changed on the glass**.
2. **Four extra *active* lines** (`#define LVDS_HEIGHT 604`): the picture's last
   lines came out **right**.

So the fault is not the frame's *total* and not the link's timing; it sits on
the **close of the active area**, and something of the picture is being shown
where the frame closes. Both probes are scaffolding and are out of the tree
again (the glass is back at 800x600).

**Where the closure actually is.** `lvds_hstx_line_addr_for_v()` gives every
line of the vertical blanking - and every descriptor past `LVDS_HSTX_V_TOTAL`,
the ring's guard - the *inactive* line: true blanking, nothing to show. There is
exactly **one line that is not blanking**: the line immediately before the
active area,

```c
if (v == (LVDS_HSTX_V_INACTIVE - 1)) {
    return (uint32_t)(uintptr_t)g_active_line[0];   /* an active-encoded line */
}
```

and its buffer is **stale**, not blank: `g_active_line[0]` holds the last line
computed into slot 0, which on a 600-active frame is **row 596** - the picture's
fourth-from-last line. That single line is the leak, and it explains both probes
exactly: four more *active* lines move slot 0's last content onto a *blank* extra
line (so the picture comes out right), while four more *blanking* lines leave
that stale picture line exactly where it was (so nothing changes).

**Option 1's panel check came back negative**, so that closure line was never
what the panel showed: making it true blanking changed nothing on the glass, and
the line is back as it was (its comment records the check).

**The marker probe found it (2026-10-04).** Five known colours - white, red,
green, blue, black - were painted on the frame's last five lines, one colour per
line, with a break every 32 rows so a repeat is visible. The bench read back:
*"and red is twice."* Our own known content, one line of it **shown twice**: the
close of the active area **repeats a line**, whatever content is there. Not our
content, not our blanking (the extra-blanking probe changed nothing), not the
clock - the panel/link repeats a line at the end of the active area.

**The fix: four spare active lines at the frame's close.** The frame carries
`LVDS_PANEL_ACTIVE_HEIGHT 604` against the glass's 600, the view - and so every
screen, and the station - stays the glass's own 600x800 mapped onto the *first*
600 active lines, and the last four active lines are the frame's blank tail. The
repeated line lands on that tail instead of on the picture. Confirmed on the
bench: with the spare lines in, the marker probe's five colours read clean and
single - and the probe has come out of the tree again. **The spare tail is
load-bearing**: shortening the frame back to 600 active lines puts the repeat
back on the picture.

**How much tail is enough - measured 2026-10-04.** The bench walked the active
height down: at **602** lines an extra *black line* appears (the tail is one
short and the close's own line shows), at **603** and **604** the glass is clean,
and at **600** the repeat lands on the picture itself. So the close needs the
active area to end **at least three lines** past the picture: the frame keeps
`604` for one line of margin over that threshold. Any future change to the
frame's length has to keep that tail; a *root cause* is still not known - what is
known is that the panel/link repeats or eats a line at the close of the active
area, independently of the content, and that three spare lines absorb it.

## Memory Ownership

Realtime scanout memory is static, aligned, and SRAM-backed:

- 4bpp scanout framebuffer.
- active HSTX line buffers.
- descriptor/control DMA ring.
- explicit core1 stack.
- HSTX control, core1 state, palette, and LUT state.

Core0 may draw and present only through the public renderer/HSTX APIs. Core1
owns LUT expansion and line preparation. DMA owns HSTX FIFO feed and descriptor
progression.

Static storage gives stable lifetime and predictable addresses. It does not
make memory core-private; ownership is enforced by module boundaries and API
discipline.

Rules:

- Keep scanout sourced from SRAM.
- Keep permanent realtime buffers static and aligned.
- Do not put large line buffers, framebuffers, descriptor pools, or core1 live
  state in Scratch X/Y.
- Do not add large automatic locals to the core1 scanout path.
- Do not reintroduce PSRAM front/back scanout.
- Do not reintroduce PSRAM-to-active-scanout DMA present.
- Do not add live HSTX health polling or auto-recovery to release code.

If scanout fails now, treat it as a real memory, ownership, or timing fault.

## Line Preparation Rule

Core1 prepares the line the descriptor ring reaches
`LVDS_HSTX_ACTIVE_LINE_BUFFERS - 1` scanlines later, and that arithmetic wraps
into the next frame. Preparing from the current descriptor's active `y` and
stopping at the image edge instead left the first lines of every frame holding
the tail of the previous one, because those descriptors are reached from the
vertical blanking, where the old rule prepared nothing. The artifact read as a
band of the bottom of the screen across the top of the header. Keep the wrap.

## Public Boundary

The release boundary is intentionally small:

- initialize scanout
- set palette entries
- take the palette again (`lvds_hstx_palette_reset()`; the boot picture brings
  colours of its own and hands them back before the work screen draws - one
  caller, one call, at start-up)
- draw bounded primitives into the current draw target
- present the draw target into scanout
- query basic readiness/status

Higher layers must not depend on DMA channel internals, line-buffer addresses,
descriptor layout, core1 state, or old diagnostic counters.

Draw guards belong in the low-level API: invalid coordinates must be rejected
before they can touch framebuffer memory.

**Images belong here too, and are not yet here.** The boot splash carries its
own picture machinery in the panel that shows it (`ncc_boot.c`: a run-length
decode, a colour-to-palette mapping per colour, and taking the palette and
handing it back). The panel has asked for pictures on the keys of its 3x3 pad as
well (`ncc/TODO.md`, "Pictures on the pad's keys"), which is the same thing
again - an image, its own colours, drawn at a place - so the answer is one
element in this module's boundary rather than a third copy of that code:

- the renderer owns the image: its runs, its size, its colours, and the palette
  slots those colours take while it is up (`lvds_hstx_palette_reset()` is the
  hand-back, and it has exactly one caller today - the splash);
- a caller with an image in hand says `lvds_image_draw(&image, x, y)` and
  nothing else; where the image came from is the caller's business;
- the palette bookkeeping has to live with the image, because the sixteen slots
  are shared with the panel's own colours - that is what the splash's own bug
  was about (colours taken when a table was *parsed* rather than when a colour
  was first *used*).

The first user is the splash itself; converting it is the pilot for the pad.

## Historical Warnings

These were useful while diagnosing the old instability, but they are not
supported fallback paths:

- old ping/pong DMA scanout
- CPU-rearm scanout
- direct scanout from the draw path
- PSRAM scanout/frontbuffer tests
- PSRAM-to-SRAM present DMA
- renderer-owned generated G-code caches
- renderer-side G-code generation
- HSTX live health poll and auto-recover
- line-repeat watchdog and fault injection
- canary/overlap/torture diagnostics in release builds
- scanline slack/timing log instrumentation in release builds
- chunked framebuffer present
- Scratch X/Y placement tricks for HSTX live state

Experimental PSRAM scanout was rejected because it was slow and caused sync
loss. Experimental DMA present from the PSRAM draw buffer into the SRAM scanout
framebuffer measured roughly 7.3 ms for 240,000 bytes and produced display
garbage even with HSTX DMA priority and paced present DMA. Keep
`lvds_hstx_present()` on the CPU copy path unless the scanout architecture
changes.

Chunked framebuffer present was also removed. It was a pacing experiment for
earlier unstable memory/scanout layouts; the current strict-memory backend uses
one normal `lvds_hstx_present()` copy.

Old failure logs showed cases where uCNC and motion continued while display
scanout froze. That class was a stalled HSTX DMA/control path, not a full
machine-core crash. The current descriptor-ring backend replaced the old
ping/pong handoff and removed the release recovery code.

## Torture History

`LVDS_HSTX_TORTURE_TEST` was a temporary compile-time stress harness for the
`RP2350-LEANCAM-LVDS` target. It is removed from release code.

The harness repeatedly exercised selected program runs, snapshot churn,
framebuffer present, historical line-repeat injection, stack diagnostics, and
canary diagnostics while emitting compact serial summaries. Those knobs belong
in history only. Do not add them back to release code as normal runtime
configuration.
