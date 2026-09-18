#include "nc_sender.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The NC emitter logs through the firmware's protocol stream. A host build has
   no such stream, so the log hook is a no-op here. */
void grbl_stream_printf(const char *fmt, ...)
{
    (void)fmt;
}

nc_sender_target_t nc_sender_grbl_target(void)
{
    nc_sender_target_t target;

    target.supports_lathe_words = false;
    target.supports_threading = false;
    target.keep_comments = false;
    return target;
}

nc_sender_target_t nc_sender_ucnc_target(void)
{
    nc_sender_target_t target;

    target.supports_lathe_words = true;
    target.supports_threading = true;
    target.keep_comments = false;
    return target;
}

void nc_sender_begin(nc_sender_t *sender,
                     const nc_document_t *doc,
                     const nc_sender_target_t *target)
{
    if (!sender)
        return;
    memset(sender, 0, sizeof(*sender));
    sender->doc = doc;
    sender->target = target ? *target : nc_sender_grbl_target();
    sender->diameter_mode = true; /* NC programs default to G7 diameter mode */
    nc_emit_stream_begin(&sender->emit, doc, 0);
    nc_emit_stream_set_log(&sender->emit, false);
}

/* Append a number without trailing zeros so converted words stay readable. */
static void nc_sender_format_scaled(char *out,
                                    size_t out_sz,
                                    char letter,
                                    float value)
{
    char text[32];
    char *dot;

    snprintf(text, sizeof(text), "%.4f", (double)value);
    /* Trim only the fractional zeros, never the digits before the point. */
    dot = strchr(text, '.');
    if (dot) {
        char *end = text + strlen(text);
        while (end > dot && end[-1] == '0')
            *--end = '\0';
        if (end == dot + 1)
            *dot = '\0';
    }
    if (!text[0])
        snprintf(text, sizeof(text), "0");
    snprintf(out, out_sz, "%c%s", letter, text);
}

/* Rewrite one line for the target:

   - G7/G8 are lathe modal words Grbl does not know. A target without lathe word
     support consumes them and remembers the mode instead.
   - In diameter mode every X word is a diameter. A radius-only target gets the
     halved value; uCNC does the same conversion inside its G7/G8 modifier.

   Returns false when the line carries nothing a controller should receive. */
static bool nc_sender_rewrite(nc_sender_t *sender,
                              const char *in,
                              char *out,
                              size_t out_sz)
{
    size_t used = 0u;
    const char *p = in;
    bool lathe_target = sender->target.supports_lathe_words;

    if (out_sz == 0u)
        return false;
    out[0] = '\0';

    while (*p) {
        if (*p == '(' || *p == ';') {
            if (sender->target.keep_comments) {
                size_t len = strlen(p);
                if (used + len + 1u >= out_sz)
                    break;
                memcpy(out + used, p, len);
                used += len;
                out[used] = '\0';
            }
            break;
        }
        if (isspace((unsigned char)*p)) {
            if (used + 1u < out_sz) {
                out[used++] = *p;
                out[used] = '\0';
            }
            p++;
            continue;
        }

        {
            char letter = (char)toupper((unsigned char)*p);
            const char *start = p;
            const char *number;
            bool numeric;

            p++;
            number = p;
            if (*p == '+' || *p == '-')
                p++;
            while (*p && (isdigit((unsigned char)*p) || *p == '.'))
                p++;
            numeric = p != number && (isdigit((unsigned char)number[0]) ||
                                      ((number[0] == '+' || number[0] == '-') &&
                                       isdigit((unsigned char)number[1])));

            if (letter == 'G' && numeric && !lathe_target) {
                long code = strtol(number, NULL, 10);
                if (code == 7 || code == 8) {
                    sender->diameter_mode = code == 7;
                    sender->dropped_words++;
                    continue;
                }
            }
            if (letter == 'X' && numeric && sender->diameter_mode && !lathe_target) {
                char word[40];
                nc_sender_format_scaled(word, sizeof(word), 'X',
                                        strtof(number, NULL) * 0.5f);
                sender->converted++;
                if (used + strlen(word) + 1u < out_sz) {
                    memcpy(out + used, word, strlen(word));
                    used += strlen(word);
                    out[used] = '\0';
                }
                continue;
            }
            if (used + (size_t)(p - start) + 1u < out_sz) {
                memcpy(out + used, start, (size_t)(p - start));
                used += (size_t)(p - start);
                out[used] = '\0';
            }
        }
    }

    /* Trim trailing whitespace left by removed words. */
    while (used > 0u && isspace((unsigned char)out[used - 1u]))
        out[--used] = '\0';
    return used > 0u;
}

nc_sender_result_t nc_sender_next(nc_sender_t *sender,
                                  char *out,
                                  size_t out_sz,
                                  size_t *source_line)
{
    char line[NC_MAX_LINE_LEN];

    if (!sender || !out || out_sz == 0u)
        return NC_SENDER_ERROR;
    out[0] = '\0';

    for (;;) {
        size_t emitted_line = 0u;
        nc_emit_result_t result = nc_emit_stream_next(&sender->emit, line,
                                                      sizeof(line),
                                                      &emitted_line);

        if (result == NC_EMIT_SKIP) {
            if (sender->emit.active)
                continue;
            sender->error = sender->emit.error;
            return sender->emit.error == G7X_OK ? NC_SENDER_DONE : NC_SENDER_ERROR;
        }
        if (result == NC_EMIT_ERROR) {
            sender->error = sender->emit.error;
            return NC_SENDER_ERROR;
        }

        if (!sender->target.supports_threading &&
            (g7x_command_is(line, "G33") || g7x_command_is(line, "G76"))) {
            sender->error = G7X_UNSUPPORTED;
            return NC_SENDER_ERROR;
        }

        if (source_line)
            *source_line = emitted_line;
        if (g7x_cycle_from_line(line) != G7X_CYCLE_NONE ||
            g7x_contour_cmd_from_line(line) != G7X_CONTOUR_NONE)
            sender->expanded++;
        else if (line[0] != '(')
            sender->started = true;

        if (!nc_sender_rewrite(sender, line, out, out_sz))
            continue; /* the whole line was lathe-only or comment text */
        return NC_SENDER_OK;
    }
}

g7x_result_t nc_sender_last_error(const nc_sender_t *sender)
{
    return sender ? sender->error : G7X_BAD_FIELD;
}

bool nc_sender_line_sendable(const char *line)
{
    const char *p = line;

    if (!line)
        return false;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '(' || *p == ';')
        return false;
    if (g7x_command_is(p, "G970") || g7x_command_is(p, "G971") ||
        g7x_command_is(p, "G972") || g7x_command_is(p, "G973"))
        return false;
    return true;
}

nc_result_t nc_sender_load_file(nc_document_t *doc, const char *path)
{
    FILE *fp;
    char line[NC_MAX_LINE_LEN * 2];

    if (!doc || !path || !*path)
        return NC_ERR_BAD_ARG;
    fp = fopen(path, "r");
    if (!fp)
        return NC_ERR_IO;

    nc_document_init(doc);
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r'))
            line[--len] = '\0';
        if (len >= NC_MAX_LINE_LEN) {
            fclose(fp);
            return NC_ERR_LINE_TOO_LONG;
        }
        if (nc_insert_line(doc, doc->line_count, line) != NC_OK) {
            fclose(fp);
            return NC_ERR_TOO_MANY_LINES;
        }
    }
    fclose(fp);
    snprintf(doc->path, sizeof(doc->path), "%s", path);
    doc->dirty = false;
    return NC_OK;
}

nc_result_t nc_sender_save_file(const nc_document_t *doc, const char *path)
{
    FILE *fp;
    size_t i;

    if (!doc || !path || !*path)
        return NC_ERR_BAD_ARG;
    fp = fopen(path, "w");
    if (!fp)
        return NC_ERR_IO;
    for (i = 0; i < doc->line_count; i++) {
        if (fprintf(fp, "%s\n", doc->lines[i].text) < 0) {
            fclose(fp);
            return NC_ERR_IO;
        }
    }
    if (fclose(fp) != 0)
        return NC_ERR_IO;
    return NC_OK;
}
