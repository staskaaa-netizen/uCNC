# Lathe cutter radius compensation (G40/G41/G42)

This is the biggest untouched feature in the core. It is a spec and a staged
plan, not an implementation.

## What exists today

- `parser.c` parses the compensation modal group and stores it
  (`new_state->groups.cutter_radius_compensation = code - 40`). The group table
  itself is annotated "not implemented yet".
- Nothing else in the core reads that group. Linear and arc motion therefore run
  on the **programmed** path while the operator believes the tool nose radius is
  being compensated. That silence is the most dangerous part of the current
  state: an active `G41`/`G42` is accepted and ignored.
- Canned cycles do reject it (`cutter_radius_compensation != G40` returns
  `STATUS_GCODE_CANNED_CYCLE_INVALID_RADIUSCOMPMODE`), so milling-style cycles
  fail loudly rather than cutting wrong.
- G7x does not look at the group at all; the LinuxCNC reference this module was
  derived from refuses G7x cycles with compensation enabled.
- Lathe features are off by default: `ENABLE_LATHE` is commented out in
  `cnc_config.h`.

## What "proper lathe mode" has to mean here

Radius compensation is a **path** transformation, so it belongs in the
parser/path stage, not in the realtime interpolator: by the time motion reaches
the planner the compensated geometry must be final. Two consequences follow.

1. Diameter mode is the normal lathe case. With `G7`, `X` words are diameters
   while the tool nose radius is a real length. The offset is a radius in X;
   every offset must be applied in radius space and converted back, or the part
   comes out half-size in X. `G8` is the radius case and needs no such
   conversion.
2. The side of the tool is not a lathe operator's choice. On an OD turn the tool
   sits on the +X side of the work, on a bore it is inside; which of G41/G42 puts
   the tool on the *programmed* left or right therefore depends on the cut
   direction and the tool orientation quadrant. The tool table already exists
   (NC `.t` files, `nc_tools`), and a real implementation needs the nose radius
   plus the orientation quadrant from there (or from settings for a one-tool
   machine).

## Required pieces

1. **Never ignore the group.** If compensation is requested and this build cannot
   apply it (feature off, no tool radius known, unsupported move type), refuse
   the block with a specific status instead of cutting the uncompensated path.
   This is the safety half of the feature and it is stage 1.
2. **Offset geometry**: parallel offset of lines and arcs by the nose radius,
   with proper intersection solving at convex and concave corners, in radius
   space, plus the diameter conversion for G7.
3. **Entry and exit**: lead-in and lead-out moves, the point where compensation
   becomes/shuts off active, and what happens on G40 mid-contour.
4. **Gouging and self-intersection checks**: an inner corner whose radius cannot
   be cut with the tool, or an offset path that crosses itself, must be reported
   (the corner-fit rule added to G7x for editor messages is the same class of
   check).
5. **Units and offsets**: G20/G21 scaling, work offsets, and G92 interaction must
   be applied before the offset.
6. **Cycles**: two honest options. Stage 1 follows the LinuxCNC precedent and
   refuses G71/G72/G76 with compensation active, with a clear status. Later, the
   contour can be offset once and handed to the cycle generator, so roughing and
   finishing both work from the compensated profile. Threading should stay
   uncompensated by definition: the thread is defined by pitch and crest.

## Interaction with G7x

G7x owns the contour and cycle semantics, so the compensation rule must be
stated there too: which cycle accepts a compensated contour, and which refuses.
Nothing in G7x should implement compensation itself - it consumes the compensated
profile or refuses, exactly like NC consumes G7x and never re-implements it
(`AGENTS.md` rule 4).

## Staged plan

| Stage | Content | Verified by |
| --- | --- | --- |
| S1 | Parse G41/G42 honestly: refuse when the build cannot compensate (feature off, no radius, unsupported move), keep cycles refusing, and report the reason through the NC message channel | host parser suite, plus a bench check that a program with G41 stops instead of cutting |
| S2 | Straight-line compensation, radius space, corner intersections, G7/G8 conversion, lead-in/lead-out | host suite asserting planner targets for known contours |
| S3 | Arcs, including inner corners and the "radius too small for the corner" report | host suite plus a test part |
| S4 | Gouging/self-intersection detection and final G40 exit semantics | bench test part with OD and ID cuts |
| S5 | Cycles: offset a contour once and feed it to G71/G72; keep G76 uncompensated | host suite + bench |

## Open decisions

- Where the nose radius and orientation come from: the NC tool table (`.t`,
  `nc_tools`) or machine settings. The tool table is the better home because a
  program already references tools by `Tn`; a one-tool machine can fall back to
  a setting.
- Whether to lift `ENABLE_LATHE` in `cnc_config.h` for the LVDS profile now, so
  the lathe paths are compiled in even before compensation lands.
- G41/G42 side convention to document: state the machine's convention (tool on
  the +X side for OD) and the resulting mapping explicitly, because every
  operator will assume their own.
