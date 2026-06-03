# RP2350 LVDS Renderer

This module is the RP2350/LVDS pixel backend for uCNC. It owns panel timing,
framebuffer scanout, HSTX setup, DMA descriptor progression, palette/LUT
expansion, PSRAM draw-buffer presentation, and primitive draw guards.

It does not own LeanCam program state, files, menus, G-code generation, tool
catalogs, or machine safety decisions. Higher layers may draw through the
renderer API, but they must not reach into HSTX internals.

## Current Hardware Path

- Sharp LQ121S1LG44 800x600 single-channel LVDS panel.
- RP2350 HSTX drives LVDS pairs through a PicoLVDS-style 7x256 scanline LUT.
- Core1 prepares active line buffers from the 4bpp indexed scanout framebuffer.
- DMA channel 10 feeds prepared line data to the HSTX FIFO.
- DMA channel 11 advances channel 10 through a static SRAM descriptor ring.
- Scanout framebuffer and realtime line/descriptor state live in SRAM.
- Optional PSRAM draw backbuffer is copied into SRAM by `lvds_hstx_present()`.

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

## Public Boundary

The release boundary is intentionally small:

- initialize scanout
- set palette entries
- draw bounded primitives into the current draw target
- present the draw target into scanout
- query basic readiness/status

Higher layers must not depend on DMA channel internals, line-buffer addresses,
descriptor layout, core1 state, or old diagnostic counters.

Draw guards belong in the low-level API: invalid coordinates must be rejected
before they can touch framebuffer memory.

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
- Scratch X/Y placement tricks for HSTX live state

Experimental PSRAM scanout was rejected because it was slow and caused sync
loss. Experimental DMA present from the PSRAM draw buffer into the SRAM scanout
framebuffer measured roughly 7.3 ms for 240,000 bytes and produced display
garbage even with HSTX DMA priority and paced present DMA. Keep
`lvds_hstx_present()` on the CPU copy path unless the scanout architecture
changes.

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
