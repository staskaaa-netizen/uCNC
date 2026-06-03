# LeanCam / LVDS Current Architecture

Status: current working profile after the HSTX/LVDS cleanup.

Purpose: short enough to audit, long enough to catch duplicate owners. If a
future change adds another buffer, queue, timer, cache, or task, compare it to
this document first.

## 1. Golden Rule

LeanCam is a source of lines and snapshots. uCNC owns parsing, planner buffering,
motion pacing, and machine state. The LVDS renderer owns pixels only.

Do not reintroduce:

- renderer-side G-code generation
- generated-preview line arrays
- alternate stream tasks
- full generated run buffers for selected runs
- fixed-rate preview schedulers
- module policy hidden outside `cnc_hal_overrides.h`

## 2. One Runtime Owner

`cnc_io_dotasks` calls the LeanCam/LVDS owner through:

```text
cnc_io_dotasks
  -> execution_controller_poll()
```

`execution_controller_poll()` owns the order:

```text
poll keypad
  -> leancam_bridge_handle_key()
leancam_bridge_tick()
  -> start armed run streams
  -> advance sim-preview stepper after renderer ack
  -> service pending NC run arm
  -> autosave/file refresh when safe
leancam_visual_state_poll()
  -> build one UI snapshot
leancam_visual_draw()
  -> draw pixels from snapshot
lvds_hstx_present/chunked_step()
  -> publish pixels to scanout
```

There is no second module secretly polling keys, advancing LeanCam state, or
generating G-code from the renderer.

## 3. Selected LeanCam Run

Selected LeanCam program run is demand-fed into the normal uCNC stream.

```text
key # in program view
  -> lc_menu_cb_program_run_selected()
  -> first press arms fullscreen sim preview
  -> second press runs selected line/owning region
  -> lc_run_selected_line()
  -> lc_run_selected_line_stream()
  -> leancam_gcode_stepper_begin(program, start, end)
  -> grbl_stream_readonly(lc_step_stream_getc, ...)
```

Then uCNC pulls bytes:

```text
uCNC parser asks stream for byte
  -> lc_step_stream_getc()
  -> if current generated line is drained:
       leancam_gcode_stepper_next()
  -> one generated NC line is exposed as bytes
```

Important: LeanCam does not generate the whole selected G71/G72/G76 run first.
The same stepper emits the next source/generated line only when the stream needs
it.

## 4. Raw NC File Run

File-manager NC run is raw file streaming, not LeanCam cycle expansion.

```text
key # in NC file view
  -> selected mode: Single / From / Full
  -> short arm delay
  -> lc_run_nc_selected_mode_now()
```

Two cases:

- Cached viewer program is available: selected range may use the same
  `lc_step_stream_*` path.
- Raw file source is used: `lc_nc_file_stream_*` opens the file and reads the
  next source line only when uCNC asks for it.

Raw `.nc` execution must remain raw NC.

## 5. Sim Preview

Sim preview uses the same generator interface style as selected run, but its
consumer is the snapshot/renderer instead of the uCNC parser.

```text
first # in program view
  -> lc_preview_gcode_begin_selected(start, end)
  -> leancam_gcode_stepper_begin(program, start, end)
```

Then bridge ticks expose one line at a time:

```text
leancam_bridge_tick()
  -> lc_preview_gcode_step()
  -> if renderer acked previous sequence:
       leancam_gcode_stepper_next()
       copy one generated line into g_preview_stepper.current_line
       increment draw_seq
```

Snapshot carries:

```text
leancam_sim_preview_active
leancam_sim_preview_seq
leancam_sim_preview_index
leancam_sim_preview_line
```

Renderer consumes:

```text
leancam_visual_draw()
  -> draw frame->leancam_sim_preview_line when seq changes
  -> leancam_bridge_preview_ack(seq)
```

This is not fixed-time pacing anymore. It is task-loop stepping with renderer
acknowledgement. The ack prevents skipped preview lines without adding a FIFO.

## 6. Snapshot Boundary

Snapshot state flows one way:

```text
LeanCam bridge / machine state
  -> leancam_bridge_fill_snapshot()
  -> `ui_snapshot_frame_t` from `leancam_snapshot_frame.h`
  -> leancam_visual_state_poll()
  -> leancam_visual_draw()
```

The renderer may read snapshot fields and keep visual-only caches such as live
simulation material masks or small redraw hashes. It must not mutate LeanCam app
state, open files, poll keys, or call the G-code generator.

## 7. LVDS/HSTX Boundary

Current LVDS module ownership:

- `lvds_renderer_boot.c`: boot/listener glue.
- `lvds_hstx.c`: HSTX scanout, descriptor ring, draw guards, and present.
- `lvds_draw_api.c`: drawing primitive boundary over HSTX framebuffer.
- `lvds_psram.c`: PSRAM helpers.
- `lvds_palette.c`: palette.

Current LeanCam visual ownership:

- `visual/leancam_visual.c`: page-level drawing from snapshots.
- `visual/leancam_visual_state.c`: snapshot publication.
- `visual/lvds_ui_*.c`: UI layout/header/footer/text helpers now owned by
  LeanCam, not the LVDS hardware backend.

LVDS hardware code does not know LeanCam menus, tool catalog, editor state, or
G71/G76 semantics.

## 8. Resource Ownership

`leancam_resource.c/h` is the small shared-resource gatekeeper:

- file IO critical section
- autosave permission
- PSRAM regions for current active users
- frame access hooks

Active PSRAM regions:

- `LC_PSRAM_REGION_LIVE_SIM`
- `LC_PSRAM_REGION_TOOL_CATALOG`

Removed regions:

- direct stream buffer
- renderer generated-preview G-code cache
- renderer generated-preview G-code state snapshot

## 9. Fixed Memory Layout

The last HSTX bug swarm was solved by making ownership and address areas boring
and explicit. Treat this layout as part of the architecture, not as incidental
allocation detail.

### SRAM / Core-Local HSTX State

`lvds_hstx.c` owns the realtime scanout memory. It is static, aligned, and kept
out of random stack/Scratch X experiments.

Active blocks:

- `g_framebuffer`: 4 bpp SRAM scanout framebuffer,
  `LVDS_HSTX_WIDTH * LVDS_HSTX_HEIGHT / 2`.
- `g_active_line`: four active HSTX line buffers,
  `LVDS_HSTX_ACTIVE_LINE_BUFFERS` by `LVDS_HSTX_LINE_TRANSFERS`.
- `g_inactive_line`: inactive porch/sync line buffer.
- `g_inactive_line_vs`: inactive VS line buffer.
- `g_scanout_line_addr_ring`: 1024-entry descriptor address ring, aligned to
  4096 bytes.
- `g_core1_stack`: explicit core1 stack,
  `LVDS_HSTX_CORE1_STACK_WORDS`.
- `g_hstx_ctrl`, `g_hstx_core1`, and palette/LUT state.

DMA ownership:

- DMA channel 10 feeds HSTX from line buffers.
- DMA channel 11 advances channel 10 through the descriptor ring.
- Core1 refreshes line buffers/descriptors.
- Core0 may request draw and present only through public HSTX APIs.

Rules:

- Do not move HSTX live state to automatic locals.
- Do not put descriptor rings, line buffers, or framebuffers into Scratch X/Y.
- Do not add direct-scanout toggles or another DMA copy path from PSRAM to
  active scanout while scanout is running unless it is a deliberate isolated
  experiment documented in the LVDS README.

### PSRAM Map

PSRAM is memory-mapped at:

```text
LVDS_PSRAM_BASE = 0x11000000
```

Current fixed offsets:

```text
0x000000  PSRAM draw backbuffer, size LVDS_HSTX_FB_SIZE
0x080000  LC_PSRAM_REGION_LIVE_SIM
0x0C0000  LC_PSRAM_REGION_TOOL_CATALOG
```

Meaning:

- The PSRAM draw backbuffer is used by normal LeanCam drawing when
  `LEANCAM_USE_PSRAM_BACKBUFFER` is enabled and PSRAM init succeeds.
- `lvds_hstx_present()` copies the PSRAM draw buffer into the SRAM scanout
  framebuffer.
- Live simulation scratch/mask memory starts at 512 KiB.
- Tool catalog scratch/fallback memory starts at 768 KiB.

Removed PSRAM regions:

- direct stream text buffer
- generated-preview G-code cache
- generated-preview G-code state snapshot

Rules:

- PSRAM offsets are fixed owners, not a heap.
- New PSRAM users must go through `lc_resource_psram_region()`.
- Do not overlap offset 0; it is the draw backbuffer when enabled.
- Do not allocate generated run/preview text in PSRAM.
- If a future region is added, document its offset here and leave clear space
  from existing regions.

### Historical Diagnostic Lessons

The stable profile depends on keeping realtime memory visible and boring.

- Core1 scanout has an explicit stack. Runtime stack/canary/overlap diagnostics
  were useful during bring-up, but are not part of the live release path.
- HSTX health polling, auto-recovery, line-repeat fault injection, direct
  scanout toggles, and torture loops were removed from release code. Their
  findings live in the LVDS README warning/history section.
- A memory overlap, stack overflow, or descriptor ownership bug is a real
  architecture fault, not something to mask with fallback behavior.

### Short Memory Model

```text
core0/uCNC/LeanCam:
  program state, file IO, stream adapters, PSRAM drawing, snapshots

core1/HSTX:
  scanout scheduler, line expansion, descriptor refresh, explicit stack

DMA:
  HSTX FIFO feed and descriptor progression

PSRAM:
  draw backbuffer and fixed LeanCam scratch regions only

SRAM:
  live scanout framebuffer, line buffers, descriptor ring, core1/HSTX state
```

This separation is the current stable answer. If a bug fix wants to blur these
areas again, assume it is wrong until proven otherwise.

## 10. Current Intentional Layers

These are still real and intentional:

- `execution_controller`: single visible order owner.
- `leancam_bridge`: app state and orchestration.
- `leancam_gcode_stepper`: selected run and sim-preview line generator.
- `lc_step_stream_*`: uCNC parser stream adapter for selected generated runs.
- `lc_nc_file_stream_*`: uCNC parser stream adapter for raw file runs.
- `ui_snapshot_frame_t`: LeanCam bridge-to-renderer data handoff, declared in
  `leancam_snapshot_frame.h`.
- `leancam_visual_*`: snapshot-to-pixels.
- `lvds_hstx`: pixels-to-scanout.

These are not duplicate owners right now. They are boundaries.

## 11. Removed Architecture

Gone from active code:

- `cam_stream.c/h`
- `LC_SELECTED_RUN_USE_STREAM`
- generated selected-run line queue
- PSRAM direct stream text buffer
- direct-stream buffer knobs
- renderer-side generated G-code cache
- renderer-side G-code state save/restore for preview
- fixed millisecond generated-preview pacing
- generated-preview line array/FIFO
- hidden renderer-side LeanCam advancement

Historical failure notes are kept in `lvds_renderer/README.md`. Their place is
documentation, not active fallback paths.

## 12. Audit Checklist

When looking for future double layers, search for these classes:

- another call to `grbl_stream_readonly()` outside the two stream adapters and
  M30 finish helper
- another generated-line array/cache
- any `leancam_gcode_*` generator call from `visual/` or `lvds_renderer/`
- any file IO from `visual/` or `lvds_renderer/`
- another task/timer that advances LeanCam state
- another PSRAM region for stream text or preview G-code
- another scratch/stack/descriptor placement trick for HSTX/core1
- another direct-scanout or PSRAM-to-active-scanout DMA path
- config knobs in core/helper files that belong in `cnc_hal_overrides.h`

## 13. TODO Status Snapshot

### Finished / Current Profile

- `cnc_hal_overrides.h` remains active and is the config surface.
- Execution controller is the single visible runtime owner.
- Keypad polling moved out of renderer state.
- LVDS hardware renderer is a pixel backend only.
- LeanCam visual files moved out of `lvds_renderer`.
- Old `cam_stream` alternate executor removed.
- Old direct stream staging buffer removed.
- Selected LeanCam runs are demand-fed by `leancam_gcode_stepper_*()`.
- G71/G72/G76 selected runtime emits generated lines one at a time.
- Raw NC file runs stream source lines on demand.
- Renderer-side generated preview cache removed.
- Sim preview uses a bridge-owned `leancam_gcode_stepper_t`.
- Preview handoff uses renderer ack, not fixed-time pacing.
- HSTX realtime memory is static/aligned with an explicit core1 stack.
- PSRAM use is fixed-region only: draw backbuffer, live sim, tool catalog.
- `.lcam` is obsolete; NC is the intended editable/runnable format.
- Pipe/brace-era stored rows are removed from active program storage.

### Still Useful / Not Finished

- `leancam_bridge.c` is still large and owns too much orchestration.
- Menu callback table still lives in bridge.
- Draft commit still mixes mode changes, autosave, preset side effects, and UI
  state.
- File browser/prompt and NC viewer are improved but still bridge-orchestrated.
- Renderer split is improved, but page drawing remains a large visual file.
- Editor preview still uses the temporary yellow material-removal fill.
- More robust direct scanline rough-area preview is still desirable.
- Setup should eventually move from `SETUP ...` to CNC-looking setup G-codes
  such as `G970/G971/G972/G973`.
- Optional ICP/vector contour editor remains future work.

### Stale / No Longer Desired

- Generated sim line RAM/PSRAM cache.
- Renderer-owned generated preview preparation.
- Fixed millisecond generated-preview pacing.
- Special NC run-view executor beyond the normal demand-fed/raw file streams.
- Old `.lcam` import/fallback work.
- Private pipe-language storage.
- `cam_stream` task/queue path.
- Full generated selected-run buffer before feeding uCNC.
- Scratch X/Y and stack-overflow-era HSTX placement experiments.
- PSRAM scanout/frontbuffer experiments as normal runtime paths.

## 14. Short Mental Model

```text
Input:
  keypad -> execution_controller -> bridge

Run:
  bridge -> stepper/file line source -> grbl_stream_readonly -> uCNC parser

Preview:
  bridge -> stepper -> snapshot line -> renderer -> ack

Display:
  bridge/machine state -> snapshot -> visual draw -> LVDS/HSTX scanout
```

If a new feature does not fit one of those arrows, it probably needs a hard
look before it lands.
