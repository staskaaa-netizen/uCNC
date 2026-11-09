#include "../g7x_contour.h"
#include "../g7x_blocks.h"
#include "../g7x_source.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct {
    char lines[256][96];
    int count;
} sink_t;

static int collect_g7x(g7x_stream_t *stream, sink_t *sink)
{
    char out[96];
    g7x_step_result_t step;

    memset(sink, 0, sizeof(*sink));
    while ((step = g7x_stream_next(stream, out, sizeof(out))) == G7X_STEP_LINE) {
        if (sink->count >= 256) {
            printf("FAIL too many G7x lines\n");
            return 1;
        }
        strncpy(sink->lines[sink->count], out, sizeof(sink->lines[sink->count]) - 1u);
        sink->count++;
    }
    if (step != G7X_STEP_DONE) {
        printf("FAIL G7x stream ended with %d\n", (int)step);
        return 1;
    }
    return 0;
}

static int collect_g76(g7x_thread_stream_t *stream, sink_t *sink)
{
    char out[96];
    g7x_step_result_t step;

    memset(sink, 0, sizeof(*sink));
    while ((step = g7x_thread_next(stream, out, sizeof(out))) == G7X_STEP_LINE) {
        if (sink->count >= 256) {
            printf("FAIL too many G76 lines\n");
            return 1;
        }
        strncpy(sink->lines[sink->count], out, sizeof(sink->lines[sink->count]) - 1u);
        sink->count++;
    }
    if (step != G7X_STEP_DONE) {
        printf("FAIL G76 stream ended with %d\n", (int)step);
        return 1;
    }
    return 0;
}

static int contains(const sink_t *sink, const char *needle)
{
    int i;

    for (i = 0; i < sink->count; i++)
        if (strstr(sink->lines[i], needle))
            return 1;
    return 0;
}

static int has_exact(const sink_t *sink, const char *line)
{
    int i;

    for (i = 0; i < sink->count; i++)
        if (strcmp(sink->lines[i], line) == 0)
            return 1;
    return 0;
}

static int add_line(g7x_stream_t *stream, const char *line)
{
    bool done = false;
    g7x_result_t result = g7x_stream_add_line(stream, line, &done);

    if (result != G7X_OK) {
        printf("FAIL add %s result=%s\n", line, g7x_result_text(result));
        return 1;
    }
    return 0;
}

static int test_allowance_approach(void)
{
    /* Diameter source coordinates: X allowance 0.5 and radial R1 produce
       X52.5 clearance. Z allowance 0.5 plus R1 produces +/-1.5 clearance.

       What is checked is where the cycle stands, not which lines it prints: a
       rapid that would not move the tool is never emitted, each pass retracts
       in X only (the OD pass returns in Z because it cuts from the face end
       again, the facing pass does not), and the cycle ends on the X retract
       followed by the Z return to the clearance point - the corner a `G0`
       before the cycle establishes, and where Fanuc leaves the tool after a
       stock-removal cycle.

       And the roughing has to leave **exactly** the finish allowance: the last
       rough pass steps onto the boundary instead of stopping a whole step short
       of it (that step used to become the automatic finish cut). With X0.5
       diameter allowance over an X50 to X30 profile the boundary is X30.5, so
       that pass has to be in the stream. */
    for (int cycle = 71; cycle <= 72; cycle++) {
        for (int direction = -1; direction <= 1; direction += 2) {
            g7x_stream_t stream;
            sink_t sink;
            float x = 0.0f;
            float z = 0.0f;
            float finish_x = 0.0f;
            float finish_z = 0.0f;
            bool have = false;
            bool finish_seen = false;
            bool finish_have = false;
            int idle = 0;
            const char *header = cycle == 71 ? "G71 U2 R1 X0.5 Z0.5 F120" :
                                                "G72 W2 R1 X0.5 Z0.5 F120";
            if (g7x_stream_begin(&stream, header) != G7X_OK ||
                add_line(&stream, "G1 X50 Z0") ||
                add_line(&stream, direction < 0 ? "G1 X30 Z-10" : "G1 X30 Z10") ||
                add_line(&stream, "G80") || collect_g7x(&stream, &sink)) return 1;
            const char *zclear = direction < 0 ? "G0 Z1.500" : "G0 Z-1.500";
            float zclear_value = direction < 0 ? 1.5f : -1.5f;
            if (sink.count < 5 || strcmp(sink.lines[2], "G0 X52.500") ||
                strcmp(sink.lines[3], zclear)) {
                printf("FAIL G%d approach direction %d\n", cycle, direction); return 1;
            }
            for (int i = 0; i < sink.count; i++) {
                float nx = x;
                float nz = z;
                const char *px;
                const char *pz;
                bool moved = false;

                if (!strncmp(sink.lines[i], "G0 ", 3) &&
                    strchr(sink.lines[i], 'X') && strchr(sink.lines[i], 'Z')) {
                    puts("FAIL diagonal rapid in cycle"); return 1;
                }
                if (!strcmp(sink.lines[i], "(G7x finish contour)")) {
                    finish_seen = true;
                    continue;
                }
                if (sink.lines[i][0] != 'G')
                    continue;
                /* Where the finish *cut* starts: the rapid approach before it
                   (when one is needed) has already been replayed above. */
                if (finish_seen && !finish_have && sink.lines[i][1] != '0') {
                    finish_x = x;
                    finish_z = z;
                    finish_have = true;
                }
                px = strchr(sink.lines[i], 'X');
                pz = strchr(sink.lines[i], 'Z');
                if (px && sscanf(px + 1, "%f", &nx) == 1 &&
                    (!have || fabsf(nx - x) > 0.0005f))
                    moved = true;
                if (pz && sscanf(pz + 1, "%f", &nz) == 1 &&
                    (!have || fabsf(nz - z) > 0.0005f))
                    moved = true;
                if (have && !moved)
                    idle++;
                x = nx;
                z = nz;
                have = true;
            }
            if (idle) {
                printf("FAIL G%d emits %d rapid(s) that do not move\n", cycle, idle);
                return 1;
            }
            if (!finish_have) {
                puts("FAIL no finish contour"); return 1;
            }
            /* The last rough pass is on the allowance boundary. */
            if (cycle == 71) {
                if (!has_exact(&sink, "G1 X30.500 F120.000")) {
                    puts("FAIL G71 does not leave the X allowance"); return 1;
                }
            } else if (!has_exact(&sink, direction < 0 ? "G0 Z-9.500" : "G0 Z9.500")) {
                puts("FAIL G72 does not leave the Z allowance"); return 1;
            }
            /* The finish cut starts from the clearance point, whether the
               generator had to move there or was already there. */
            if (fabsf(finish_x - 52.5f) > 0.0005f ||
                fabsf(finish_z - zclear_value) > 0.0005f) {
                printf("FAIL finish starts at X%.3f Z%.3f, wanted X52.500 Z%.3f\n",
                       (double)finish_x, (double)finish_z, (double)zclear_value);
                return 1;
            }
            /* And it ends back at the clearance point: out in X first, then the
               Z return, which is where the program's `G0` before the cycle put
               the tool. */
            if (sink.count < 2 || strcmp(sink.lines[sink.count - 2], "G0 X52.500") ||
                strcmp(sink.lines[sink.count - 1], zclear)) {
                printf("FAIL G%d ends on \"%s\" / \"%s\", wanted the clearance "
                       "point\n",
                       cycle,
                       sink.count > 1 ? sink.lines[sink.count - 2] : "",
                       sink.count > 0 ? sink.lines[sink.count - 1] : "");
                return 1;
            }
            if (fabsf(x - 52.5f) > 0.0005f ||
                fabsf(z - zclear_value) > 0.0005f) {
                puts("FAIL the cycle does not end at the clearance point");
                return 1;
            }
        }
    }
    return 0;
}

static int test_g71_basic(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X40 Z-10") ||
        add_line(&stream, "G1 X30 Z-25") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G71 rough X23.000)") ||
        !contains(&sink, "(G7x finish contour)") ||
        !has_exact(&sink, "G1 X50.000 Z0.000 F120.000") ||
        !has_exact(&sink, "G1 X30.000 Z-25.000")) {
        printf("FAIL G71 basic output\n");
        return 1;
    }
    return 0;
}

static int test_g72_basic(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G72 W2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X10 Z-25") ||
        add_line(&stream, "G1 X40 Z-25 C0 R0") ||
        add_line(&stream, "G1 X40 Z0") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G72 rough Z") ||
        !contains(&sink, "(G7x finish contour)") ||
        !has_exact(&sink, "G1 X10.000 Z-25.000 F120.000") ||
        !has_exact(&sink, "G1 X40.000 Z0.000")) {
        printf("FAIL G72 basic output\n");
        return 1;
    }
    return 0;
}

/* G70 is the finish cut of a contour the caller already holds: the profile as
   programmed, at the feed the rows carry, with nothing offset and no roughing
   at all. A contour that doubles back is fine here - parallel passes cannot
   clear a V, but a finish cut only follows the line the program describes. */
static int test_g70_finish_only(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G70 P100 Q200 F0.3") != G7X_OK)
        return 1;
    if (add_line(&stream, "N100 G0 X52 Z4") ||          /* the P block */
        add_line(&stream, "N110 G1 X50 Z0") ||
        add_line(&stream, "N120 G1 X50 Z-10 F180") ||   /* the profile's feed */
        add_line(&stream, "N130 G1 X40 Z-10") ||
        add_line(&stream, "N200 G1 X36 Z-4") ||         /* doubles back in Z */
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (contains(&sink, "(G71 rough") || contains(&sink, "(G72 rough")) {
        puts("FAIL G70 emitted roughing passes");
        return 1;
    }
    if (!has_exact(&sink, "G0 X52.000 Z4.000")) {
        puts("FAIL G70 did not rapid to the profile's start");
        return 1;
    }
    if (!has_exact(&sink, "G1 X50.000 Z0.000 F0.300")) {
        puts("FAIL G70 did not take the cycle feed");
        return 1;
    }
    if (!has_exact(&sink, "G1 X50.000 Z-10.000 F180.000")) {
        puts("FAIL G70 did not take the row's own feed");
        return 1;
    }
    if (!has_exact(&sink, "G1 X36.000 Z-4.000")) {
        puts("FAIL G70 did not follow the profile");
        return 1;
    }
    return 0;
}

/* The `P` block of a numbered range: written as a `G0` it is Fanuc's
   positioning move, so the cycle rapids to that point and the cut starts with
   the row after it. A profile row's own `F` is the *finish* feed - the roughing
   runs at the cycle header's feed - and it is emitted where the program wrote
   it, not once for the whole finish pass. A rapid *later* in the profile is
   followed as a cut: a cycle never puts a rapid through the material. */
static int test_profile_feed_and_approach(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G0 X52 Z4") ||            /* the `P` block: approach */
        add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X50 Z-10 F180") ||     /* the profile's feed */
        add_line(&stream, "G0 X40 Z-10") ||          /* a rapid inside: a cut */
        add_line(&stream, "G1 X30 Z-25") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G0 X52.000 Z4.000")) {
        puts("FAIL the P block was not rapid to");
        return 1;
    }
    if (!has_exact(&sink, "G1 X50.000 Z0.000 F120.000")) {
        puts("FAIL the cut after the P block did not carry the finish feed");
        return 1;
    }
    if (!has_exact(&sink, "G1 X50.000 Z-10.000 F180.000")) {
        puts("FAIL the profile's own feed was not emitted");
        return 1;
    }
    if (has_exact(&sink, "G0 X40.000 Z-10.000")) {
        puts("FAIL a rapid inside the profile was emitted as a rapid");
        return 1;
    }
    return 0;
}

/* A block the generator made carries no source line of its own; a profile row
   carries its own. Every sender reads the difference: it is how the pane knows
   that a cycle's own rapids belong to the cycle and not to the program's first
   line (bench, 2026-09-27 - the mark held line 1 while a cycle ran, and a fault
   on one of those moves left the operator on a stock line `1 SINGLE` cannot
   run). The rows' own lines are what the sender feeds as `pending_source_line`,
   so that is what this sets. */
/* The cutter radius compensation a cycle consumes: with the tool's nose radius
   asked for (`g7x_stream_set_comp()`), the profile the cycle cuts is the
   *offset* one - every line moved by the radius - and without it the program is
   cut as written. The geometry is `lathe_comp`'s; this checks that the cycle
   actually consumes it. */
static int test_cutter_comp_offset(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    /* One millimetre of nose radius, the tool on the left of the path: the OD
       the tool's centre follows is 1 mm out of the programmed X50, which is
       2 mm in the diameter the generator writes out. The cycle takes the shift
       when the contour is prepared - at the `G80` below - which is why this is
       asked for before the profile's rows are fed. */
    g7x_stream_set_comp(&stream, G7X_COMP_LEFT, 1.0f);
    if (add_line(&stream, "G0 X52 Z4") ||            /* the `P` block */
        add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X50 Z-10 F180") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G1 X52.000 Z-10.000 F180.000")) {
        int dump;

        for (dump = 0; dump < sink.count; dump++) {
            printf("  comp line %d: %s\n", dump, sink.lines[dump]);
        }
        puts("FAIL the compensated cycle did not move the OD out by the nose "
             "radius");
        return 1;
    }
    if (has_exact(&sink, "G1 X50.000 Z-10.000 F180.000")) {
        puts("FAIL the compensated cycle still cut the programmed X");
        return 1;
    }
    /* The same profile without compensation is the program, unchanged. */
    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G0 X52 Z4") ||
        add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X50 Z-10 F180") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G1 X50.000 Z-10.000 F180.000")) {
        puts("FAIL an uncompensated cycle moved the profile");
        return 1;
    }
    return 0;
}

/* The allowance a *compensated* cycle carries: the roughing passes have to
   leave the allowance **outside** the surface the finish cut will make. With
   the nose radius asked for and an allowance in the header, every roughing
   pass must stay at or outside the path the finish cut follows - a pass inside
   it cuts the surface the finish cut is *for*.

   Recreates the bench's red alert of 2026-10-04: *"for x20 in diameter mode its
   wants an x13 for r3. but now it is only x12. so 1 mm below needed depth. for
   g70 it is ok for g71 it is bad"*. X20 in a diameter program is radius 10; a
   3 mm nose puts the tool's centre path at radius 13, and a roughing pass at
   radius 12 is a millimetre inside the finished surface. */
static int test_cutter_comp_allowance(void)
{
    g7x_stream_t stream;
    sink_t sink;
    float finish_x = 1.0e9f;
    int i;
    int failures = 0;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X1 Z1 F120") != G7X_OK)
        return 1;
    g7x_stream_set_comp(&stream, G7X_COMP_LEFT, 3.0f);
    if (add_line(&stream, "G0 X22 Z4") ||            /* the `P` block */
        add_line(&stream, "G1 X20 Z0") ||
        add_line(&stream, "G1 X20 Z-10 F180") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;

    /* The finish contour the cycle emits ends the cycle: its own X values are
       the tool-centre path (diameters on the wire, so a radius is half). The
       smallest of them is the surface the allowance is measured from. */
    for (i = 0; i < sink.count; i++) {
        float x;

        if (sscanf(sink.lines[i], "G1 X%f", &x) == 1 && x * 0.5f < finish_x) {
            finish_x = x * 0.5f;
        }
    }
    if (finish_x > 1.0e8f) {
        puts("FAIL the compensated cycle emitted no finish contour");
        return 1;
    }
    /* Every roughing level the cycle announced must be at or outside that, and
       the *last* one must leave exactly the allowance the program wrote - in
       the program's own frame. `X1` in a **diameter** program is one millimetre
       across, half a millimetre in the radius both the region and the roughing
       levels live in; the uncompensated cycle this file has always checked
       (`test_allowance_approach`) leaves its last pass at exactly that. So a
       compensated cycle's last roughing level is the tool-centre path plus
       0.5 - and 14.0 here is the doubled allowance, the millimetre the bench
       saw. */
    for (i = 0; i < sink.count; i++) {
        float level;

        if (sscanf(sink.lines[i], "(G71 rough X%f", &level) == 1) {
            if (level < finish_x - 0.001f) {
                printf("FAIL a roughing pass at X%.3f is inside the finish "
                       "path's own X%.3f (radius) - the allowance has to be "
                       "*outside* it\n", (double)level, (double)finish_x);
                failures++;
            }
            if (level > finish_x + 0.5f + 0.01f) {
                printf("FAIL a roughing pass at X%.3f leaves more than the "
                       "allowance the program wrote: X1 in a diameter program "
                       "is half a millimetre in radius, so the last pass "
                       "belongs at X%.3f\n", (double)level,
                       (double)(finish_x + 0.5f));
                failures++;
            }
        }
    }
    if (!failures) {
        printf("PASS the compensated cycle's roughing stays outside its own "
               "finish path (radius %.3f)\n", (double)finish_x);
    }
    return failures ? 1 : 0;
}

static int test_generated_block_source_line(void)
{
    g7x_stream_t stream;
    char out[96];
    g7x_step_result_t step;
    int generated = 0;              /* motions that are no row of the program */
    int rows = 0;                   /* motions that are the row they cut */
    int guard = 0;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    stream.pending_source_line = 0u;
    if (add_line(&stream, "G0 X52 Z4"))          /* the `P` block: a rapid */
        return 1;
    stream.pending_source_line = 1u;
    if (add_line(&stream, "G1 X50 Z0"))
        return 1;
    stream.pending_source_line = 2u;
    if (add_line(&stream, "G1 X40 Z-20") ||
        add_line(&stream, "G80"))
        return 1;
    while ((step = g7x_stream_next(&stream, out, sizeof(out))) == G7X_STEP_LINE &&
           guard++ < 512) {
        /* Only the motions: a comment the generator writes carries no source
           line either, and would cover for the motions if it were counted. */
        if (out[0] != 'G' || (out[1] != '0' && out[1] != '1')) {
            continue;
        }
        if (stream.last_source_line == (size_t)-1) {
            generated++;
        } else if (stream.last_source_line <= 2u) {
            rows++;
        } else {
            printf("FAIL a step claims source line %lu: %s\n",
                   (unsigned long)(stream.last_source_line + 1u), out);
            return 1;
        }
    }
    if (step != G7X_STEP_DONE) {
        printf("FAIL the G71 stream ended with %d\n", (int)step);
        return 1;
    }
    if (!generated) {
        puts("FAIL every motion answered for a row: the generator's own - the "
             "clearance, the retracts, the changes of pass - claim the "
             "program's first line");
        return 1;
    }
    if (!rows) {
        puts("FAIL no motion kept the profile row's own line");
        return 1;
    }
    return 0;
}

static int test_g71_corner_radius(void)
{
    g7x_stream_t stream;
    sink_t sink;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X50 Z-10 R1") ||
        add_line(&stream, "G1 X40 Z-10") ||
        add_line(&stream, "G1 X40 Z-20") ||
        add_line(&stream, "G80"))
        return 1;
    if (collect_g7x(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G1 X50.000 Z-9.000") ||
        !contains(&sink, "G3 X48.000 Z-10.000 I-1.000 K0.000")) {
        printf("FAIL G71 radius corner output\n");
        return 1;
    }
    return 0;
}

static int test_bad_contour_rejected(void)
{
    g7x_stream_t stream;
    bool done = false;

    if (g7x_stream_begin(&stream, "G71 U2 R1 X0.5 Z0.5 F120") != G7X_OK)
        return 1;
    if (add_line(&stream, "G1 X50 Z0") ||
        add_line(&stream, "G1 X25 Z-25") ||
        add_line(&stream, "G1 X40 Z-10"))
        return 1;
    if (g7x_stream_add_line(&stream, "G80", &done) != G7X_UNSUPPORTED) {
        printf("FAIL non-monotonic contour accepted\n");
        return 1;
    }
    return 0;
}

static int test_g76_basic(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X40 X36 Z-20 P2 Q1 F1.5 H1 MIN_Q1",
                         0.0f,
                         2.0f) != G7X_OK) {
        printf("FAIL G76 begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G76 FANUC D 40.000 X 36.000 F 1.500 P 2.000 Q 1.000 R 0.000") ||
        !has_exact(&sink, "G0 X42.000 Z1.500") ||
        !contains(&sink, "(THREAD pass 2 X36.000") ||
        !has_exact(&sink, "G33 X36.000 Z-20.000 K1.500") ||
        !contains(&sink, "(THREAD spring X36.000") ||
        !has_exact(&sink, "G0 Z1.500")) {
        printf("FAIL G76 basic output\n");
        return 1;
    }
    return 0;
}

static int test_g76_taper_id(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X24 Z10 Z1=0 P2 Q1 F2 D1 MIN_Q1",
                         0.0f,
                         1.0f) != G7X_OK) {
        printf("FAIL G76 ID/taper begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!has_exact(&sink, "G0 X19.000 Z-2.000") ||
        !contains(&sink, "(THREAD pass 2 X24.000 Z0.000)") ||
        !has_exact(&sink, "G33 X25.000 Z10.000 K2.000")) {
        printf("FAIL G76 ID/taper output\n");
        return 1;
    }
    return 0;
}

static int test_g76_letters(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X18 Z-20 P1.0 Q0.3 MIN_Q0.05 FINISH_R0.02 F1.5 H2",
                         0.0f,
                         1.0f) != G7X_OK) {
        printf("FAIL G76 semantic begin\n");
        return 1;
    }
    if (collect_g76(&stream, &sink))
        return 1;
    if (!contains(&sink, "(G76 FANUC D 20.000 X 18.000 F 1.500 P 1.000 Q 0.050 R 0.020") ||
        !has_exact(&sink, "G0 X21.000 Z1.500") ||
        !contains(&sink, "(THREAD finish X18.000") ||
        !contains(&sink, "(THREAD spring X18.000") ||
        !has_exact(&sink, "G33 X18.000 Z-20.000 K1.500")) {
        printf("FAIL G76 semantic output\n");
        return 1;
    }
    return 0;
}

static int test_g76_semantic_invalid_pitch_depth(void)
{
    g7x_thread_stream_t stream;

    if (g7x_thread_begin(&stream,
                         "G76 START_X20 X18 Z-20 P0 Q0.3 F0",
                         0.0f,
                         1.0f) != G7X_BAD_FIELD) {
        printf("FAIL G76 invalid pitch/depth accepted\n");
        return 1;
    }
    return 0;
}

static int test_g76_decreasing_schedule(void)
{
    g7x_thread_stream_t stream;
    sink_t sink;
    if (g7x_thread_begin_semantic(&stream, 40, 36, 0, -20, 1.5f,
                                 2, 1, .25f, .1f, 1, 0, 1, 0, 0) != G7X_OK ||
        collect_g76(&stream, &sink)) return 1;
    unsigned cuts = 0;
    for (int i = 0; i < sink.count; i++)
        if (strncmp(sink.lines[i], "G33 ", 4) == 0) cuts++;
    if (cuts != 5 || !contains(&sink, "(THREAD pass 1 X38.000") ||
        !contains(&sink, "(THREAD pass 2 X36.500") ||
        !contains(&sink, "(THREAD pass 3 X36.200") ||
        !contains(&sink, "(THREAD finish X36.000") ||
        !contains(&sink, "(THREAD spring X36.000")) {
        puts("FAIL first/decreasing/minimum/finish/spring schedule"); return 1;
    }
    return 0;
}

static int test_g76_invalid_contract(void)
{
    g7x_thread_stream_t stream;
    const char *bad[] = {
        "G76 START_X40 X36 Z-20 P1 Q1 F1.5", /* inconsistent height */
        "G76 START_X40 X36 Z-20 P2 Q1 Fnan",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 MIN_Q2",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 L1.5",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 L501",
        "G76 START_X40 X36 Z-20 P2 Q1 F1.5 I-30"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (g7x_thread_begin(&stream, bad[i], 0, 1) != G7X_BAD_FIELD) {
            printf("FAIL accepted %s\n", bad[i]); return 1;
        }
    }
    if (g7x_thread_begin_semantic(&stream, 40, 36, 0, -20, INFINITY,
                                2, 1, .25f, 0, 1, 0, 0, 0, 0) != G7X_BAD_FIELD)
        return 1;
    return 0;
}

/* Fanuc two-line header: depth and retract on the first block, range and finish
   allowances in U/W on the second. It must generate exactly the same cycle as
   the project's one-line spelling of the same geometry. */
static int test_two_line_header(void)
{
    static const char *first[2] = { "G71 U2 R1", "G72 W2 R1" };
    static const char *second[2] = { "G71 P100 Q200 U0.5 W0.25 F120",
                                     "G72 P100 Q200 U0.5 W0.25 F120" };
    static const char *single[2] = { "G71 U2 R1 X0.5 Z0.25 F120",
                                     "G72 W2 R1 X0.5 Z0.25 F120" };
    g7x_stream_t linked;
    g7x_stream_t plain;
    sink_t a;
    sink_t b;
    int cycle;
    int i;

    for (cycle = 0; cycle < 2; cycle++) {
        g7x_stream_reset(&linked);
        g7x_stream_reset(&plain);
        if (g7x_stream_begin_linked(&linked, first[cycle], second[cycle]) != G7X_OK ||
            g7x_stream_begin(&plain, single[cycle]) != G7X_OK)
            return 1;
        for (i = 0; i < 2; i++) {
            g7x_stream_t *stream = i == 0 ? &linked : &plain;
            if (add_line(stream, "N100 G1 X50 Z0") ||
                add_line(stream, "G1 X50 Z-10") ||
                add_line(stream, "N200 G1 X40 Z-10") ||
                add_line(stream, "G80"))
                return 1;
        }
        if (collect_g7x(&linked, &a) || collect_g7x(&plain, &b))
            return 1;
        if (a.count != b.count) {
            printf("FAIL two-line stream count %d vs %d\n", a.count, b.count);
            return 1;
        }
        for (i = 0; i < a.count; i++) {
            if (strcmp(a.lines[i], b.lines[i])) {
                printf("FAIL two-line [%s] vs [%s]\n", a.lines[i], b.lines[i]);
                return 1;
            }
        }
        /* X 50 + U0.5 + R1 is a diameter of 52.5; Z 0 + W0.25 + R1 clears 1.25. */
        if (!has_exact(&a, "G0 X52.500") || !has_exact(&a, "G0 Z1.250")) {
            printf("FAIL two-line clearance cycle %d\n", cycle);
            return 1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t numbers[4];
    const char *texts[4];
    unsigned count;
} fake_source_t;

static g7x_source_status_t fake_source_next(void *user,
                                            const g7x_source_pos_t *from,
                                            g7x_source_pos_t *out,
                                            char *text,
                                            size_t text_sz)
{
    const fake_source_t *source = user;
    unsigned i;

    for (i = (unsigned)from->line_index; i < source->count; i++) {
        if (source->numbers[i] < from->number)
            continue;
        snprintf(text, text_sz, "%s", source->texts[i]);
        out->number = source->numbers[i];
        out->line_index = (size_t)i + 1u;
        return G7X_SOURCE_OK;
    }
    return G7X_SOURCE_MISSING;
}

typedef struct {
    uint32_t seen[8];
    unsigned count;
} range_log_t;

static void log_range_block(void *user, uint32_t number, const char *text)
{
    range_log_t *log = user;

    (void)text;
    if (log->count < 8)
        log->seen[log->count] = number;
    log->count++;
}

/* Bounded serial retention: lookup by number, ordered range walk, and explicit
   failures for missing, ambiguous and evicted blocks. */
static int test_numbered_history(void)
{
    g7x_history_t history;
    range_log_t log;
    char long_text[G7X_RETAINED_TEXT_LEN + 8];
    unsigned visited = 0;
    unsigned i;

    g7x_history_reset(&history);
    if (g7x_history_find(&history, 100) ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_RANGE_MISSING ||
        g7x_history_add(NULL, 1, "x") != G7X_BAD_FIELD ||
        g7x_history_add(&history, 1, "") != G7X_BAD_FIELD)
        return 1;

    memset(long_text, 'G', sizeof(long_text) - 1u);
    long_text[sizeof(long_text) - 1u] = '\0';
    if (g7x_history_add(&history, 99, long_text) != G7X_WRITE_FAILED)
        return 1;

    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK)
        return 1;
    if (!g7x_history_find(&history, 100) ||
        !g7x_history_find(&history, 200) || g7x_history_find(&history, 300) ||
        strcmp(g7x_history_find(&history, 150), "N150 G1 Z-10"))
        return 1;

    memset(&log, 0, sizeof(log));
    if (g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_OK ||
        visited != 3 || log.count != 3 || log.seen[0] != 100 ||
        log.seen[1] != 150 || log.seen[2] != 200) {
        printf("FAIL history range walk visited=%u\n", visited);
        return 1;
    }
    if (g7x_history_visit_range(&history, 100, 300, log_range_block, &log,
                                &visited) != G7X_RANGE_MISSING ||
        g7x_history_visit_range(&history, 200, 100, log_range_block, &log,
                                &visited) != G7X_RANGE_AMBIGUOUS ||
        g7x_history_visit_range(&history, 100, 200, NULL, &log,
                                &visited) != G7X_BAD_FIELD)
        return 1;

    /* A repeated number inside the range is ambiguous, not "the last one". */
    g7x_history_reset(&history);
    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 175, "N175 G1 X45") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-12") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_RANGE_AMBIGUOUS)
        return 1;
    /* A repeated number after the Q block is outside the range and allowed. */
    g7x_history_reset(&history);
    if (g7x_history_add(&history, 100, "N100 G0 X50 Z0") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-10") != G7X_OK ||
        g7x_history_add(&history, 200, "N200 G1 X40") != G7X_OK ||
        g7x_history_add(&history, 150, "N150 G1 Z-12") != G7X_OK ||
        g7x_history_visit_range(&history, 100, 200, log_range_block, &log,
                                &visited) != G7X_OK || visited != 3)
        return 1;

    /* Eviction is bounded and must not resolve dropped blocks by accident. */
    g7x_history_reset(&history);
    for (i = 0; i < G7X_MAX_RETAINED_BLOCKS + 2u; i++) {
        char text[G7X_RETAINED_TEXT_LEN];
        snprintf(text, sizeof(text), "N%u G1 X1", 1000u + i);
        if (g7x_history_add(&history, 1000u + i, text) != G7X_OK)
            return 1;
    }
    if (!g7x_history_evicted(&history) ||
        g7x_history_find(&history, 1000) ||
        !g7x_history_find(&history, 1000u + G7X_MAX_RETAINED_BLOCKS + 1u) ||
        g7x_history_visit_range(&history, 1000, 1000u + G7X_MAX_RETAINED_BLOCKS + 1u,
                                log_range_block, &log, &visited) != G7X_RANGE_MISSING)
        return 1;
    return 0;
}

/* The caller-supplied cursor is the only random-access path into program text;
   G7x itself must report a missing source instead of guessing. */
static int test_numbered_source(void)
{
    fake_source_t source = {
        { 100u, 150u, 200u },
        { "N100 G0 X50 Z0", "N150 G1 Z-10", "N200 G1 X40" },
        3u
    };
    g7x_source_t cursor = { fake_source_next, &source };
    g7x_source_pos_t pos = { 0u, 0u };
    g7x_source_pos_t found = { 0u, 0u };
    char text[48];
    const uint32_t want[3] = { 100u, 150u, 200u };
    unsigned i;

    if (g7x_source_next(NULL, &pos, &found, text, sizeof(text)) != G7X_SOURCE_ERROR)
        return 1;
    for (i = 0; i < 3u; i++) {
        if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_OK ||
            found.number != want[i] || strcmp(text, source.texts[i]))
            return 1;
        pos = found;
    }
    if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_MISSING)
        return 1;

    /* Starting mid-document resumes at the requested line index. */
    pos.number = 0u;
    pos.line_index = 1u;
    if (g7x_source_next(&cursor, &pos, &found, text, sizeof(text)) != G7X_SOURCE_OK ||
        found.number != 150u)
        return 1;
    return 0;
}

/* The document side of the cycle rules: the same program the stream tests use,
   but read as a *file* - which block a row is in, which rows are a contour, and
   the range a finish cut names above it. The lines come from a provider, not
   from any module's document type, which is the point of the interface: a
   screen with its own buffer asks the same questions the generator answers. */
static const char *const g_doc_program[] = {
    "G0 X52 Z2",
    "G71 U1 R0.2 X0.5 Z0.5 F450 P10 Q20",
    "N10 G1 X30 Z0",
    "G1 X30 Z-15 C0 R0",
    "G1 X35 Z-25 C0 R5",
    "N20 G1 X50 Z-25",
    "G80",
    "G70 P10 Q20",
    /* And a Fanuc pair, with its rows under it. */
    "G71 U1 R0.5 X0.5 Z0.5",
    "G71 P100 Q200 F450",
    "N100 G1 X40 Z0",
    "N200 G1 X55 Z-10",
    "G80",
    /* And a cycle whose range never closes. */
    "G72 U1 R0.5 X0.5 Z0.5",
    "G72 P300 Q400 F450",
    "N300 G1 X30 Z-5"
};

static const char *doc_provide(void *user, size_t index)
{
    const char *const *program = user;
    size_t count = sizeof(g_doc_program) / sizeof(g_doc_program[0]);

    (void)count;
    return program[index];
}

static g7x_doc_t doc_for(const char *const *program, size_t count)
{
    g7x_doc_t doc;

    doc.line = doc_provide;
    doc.user = (void *)program;
    doc.count = count;
    return doc;
}

static int test_document_blocks(void)
{
    g7x_doc_t doc = doc_for(g_doc_program,
                            sizeof(g_doc_program) / sizeof(g_doc_program[0]));
    size_t count = sizeof(g_doc_program) / sizeof(g_doc_program[0]);
    size_t start = 0u;
    size_t end = 0u;
    size_t first = 0u;
    size_t last = 0u;
    int fails = 0;

    /* The G80-terminated range: its rows are the profile, the end mark belongs
       to it, and the G70 below names those rows. */
    if (!g7x_doc_block_containing(&doc, 2u, &start, &end) ||
        start != 1u || end != 6u) {
        printf("FAIL document block for a row in the cycle is %u..%u\n",
               (unsigned)start, (unsigned)end);
        fails++;
    }
    if (!g7x_doc_line_is_any_contour(&doc, 3u) ||
        g7x_doc_line_is_any_contour(&doc, 0u) ||
        g7x_doc_line_is_any_contour(&doc, 7u)) {
        printf("FAIL document contour answers are wrong\n");
        fails++;
    }
    if (!g7x_doc_range_above(&doc, 7u, 10u, 20u, &first, &last) ||
        first != 2u || last != 5u) {
        printf("FAIL document range above is %u..%u\n", (unsigned)first,
               (unsigned)last);
        fails++;
    }
    if (!g7x_doc_line_path(&doc, 7u, &first, &last) ||
        first != 2u || last != 5u) {
        printf("FAIL document line path is %u..%u\n", (unsigned)first,
               (unsigned)last);
        fails++;
    }

    /* The Fanuc pair (the two `G71` lines at 8 and 9): both answer with the block
       the first one heads, and its rows are the numbered ones under it. */
    if (g7x_doc_block_start(&doc, 9u) != 8u) {
        printf("FAIL the second line of a header pair does not map back\n");
        fails++;
    }
    if (!g7x_doc_block_containing(&doc, 10u, &start, &end) ||
        start != 8u || end != 12u) {
        printf("FAIL the pair's block is %u..%u\n", (unsigned)start,
               (unsigned)end);
        fails++;
    }

    /* And a range that never closes is refused, not guessed. */
    if (g7x_doc_block_end(&doc, 13u, &end) ||
        g7x_doc_block_containing(&doc, 15u, &start, &end)) {
        printf("FAIL an unclosed range answered\n");
        fails++;
    }

    (void)count;
    if (!fails) {
        printf("document blocks: PASS\n");
    }
    return fails;
}

/* The words a program is *on* at a line - the tool and the compensation - are
   this module's state machine, and it answers for a document it is handed, with
   no screen and no tool table anywhere: `g7x_state_at()`. */
static const char *const g_state_program[] = {
    "G971 X50 Z50 I0 E0",
    "T2",
    "M3 S1000",
    "G0 X52 Z2",
    "G41",
    "G71 U3 R1 X1 Z1 F500 P50 Q55",
    "N50 G1 X20 Z0",
    "G1 X20 Z-10",
    "N55 G1 X50 Z-10",
    "G80",
    "G42",
    "T1",
    "G71 U3 R1 X1 Z1 F500 P100 Q200",
    "N100 G1 X20 Z0",
    "N200 G1 X50 Z-10",
    "G80",
    "G40",
    "T2 M6",
    "G1 X30.000 Z-3.000 F200.000",
    "M2"
};

static int test_state_words(void)
{
    g7x_doc_t doc = doc_for(g_state_program,
                            sizeof(g_state_program) /
                                sizeof(g_state_program[0]));
    static const struct {
        size_t line;
        int tool;
        g7x_comp_side_t comp;
    } want[] = {
        { 0u, -1, G7X_COMP_OFF },        /* nothing named yet */
        { 1u, 2, G7X_COMP_OFF },         /* T2 */
        { 4u, 2, G7X_COMP_LEFT },        /* G41 */
        { 9u, 2, G7X_COMP_LEFT },        /* the first cycle's G80 */
        { 10u, 2, G7X_COMP_RIGHT },      /* G42 */
        { 11u, 1, G7X_COMP_RIGHT },      /* T1, under G42 */
        { 16u, 1, G7X_COMP_OFF },        /* G40 */
        { 17u, 2, G7X_COMP_OFF },        /* T2 on a line that carries M6 too */
        { 19u, 2, G7X_COMP_OFF }         /* M2 */
    };
    int fails = 0;
    unsigned i;

    for (i = 0u; i < sizeof(want) / sizeof(want[0]); i++) {
        g7x_state_t state = g7x_state_at(&doc, want[i].line);

        if (state.tool != want[i].tool || state.comp != want[i].comp) {
            printf("FAIL line %lu reads T%d %s, not T%d %s\n",
                   (unsigned long)(want[i].line + 1u), state.tool,
                   state.comp == G7X_COMP_LEFT ? "G41" :
                   (state.comp == G7X_COMP_RIGHT ? "G42" : "G40"),
                   want[i].tool,
                   want[i].comp == G7X_COMP_LEFT ? "G41" :
                   (want[i].comp == G7X_COMP_RIGHT ? "G42" : "G40"));
            fails++;
        }
    }
    /* The word rules themselves: a `T` that continues another word, one that is
       not a whole number and one inside a comment are not tools. */
    {
        int tool = 7;

        if (g7x_line_tool("G1 X30 Z2", &tool) ||
            g7x_line_tool("XT2", &tool) ||
            g7x_line_tool("T2.5", &tool) ||
            g7x_line_tool("(T5 rough)", &tool) ||
            tool != 7) {
            printf("FAIL a row that names no tool answered one\n");
            fails++;
        }
        if (!g7x_line_tool("T1", &tool) || tool != 1 ||
            !g7x_line_tool("T2 M6", &tool) || tool != 2 ||
            !g7x_line_tool("N50 t3", &tool) || tool != 3) {
            printf("FAIL a row that names a tool was not read\n");
            fails++;
        }
    }
    /* A line past the end is the whole program, not a fault. */
    {
        g7x_state_t state = g7x_state_at(&doc, 999u);

        if (state.tool != 2 || state.comp != G7X_COMP_OFF) {
            printf("FAIL a line past the end does not read the last words\n");
            fails++;
        }
    }
    /* A word written **inside** a cycle's contour is not a word of the program:
       the generator reads those rows as the profile and no shift is applied from
       one, so the state must not count it either - otherwise a screen draws a
       shift the cut never makes. */
    {
        static const char *const inner[] = {
            "G971 X50 Z50 I0 E0",
            "T2",
            "G0 X52 Z2",
            "G42",
            "G71 U3 R1 X1 Z1 F500 P50 Q55",
            "N50 G1 X20 Z0",
            "G41",                          /* among the contour rows */
            "G1 X20 Z-10",
            "N55 G1 X50 Z-10",
            "G80",
            "G1 X60 Z5"                     /* after the block */
        };
        g7x_doc_t inner_doc = doc_for(inner, sizeof(inner) / sizeof(inner[0]));
        g7x_state_t state = g7x_state_at(&inner_doc, 9u);

        if (state.comp != G7X_COMP_RIGHT || state.tool != 2) {
            printf("FAIL a word among the contour rows shifted the program "
                   "(T%d %s at the G80)\n", state.tool,
                   state.comp == G7X_COMP_LEFT ? "G41" :
                   (state.comp == G7X_COMP_RIGHT ? "G42" : "G40"));
            fails++;
        }
    }
    if (!fails) {
        printf("state words: PASS\n");
    }
    return fails;
}

int main(void)
{
    int fails = 0;

    fails += test_g71_basic();
    fails += test_allowance_approach();
    fails += test_g72_basic();
    fails += test_profile_feed_and_approach();
    fails += test_g70_finish_only();
    fails += test_generated_block_source_line();
    fails += test_cutter_comp_offset();
    fails += test_cutter_comp_allowance();
    fails += test_g71_corner_radius();
    fails += test_bad_contour_rejected();
    fails += test_g76_basic();
    fails += test_g76_taper_id();
    fails += test_g76_letters();
    fails += test_g76_semantic_invalid_pitch_depth();
    fails += test_g76_decreasing_schedule();
    fails += test_g76_invalid_contract();
    fails += test_numbered_history();
    fails += test_numbered_source();
    fails += test_two_line_header();
    fails += test_document_blocks();
    fails += test_state_words();

    if (fails) {
        printf("FAILURES %d\n", fails);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
