#ifdef NC_HOST_TEST

#include "../nc.h"
#include "../nc_emit.h"

#include <stdio.h>
#include <string.h>

void grbl_stream_printf(const char *fmt, ...)
{
    (void)fmt;
}

static int expect_line(nc_emit_stream_t *stream, const char *want)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    do {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
    } while (got == NC_EMIT_SKIP && stream->active);

    if (got != NC_EMIT_LINE || strcmp(out, want) != 0) {
        printf("FAIL want [%s] got [%s]\n", want, got == NC_EMIT_LINE ? out : "(skip)");
        return 1;
    }
    return 0;
}

static int expect_find_line(nc_emit_stream_t *stream, const char *want)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    while (stream->active) {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
        if (got == NC_EMIT_LINE && strcmp(out, want) == 0) {
            return 0;
        }
    }

    printf("FAIL did not find [%s]\n", want);
    return 1;
}

static int expect_find_line_source(nc_emit_stream_t *stream, const char *want, size_t want_source)
{
    char out[96];
    size_t source_line;
    nc_emit_result_t got;

    while (stream->active) {
        got = nc_emit_stream_next(stream, out, sizeof(out), &source_line);
        if (got == NC_EMIT_LINE && strcmp(out, want) == 0) {
            if (source_line != want_source) {
                printf("FAIL [%s] source want %lu got %lu\n",
                       want,
                       (unsigned long)want_source,
                       (unsigned long)source_line);
                return 1;
            }
            return 0;
        }
    }

    printf("FAIL did not find [%s]\n", want);
    return 1;
}

static int expect_rejected(nc_emit_stream_t *stream)
{
    char out[96];
    size_t source;
    nc_emit_result_t r;
    do {
        r = nc_emit_stream_next(stream, out, sizeof(out), &source);
    } while (r == NC_EMIT_SKIP && stream->active);
    if (r != NC_EMIT_ERROR || stream->active || stream->error == G7X_OK) {
        printf("FAIL invalid contour was not stopped: result=%d active=%d error=%d\n",
               r, stream->active, stream->error);
        return 1;
    }
    return 0;
}

static int test_g7_g8_passthrough(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G7");
    (void)nc_insert_line(&doc, 1, "G8");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_line(&stream, "G7") ||
           expect_line(&stream, "G8");
}

static int test_g71_corner_rounding(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 R1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-9.000") ||
           expect_line(&stream, "G3 X48.000 Z-10.000 I-1.000 K0.000");
}

static int test_g71_corner_chamfer(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 C1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G7x finish contour)") ||
           expect_line(&stream, "G0 X52.500") ||
           expect_line(&stream, "G0 Z1.500") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-9.000") ||
           expect_line(&stream, "G1 X48.000 Z-10.000");
}

static int test_g71_corner_chamfer_source_line(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10 C1");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z-10");
    (void)nc_insert_line(&doc, 4, "G1 X40 Z-20");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line_source(&stream, "G1 X50.000 Z-9.000", 2) ||
           expect_find_line_source(&stream, "G1 X48.000 Z-10.000", 2);
}

static int test_g71_sample_features(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G970 X-5 U60 Z-60 W5");
    (void)nc_insert_line(&doc, 1, "G971 X50 Z50 I0 E0");
    (void)nc_insert_line(&doc, 2, "G972 C12");
    (void)nc_insert_line(&doc, 3, "G973 P7");
    (void)nc_insert_line(&doc, 4, "G71 U2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 5, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 6, "G1 X35 Z-10 C1.5");
    (void)nc_insert_line(&doc, 7, "G1 X30 Z-20 R2");
    (void)nc_insert_line(&doc, 8, "G2 X42 Z-26 R4");
    (void)nc_insert_line(&doc, 9, "G1 X50 Z-26");
    (void)nc_insert_line(&doc, 10, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    /* This profile reverses X and is outside the monotonic subset. */
    return expect_rejected(&stream);
}

static int test_g72_basic(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X10 Z-25");
    (void)nc_insert_line(&doc, 2, "G1 X40 Z-25 C0 R0");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z0");
    (void)nc_insert_line(&doc, 4, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_find_line(&stream, "(G72 rough Z-23.000)") ||
           expect_find_line(&stream, "G1 X10.000 Z-25.000 F120.000") ||
           expect_find_line(&stream, "G1 X40.000 Z-25.000") ||
           expect_find_line(&stream, "G1 X40.000 Z0.000");
}

static int test_g72_rectangle(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W1 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-5 C0 R0");
    (void)nc_insert_line(&doc, 3, "G1 X5 Z-5 C0 R0");
    (void)nc_insert_line(&doc, 4, "G1 X5 Z2");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_rejected(&stream);
}

static int test_g72_arc_finish(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W2 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X10 Z-25");
    (void)nc_insert_line(&doc, 2, "G2 X40 Z-25 R20");
    (void)nc_insert_line(&doc, 3, "G1 X40 Z0");
    (void)nc_insert_line(&doc, 4, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    /* Equal endpoint Z hides an interior arc reversal. */
    return expect_rejected(&stream);
}

static int test_g72_corner_rounding(void)
{
    nc_document_t doc;
    nc_emit_stream_t stream;

    nc_document_init(&doc);
    (void)nc_insert_line(&doc, 0, "G72 W1 R1 X0.5 Z0.5 F120");
    (void)nc_insert_line(&doc, 1, "G1 X50 Z2 C0 R0");
    (void)nc_insert_line(&doc, 2, "G1 X50 Z-10");
    (void)nc_insert_line(&doc, 3, "G1 X25 Z-5 C0 R5");
    (void)nc_insert_line(&doc, 4, "G1 X25 Z2");
    (void)nc_insert_line(&doc, 5, "G80");
    nc_emit_stream_begin(&stream, &doc, 0);

    return expect_rejected(&stream);
}

int main(void)
{
    int fails = 0;

    fails += test_g7_g8_passthrough();
    fails += test_g71_corner_rounding();
    fails += test_g71_corner_chamfer();
    fails += test_g71_corner_chamfer_source_line();
    fails += test_g71_sample_features();
    fails += test_g72_basic();
    fails += test_g72_rectangle();
    fails += test_g72_arc_finish();
    fails += test_g72_corner_rounding();
    if (fails) {
        printf("NC emit host tests failed: %d\n", fails);
        return 1;
    }
    printf("NC emit host tests passed\n");
    return 0;
}

#endif
