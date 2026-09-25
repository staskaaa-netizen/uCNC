#include "nc_emit.h"
#include "nc_g7x.h"

#include "../../interface/grbl_stream.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *nc_emit_trim(const char *line)
{
    while (line && (*line == ' ' || *line == '\t')) {
        line++;
    }
    return line ? line : "";
}

/* Where a line leaves the tool, which is what a program written in increments
   has to mean: `x`/`z` come in as the point the line starts from and go out as
   the point it ends on.

   `U` and `W` are Fanuc's incremental X and Z: they are **always** increments,
   whichever distance mode is active, and in the same units as X and Z (a
   diameter increment in G7, a radius one in G8), which is what lets a program
   write the geometry absolutely and the moves between it as distances. An `X`
   or `Z` word is the absolute it always was; an axis the line does not name
   keeps the point it had.

   False when the line names no axis at all (an ordinary line: the point does
   not move and nothing here applies). When `out` is given it receives the line
   with the increments written as the X and Z words they mean - which is what
   the controller is sent, because the machine's parser has no U or W and does
   not need one: the increment is the *program's* spelling, and this is the one
   place it is read. `words` (optional) reports which of the four it carried, as
   `NC_EMIT_WORD_*` below: an absolute word is what establishes where the tool
   is, so an increment whose axis has never been given absolutely cannot be
   resolved - the position it counts from is the machine's, not the file's. */
bool nc_emit_line_point(const char *line, float *x, float *z,
                        char *out, size_t out_sz, uint8_t *words_out)
{
    nc_word_t words[24];
    float ax = x ? *x : 0.0f;
    float az = z ? *z : 0.0f;
    bool has_axis = false;
    uint8_t seen = 0u;
    int count;
    int i;
    int src = 0;
    int used = 0;

    if (!line) {
        return false;
    }
    count = nc_parse_words(line, words, 24);
    for (i = 0; i < count; i++) {
        float value = 0.0f;
        char letter = (char)toupper((unsigned char)words[i].letter);

        if (letter != 'X' && letter != 'Z' && letter != 'U' && letter != 'W') {
            continue;
        }
        if (!nc_word_value(line, &words[i], &value)) {
            continue;
        }
        has_axis = true;
        if (letter == 'X') {
            ax = value;
            seen |= NC_EMIT_WORD_X_ABS;
        } else if (letter == 'Z') {
            az = value;
            seen |= NC_EMIT_WORD_Z_ABS;
        } else if (letter == 'U') {
            /* Always an increment, whichever distance mode is active: that is
               what the word is for. */
            seen |= NC_EMIT_WORD_U_INC;
            ax += value;
        } else {
            seen |= NC_EMIT_WORD_W_INC;
            az += value;
        }
    }
    if (!has_axis) {
        return false;
    }
    if (x) {
        *x = ax;
    }
    if (z) {
        *z = az;
    }
    if (words_out) {
        *words_out = seen;
    }
    if (!out || out_sz == 0u) {
        return true;
    }
    /* The line as the controller reads it: the increments written as the X and
       Z words they mean, everything else byte for byte. When there is no room
       the line is left as it was - the caller's buffer is the document's own
       line width, so this is a guard, not a case that happens. */
    out[0] = '\0';
    for (i = 0; i < count; i++) {
        char letter = (char)toupper((unsigned char)words[i].letter);
        int copy;
        int n;

        copy = words[i].start - src;
        if (copy > 0) {
            if ((size_t)(used + copy) >= out_sz) {
                return true;
            }
            memcpy(out + used, line + src, (size_t)copy);
            used += copy;
            src = words[i].start;
        }
        if (letter == 'U' || letter == 'W') {
            n = snprintf(out + used, out_sz - (size_t)used, "%c%.3f",
                         letter == 'U' ? 'X' : 'Z',
                         (double)(letter == 'U' ? ax : az));
            if (n <= 0 || (size_t)n >= out_sz - (size_t)used) {
                return true;
            }
            used += n;
        } else {
            copy = words[i].end - words[i].start;
            if ((size_t)(used + copy) >= out_sz) {
                return true;
            }
            memcpy(out + used, line + src, (size_t)copy);
            used += copy;
        }
        src = words[i].end;
    }
    {
        size_t rest = strlen(line + src);

        if ((size_t)used + rest + 1u > out_sz) {
            return true;
        }
        memcpy(out + used, line + src, rest + 1u);
    }
    return true;
}

/* The lines the controller is given: an ordinary motion, or a contour row that
   goes into the generator. Everything else in a program - a cycle header, a
   stock definition, a `G80`, a comment - is read by this layer and never sent,
   and its words are parameters rather than a position.

   Whoever asks where the tool is has to ask this first: a `G71 U1 ...` line
   spells a depth of cut with the same letter an increment is spelled with, so
   the `U` rule only applies to the lines this answers true for. The preview
   draws its contour from the sender, so it asks the same question here instead
   of keeping a second list of what a line means. */
bool nc_emit_line_is_direct(const char *line)
{
    /* A leading `N` is the row's own label; `nc_emit_source_line()` answers
       about the body, so this does too. */
    line = g7x_skip_line_number(nc_emit_trim(line));
    if (!*line || *line == '(') {
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
    if (!nc_emit_line_is_direct(body)) {
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
    stream->x = 0.0f;
    stream->z = 0.0f;
    stream->x_known = false;
    stream->z_known = false;
    stream->row_valid = false;
    g7x_stream_reset(&stream->g7x);
    /* A run that starts in the middle of a program still has to know where the
       tool is: the lines before it are emitted into nothing, but they move the
       point the increments are counted from - cycles and all, because the point
       follows what the stream would have sent. */
    if (doc && start_line > 0u && start_line <= doc->line_count) {
        nc_emit_stream_t prime;
        char scratch[NC_MAX_LINE_LEN];
        size_t guard = 0u;

        nc_emit_stream_begin(&prime, doc, 0u);
        nc_emit_stream_set_log(&prime, false);
        while (prime.active && nc_emit_stream_line(&prime) < start_line &&
               guard++ < NC_MAX_LINES * 8u) {
            if (nc_emit_stream_next(&prime, scratch, sizeof(scratch), 0) ==
                NC_EMIT_ERROR) {
                break;
            }
        }
        stream->x = prime.x;
        stream->z = prime.z;
        stream->x_known = prime.x_known;
        stream->z_known = prime.z_known;
        stream->row_valid = false;
    }
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

/* Hand one source row to the generator, with the increments it carries written
   as the points they mean. A contour is relative row to row, so the rows are
   resolved against a point of their own: the stream's point is where the
   *machine* is, and it is this block's generated motion - not the rows - that
   moves it. */
static const char *nc_emit_row(nc_emit_stream_t *stream,
                               const char *line,
                               char *scratch,
                               size_t scratch_sz)
{
    if (!stream->row_valid) {
        stream->row_x = stream->x;
        stream->row_z = stream->z;
        stream->row_valid = true;
    }
    if (nc_emit_line_point(line, &stream->row_x, &stream->row_z,
                            scratch, scratch_sz, 0) &&
        strcmp(scratch, line) != 0) {
        return scratch;
    }
    return line;
}

static bool nc_emit_feed_g7x(nc_emit_stream_t *stream)
{
    while (stream && stream->doc && stream->source_line < stream->doc->line_count) {
        const char *line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
        char row[NC_MAX_LINE_LEN];
        bool done = false;

        stream->source_line++;
        stream->g7x.pending_source_line = stream->source_line - 1u;
        stream->error = g7x_stream_add_line(&stream->g7x,
                                           nc_emit_row(stream, line, row,
                                                       sizeof(row)),
                                           &done);
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

/* Resolve what a line carries and move the stream's point to where it leaves
   the tool.

   `U` and `W` are Fanuc's incremental X and Z: the *program* may spell a move as
   the distance from where the tool is, and the controller has no such word. The
   rule is read in one place (`nc_emit_line_point()`), and what goes out is the
   absolute line it means - so the increment is the operator's spelling and the
   block the machine runs is the ordinary one it always was. A line with no axis
   word does not move the tool and is left alone; a program that moves by
   increments before it has said where it is cannot be resolved here, so its
   increments are left as written and the controller refuses them by name. */
static void nc_emit_resolve(nc_emit_stream_t *stream, char *line, size_t out_sz)
{
    char resolved[NC_MAX_LINE_LEN];
    float x = stream->x;
    float z = stream->z;
    uint8_t words = 0u;

    if (!nc_emit_line_point(line, &x, &z, resolved, sizeof(resolved), &words)) {
        return;
    }
    if (words & NC_EMIT_WORD_X_ABS) {
        stream->x_known = true;
    }
    if (words & NC_EMIT_WORD_Z_ABS) {
        stream->z_known = true;
    }
    /* An increment counts from where the tool is, so its axis has to have been
       given absolutely before it: otherwise the file would be inventing a
       position. Left as written, the controller refuses it by name. */
    if (((words & NC_EMIT_WORD_U_INC) && !stream->x_known) ||
        ((words & NC_EMIT_WORD_W_INC) && !stream->z_known)) {
        return;
    }
    stream->x = x;
    stream->z = z;
    if (strcmp(resolved, line) != 0 && strlen(resolved) < out_sz) {
        strcpy(line, resolved);
    }
}

/* Feed the numbered range that sits *above* the cycle line. `G70 P Q` names the
   profile the roughing cycle already collected - the rows are before it, not
   after - so the finish cut is expanded from the range
   `nc_g7x_range_above()` finds (the pane marks the same lines from the same
   answer), fed in program order. A range that is not there is reported as
   missing instead of being guessed. */
static bool nc_emit_feed_g7x_range_above(nc_emit_stream_t *stream,
                                         uint32_t p,
                                         uint32_t q)
{
    size_t first = 0u;
    size_t last = 0u;
    size_t i;
    bool done = false;

    if (!stream || !stream->doc || p == 0u || q < p)
        return false;

    if (!nc_g7x_range_above(stream->doc, stream->source_line, p, q, &first, &last)) {
        stream->error = G7X_RANGE_MISSING;
        return false;
    }

    for (i = first; i <= last; i++) {
        const char *line = nc_emit_trim(stream->doc->lines[i].text);
        char row[NC_MAX_LINE_LEN];

        if (!*line || g7x_contour_cmd_from_line(line) == G7X_CONTOUR_END)
            continue;
        stream->g7x.pending_source_line = i;
        stream->error = g7x_stream_add_line(&stream->g7x,
                                           nc_emit_row(stream, line, row,
                                                       sizeof(row)),
                                           &done);
        if (stream->error != G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X ADD FAIL %.96s]\r\n", line);
            }
            g7x_stream_reset(&stream->g7x);
            stream->g7x_collecting = false;
            stream->active = false;
            return false;
        }
    }
    /* Closing the range plays the role of G80 for the generator. */
    stream->error = g7x_stream_add_line(&stream->g7x, "G80", &done);
    if (stream->error != G7X_OK) {
        g7x_stream_reset(&stream->g7x);
        stream->g7x_collecting = false;
        stream->active = false;
        return false;
    }
    stream->g7x_collecting = false;
    return true;
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
        char row[NC_MAX_LINE_LEN];

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
        stream->error = g7x_stream_add_line(&stream->g7x,
                                           nc_emit_row(stream, line, row,
                                                       sizeof(row)),
                                           &done);
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
        result = nc_emit_g7x_next(stream, out, out_sz, source_line);
        if (result == NC_EMIT_LINE) {
            /* A generated block is absolute, but it is what moves the machine,
               so the stream's point follows it. */
            nc_emit_resolve(stream, out, out_sz);
        }
        return result;
    }
    if (stream->g7x_collecting) {
        if (nc_emit_feed_g7x(stream)) {
            result = nc_emit_g7x_next(stream, out, out_sz, source_line);
            if (result == NC_EMIT_LINE) {
                nc_emit_resolve(stream, out, out_sz);
            }
            return result;
        }
        return NC_EMIT_ERROR;
    }

    if (stream->source_line >= stream->doc->line_count) {
        stream->active = false;
        return NC_EMIT_SKIP;
    }

    line = nc_emit_trim(stream->doc->lines[stream->source_line].text);
    if (g7x_cycle_from_line(line) != G7X_CYCLE_NONE) {
        const char *second = NULL;
        size_t second_line = 0u;
        size_t next = stream->source_line + 1u;
        uint32_t pq_p = 0u;
        uint32_t pq_q = 0u;
        bool numbered = false;

        /* Fanuc two-line header: the next content line repeats the cycle and
           carries the range. */
        while (next < stream->doc->line_count &&
               !*nc_emit_trim(stream->doc->lines[next].text))
            next++;
        if (next < stream->doc->line_count) {
            const char *next_line = nc_emit_trim(stream->doc->lines[next].text);
            if (nc_g7x_line_is_header(next_line) &&
                g7x_cycle_from_line(next_line) == g7x_cycle_from_line(line)) {
                second = next_line;
                second_line = next;
            }
        }
        if (nc_g7x_line_has_range_words(second ? second : line)) {
            if (!nc_g7x_line_range(second ? second : line, &pq_p, &pq_q)) {
                if (stream->log) {
                    grbl_stream_printf("[MSG:NC G7X BEGIN FAIL %.96s]\r\n", line);
                }
                stream->error = G7X_BAD_FIELD;
                stream->active = false;
                return NC_EMIT_ERROR;
            }
            numbered = true;
        }
        if (second) {
            stream->error = g7x_stream_begin_linked(&stream->g7x, line, second);
        } else {
            stream->error = g7x_stream_begin(&stream->g7x, line);
        }
        if (stream->error != G7X_OK && nc_g7x_line_has_range_words(line)) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X BEGIN FAIL %.96s]\r\n", line);
            }
            stream->error = G7X_BAD_FIELD;
            stream->active = false;
            return NC_EMIT_ERROR;
        }
        if (stream->error == G7X_OK) {
            if (stream->log) {
                grbl_stream_printf("[MSG:NC G7X BEGIN %.96s]\r\n", line);
            }
            stream->source_line = (second ? second_line : stream->source_line) + 1u;
            stream->g7x_collecting = true;
            /* A new contour: its rows start from the point the tool is at. */
            stream->row_valid = false;
            if (numbered) {
                /* G70's range is above it - the profile the roughing cycle
                   collected - while every other cycle's range follows it. */
                bool fed = g7x_cycle_from_line(line) == G7X_CYCLE_G70
                               ? nc_emit_feed_g7x_range_above(stream, pq_p, pq_q)
                               : nc_emit_feed_g7x_range(stream, pq_p, pq_q);

                if (fed) {
                    result = nc_emit_g7x_next(stream, out, out_sz, source_line);
                    if (result == NC_EMIT_LINE) {
                        nc_emit_resolve(stream, out, out_sz);
                    }
                    return result;
                }
                return NC_EMIT_ERROR;
            }
            if (nc_emit_feed_g7x(stream)) {
                result = nc_emit_g7x_next(stream, out, out_sz, source_line);
                if (result == NC_EMIT_LINE) {
                    nc_emit_resolve(stream, out, out_sz);
                }
                return result;
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
    if (result == NC_EMIT_LINE) {
        /* What is sent is the line the controller reads: an increment is written
           out as the X or Z it means, and the stream's point moves with it. */
        nc_emit_resolve(stream, out, out_sz);
    }
    return result;
}
