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

    return expect_line(&stream, "(G7x finish continuous contour)") ||
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

    return expect_line(&stream, "(G7x finish continuous contour)") ||
           expect_line(&stream, "G1 X50.000 Z0.000 F120.000") ||
           expect_line(&stream, "G1 X50.000 Z-9.000") ||
           expect_line(&stream, "G1 X48.000 Z-10.000");
}

int main(void)
{
    int fails = 0;

    fails += test_g7_g8_passthrough();
    fails += test_g71_corner_rounding();
    fails += test_g71_corner_chamfer();
    if (fails) {
        printf("NC emit host tests failed: %d\n", fails);
        return 1;
    }
    printf("NC emit host tests passed\n");
    return 0;
}

#endif
