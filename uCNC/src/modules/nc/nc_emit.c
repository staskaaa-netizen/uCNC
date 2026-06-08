#include "nc_emit.h"

#include "../../interface/grbl_stream.h"

#include <ctype.h>
#include <stdio.h>

static const char *nc_emit_trim(const char *line)
{
    while (line && (*line == ' ' || *line == '\t')) {
        line++;
    }
    return line ? line : "";
}

static bool nc_emit_is_direct(const char *line)
{
    if (!line || !*line || *line == '(') {
        return false;
    }

    if (toupper((unsigned char)line[0]) == 'G') {
        unsigned char c1 = (unsigned char)line[1];
        unsigned char c2 = (unsigned char)line[2];

        if ((c1 == '0' || c1 == '1' || c1 == '2' || c1 == '3') ||
            ((c1 == '7' || c1 == '8') && (c2 == '\0' || isspace(c2) || c2 == '('))) {
            return true;
        }
    }
    if (toupper((unsigned char)line[0]) == 'M' ||
        toupper((unsigned char)line[0]) == 'S' ||
        toupper((unsigned char)line[0]) == 'T') {
        return true;
    }

    return false;
}

nc_emit_result_t nc_emit_source_line(const nc_document_t *doc,
                                     size_t line_index,
                                     char *out,
                                     size_t out_sz)
{
    const char *line;

    if (!out || out_sz == 0) {
        return NC_EMIT_SKIP;
    }
    out[0] = '\0';
    if (!doc || line_index >= doc->line_count) {
        return NC_EMIT_SKIP;
    }

    line = nc_emit_trim(doc->lines[line_index].text);
    if (!*line || g7x_command_is(line, "G970") ||
        g7x_command_is(line, "G971") ||
        g7x_command_is(line, "G972") ||
        g7x_command_is(line, "G973") ||
        g7x_cycle_from_line(line) != G7X_CYCLE_NONE ||
        g7x_contour_cmd_from_line(line) == G7X_CONTOUR_END) {
        return NC_EMIT_SKIP;
    }
    if (!nc_emit_is_direct(line)) {
        return NC_EMIT_SKIP;
    }

    snprintf(out, out_sz, "%s", line);
    return NC_EMIT_LINE;
}

void nc_emit_stream_begin(nc_emit_stream_t *stream,
                          const nc_document_t *doc,
                          size_t start_line)
{
    if (!stream) {
        return;
    }
    stream->doc = doc;
    stream->source_line = start_line;
    stream->active = doc && start_line < doc->line_count;
    stream->log = true;
    stream->g7x_collecting = false;
    g7x_stream_reset(&stream->g7x);
}

void nc_emit_stream_set_log(nc_emit_stream_t *stream, bool log)
{
    if (stream) {
        stream->log = log;
    }
}

size_t nc_emit_stream_line(const nc_emit_stream_t *stream)
{
    return stream ? stream->source_line : 0;
}

static nc_emit_result_t nc_emit_g7x_next(nc_emit_stream_t *stream,
                                         char *out,
                                         size_t out_sz)
{
    g7x_step_result_t step = g7x_stream_next(&stream->g7x, out, out_sz);

    if (step == G7X_STEP_LINE) {
        if (stream->log) {
            grbl_stream_printf("[MSG:NC G7X OUT %.96s]\r\n", out);
        }
        return NC_EMIT_LINE;
    }
    if (stream->log) {
        grbl_stream_printf("[MSG:NC G7X %s]\r\n", step == G7X_STEP_DONE ? "DONE" : "ERROR");
    }
    if (step == G7X_STEP_ERROR) {
        stream->active = false;
        stream->g7x_collecting = false;
        return NC_EMIT_SKIP;
    }
    if (stream->source_line >= stream->doc->line_count) {
        stream->active = false;
    }
    return NC_EMIT_SKIP;
}

static bool nc_emit_feed_g7x(nc_emit_stream_t *stream)
{
    while (stream && stream->doc && stream->source_line < stream->doc->line_count) {
        const char *line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
        bool done = false;

        stream->source_line++;
        if (g7x_stream_add_line(&stream->g7x, line, &done) != G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X ADD FAIL %.96s]\r\n", line);
            }
            g7x_stream_reset(&stream->g7x);
            stream->g7x_collecting = false;
            return false;
        }
        if (done) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X G80 line %lu]\r\n",
                                   (unsigned long)stream->source_line);
            }
            stream->g7x_collecting = false;
            return true;
        }
    }

    stream->active = false;
    stream->g7x_collecting = false;
    g7x_stream_reset(&stream->g7x);
    return false;
}

nc_emit_result_t nc_emit_stream_next(nc_emit_stream_t *stream,
                                     char *out,
                                     size_t out_sz,
                                     size_t *source_line)
{
    nc_emit_result_t result;
    const char *line;

    if (!stream || !stream->active || !stream->doc) {
        return NC_EMIT_SKIP;
    }
    if (source_line) {
        *source_line = stream->source_line;
    }

    if (stream->g7x.active) {
        return nc_emit_g7x_next(stream, out, out_sz);
    }
    if (stream->g7x_collecting && nc_emit_feed_g7x(stream)) {
        return nc_emit_g7x_next(stream, out, out_sz);
    }

    line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
    if (g7x_cycle_from_line(line) != G7X_CYCLE_NONE) {
        if (g7x_stream_begin(&stream->g7x, line) == G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X BEGIN %.96s]\r\n", line);
            }
            stream->source_line++;
            stream->g7x_collecting = true;
            if (nc_emit_feed_g7x(stream)) {
                return nc_emit_g7x_next(stream, out, out_sz);
            }
        }
        if (stream->log) {
            grbl_stream_printf("[MSG:NC G7X BEGIN/FEED FAIL %.96s]\r\n", line);
        }
        return NC_EMIT_SKIP;
    }

    result = nc_emit_source_line(stream->doc, stream->source_line, out, out_sz);
    stream->source_line++;
    if (stream->source_line >= stream->doc->line_count) {
        stream->active = false;
    }
    return result;
}
