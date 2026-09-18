#include "nc_emit.h"

#include "../../interface/grbl_stream.h"

#include <ctype.h>
#include <math.h>
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
    const char *body;

    if (!out || out_sz == 0) {
        return NC_EMIT_SKIP;
    }
    out[0] = '\0';
    if (!doc || line_index >= doc->line_count) {
        return NC_EMIT_SKIP;
    }

    line = nc_emit_trim(doc->lines[line_index].text);
    /* A leading N word is a line label, not part of the command. */
    body = g7x_skip_line_number(line);
    if (!*body || g7x_command_is(body, "G970") ||
        g7x_command_is(body, "G971") ||
        g7x_command_is(body, "G972") ||
        g7x_command_is(body, "G973") ||
        g7x_cycle_from_line(body) != G7X_CYCLE_NONE ||
        g7x_contour_cmd_from_line(body) == G7X_CONTOUR_END) {
        return NC_EMIT_SKIP;
    }
    if (!nc_emit_is_direct(body)) {
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
    stream->error = G7X_OK;
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
                                         size_t out_sz,
                                         size_t *source_line)
{
    g7x_step_result_t step = g7x_stream_next(&stream->g7x, out, out_sz);

    if (step == G7X_STEP_LINE) {
        if (source_line && stream->g7x.last_source_line != (size_t)-1) {
            *source_line = stream->g7x.last_source_line;
        }
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
        stream->error = G7X_BAD_FIELD;
        return NC_EMIT_ERROR;
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
        stream->g7x.pending_source_line = stream->source_line - 1u;
        stream->error = g7x_stream_add_line(&stream->g7x, line, &done);
        if (stream->error != G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X ADD FAIL %.96s]\r\n", line);
            }
            g7x_stream_reset(&stream->g7x);
            stream->g7x_collecting = false;
            stream->active = false;
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
    stream->error = G7X_BAD_FIELD; /* Unterminated contour. */
    g7x_stream_reset(&stream->g7x);
    return false;
}

/* P/Q on the cycle header selects a numbered profile range. Returns 1 for a
   valid range, 0 when the header has no P/Q words and -1 for a malformed one. */
static int nc_emit_pq_range(const char *line, uint32_t *p, uint32_t *q)
{
    float pv;
    float qv;
    bool has_p;
    bool has_q;

    if (!line || !p || !q)
        return -1;
    has_p = g7x_get_field_float(line, "P", &pv);
    has_q = g7x_get_field_float(line, "Q", &qv);
    if (!has_p && !has_q)
        return 0;
    if (!has_p || !has_q ||
        !isfinite(pv) || !isfinite(qv) || pv < 1.0f || qv < 1.0f ||
        floorf(pv) != pv || floorf(qv) != qv || qv < pv)
        return -1;
    *p = (uint32_t)pv;
    *q = (uint32_t)qv;
    return 1;
}

/* Feed the numbered profile range [p, q] from the document. Unnumbered rows
   inside the range are contour rows, matching the parser's run-time rule. */
static bool nc_emit_feed_g7x_range(nc_emit_stream_t *stream, uint32_t p, uint32_t q)
{
    bool seen_start = false;
    uint32_t last = p - 1u;

    while (stream->doc && stream->source_line < stream->doc->line_count) {
        const char *line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
        size_t index = stream->source_line;
        uint32_t number = 0u;
        bool has_number = g7x_line_number(line, &number) && number != 0u;
        bool done = false;

        if (!seen_start) {
            if (!has_number || number != p) {
                stream->source_line++;
                continue;
            }
            seen_start = true;
        } else if (has_number) {
            if (number <= last || number > q) {
                stream->error = G7X_RANGE_AMBIGUOUS;
                break;
            }
            last = number;
        }
        if (g7x_contour_cmd_from_line(line) == G7X_CONTOUR_END) {
            /* A numbered range ends at N(Q), never at G80. */
            stream->error = G7X_RANGE_MISSING;
            break;
        }

        stream->source_line++;
        stream->g7x.pending_source_line = index;
        stream->error = g7x_stream_add_line(&stream->g7x, line, &done);
        if (stream->error != G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X ADD FAIL %.96s]\r\n", line);
            }
            g7x_stream_reset(&stream->g7x);
            stream->g7x_collecting = false;
            stream->active = false;
            return false;
        }
        if (has_number && number == q) {
            /* Closing the range plays the role of G80 for the generator. */
            bool end_done = false;
            stream->error = g7x_stream_add_line(&stream->g7x, "G80", &end_done);
            if (stream->error != G7X_OK) {
                g7x_stream_reset(&stream->g7x);
                stream->g7x_collecting = false;
                stream->active = false;
                return false;
            }
            stream->g7x_collecting = false;
            return true;
        }
    }

    if (stream->log) {
        grbl_stream_printf("[MSG:NC G7X numbered range incomplete]\r\n");
    }
    if (stream->error == G7X_OK) {
        stream->error = G7X_RANGE_MISSING;
    }
    g7x_stream_reset(&stream->g7x);
    stream->g7x_collecting = false;
    stream->active = false;
    return false;
}

static g7x_source_status_t nc_emit_numbered_next(void *user,
                                                 const g7x_source_pos_t *from,
                                                 g7x_source_pos_t *out,
                                                 char *text,
                                                 size_t text_sz)
{
    const nc_numbered_source_t *holder = user;
    size_t i;

    if (!holder || !holder->doc || !from || !out || !text || text_sz == 0)
        return G7X_SOURCE_ERROR;
    for (i = from->line_index; i < holder->doc->line_count; i++) {
        const char *line = nc_emit_trim(holder->doc->lines[i].text);
        uint32_t number = 0u;

        if (!*line || !g7x_line_number(line, &number) || number == 0u)
            continue;
        if (number < from->number)
            continue;
        snprintf(text, text_sz, "%s", line);
        out->number = number;
        out->line_index = i + 1u;
        return G7X_SOURCE_OK;
    }
    return G7X_SOURCE_MISSING;
}

g7x_source_t nc_emit_numbered_source(nc_numbered_source_t *holder,
                                     const nc_document_t *doc)
{
    g7x_source_t source;

    source.next = NULL;
    source.user = NULL;
    if (holder) {
        holder->doc = doc;
        source.next = nc_emit_numbered_next;
        source.user = holder;
    }
    return source;
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
        return nc_emit_g7x_next(stream, out, out_sz, source_line);
    }
    if (stream->g7x_collecting) {
        if (nc_emit_feed_g7x(stream))
            return nc_emit_g7x_next(stream, out, out_sz, source_line);
        return NC_EMIT_ERROR;
    }

    if (stream->source_line >= stream->doc->line_count) {
        stream->active = false;
        return NC_EMIT_SKIP;
    }

    line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
    if (g7x_cycle_from_line(line) != G7X_CYCLE_NONE) {
        g7x_cycle_t cycle = g7x_cycle_from_line(line);
        uint32_t pq_p = 0u;
        uint32_t pq_q = 0u;
        int pq = (cycle == G7X_CYCLE_G71 || cycle == G7X_CYCLE_G72)
                     ? nc_emit_pq_range(line, &pq_p, &pq_q)
                     : 0;

        if (pq < 0) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X BEGIN FAIL %.96s]\r\n", line);
            }
            stream->error = G7X_BAD_FIELD;
            stream->active = false;
            return NC_EMIT_ERROR;
        }
        stream->error = g7x_stream_begin(&stream->g7x, line);
        if (stream->error == G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X BEGIN %.96s]\r\n", line);
            }
            stream->source_line++;
            stream->g7x_collecting = true;
            if (pq > 0) {
                if (nc_emit_feed_g7x_range(stream, pq_p, pq_q)) {
                    return nc_emit_g7x_next(stream, out, out_sz, source_line);
                }
                return NC_EMIT_ERROR;
            }
            if (nc_emit_feed_g7x(stream)) {
                return nc_emit_g7x_next(stream, out, out_sz, source_line);
            }
        }
        if (stream->log) {
            grbl_stream_printf("[MSG:NC G7X BEGIN/FEED FAIL %.96s]\r\n", line);
        }
        stream->active = false;
        return NC_EMIT_ERROR;
    }

    result = nc_emit_source_line(stream->doc, stream->source_line, out, out_sz);
    stream->source_line++;
    if (stream->source_line >= stream->doc->line_count) {
        stream->active = false;
    }
    return result;
}
