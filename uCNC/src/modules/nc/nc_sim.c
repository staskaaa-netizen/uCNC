#include "nc_sim.h"

#include <string.h>

bool nc_sim_line_word_float(const char *line, char letter, float *out)
{
    nc_word_t words[16];
    int count;
    int i;

    count = nc_parse_words(line, words, 16);
    for (i = 0; i < count; i++) {
        if (words[i].letter == letter) {
            return nc_word_value(line, &words[i], out);
        }
    }

    return false;
}

void nc_sim_collect_preview(const nc_document_t *doc, nc_preview_info_t *p)
{
    float last_x = 0.0f;
    float last_z = 0.0f;
    bool have_last = false;
    size_t i;

    if (!p) {
        return;
    }

    memset(p, 0, sizeof(*p));
    p->stock_x = 50.0f;
    p->stock_z = 75.0f;
    p->stock_i = 0.0f;
    p->stock_e = 3.0f;
    p->min_x = 1000000.0f;
    p->min_z = 1000000.0f;

    if (!doc) {
        return;
    }

    for (i = 0; i < doc->line_count; i++) {
        const char *line = doc->lines[i].text;
        g7x_cycle_t cycle;
        g7x_contour_cmd_t cmd;
        float px = last_x;
        float pz = last_z;
        bool has_x;
        bool has_z;

        if (!line) {
            continue;
        }

        if (g7x_command_is(line, "G971")) {
            (void)nc_sim_line_word_float(line, 'X', &p->stock_x);
            (void)nc_sim_line_word_float(line, 'Z', &p->stock_z);
            (void)nc_sim_line_word_float(line, 'I', &p->stock_i);
            (void)nc_sim_line_word_float(line, 'E', &p->stock_e);
        }

        cycle = g7x_cycle_from_line(line);
        if (cycle != G7X_CYCLE_NONE) {
            p->cycles++;
            p->last_cycle = cycle;
        }

        cmd = g7x_contour_cmd_from_line(line);
        if (cmd == G7X_CONTOUR_NONE || cmd == G7X_CONTOUR_END) {
            continue;
        }

        has_x = nc_sim_line_word_float(line, 'X', &px);
        has_z = nc_sim_line_word_float(line, 'Z', &pz);
        if (!have_last) {
            last_x = has_x ? px : p->stock_x;
            last_z = has_z ? pz : 0.0f;
            have_last = true;
        }
        if (!has_x && !has_z) {
            continue;
        }

        if (px < p->min_x) p->min_x = px;
        if (px > p->max_x) p->max_x = px;
        if (pz < p->min_z) p->min_z = pz;
        if (pz > p->max_z) p->max_z = pz;
        if (cmd == G7X_CONTOUR_RAPID) {
            p->rapid_segments++;
        } else if (cmd == G7X_CONTOUR_ARC_CW || cmd == G7X_CONTOUR_ARC_CCW) {
            p->arc_segments++;
            p->path_segments++;
        } else {
            p->path_segments++;
        }
        last_x = px;
        last_z = pz;
    }

    if (p->stock_x <= 0.0f) p->stock_x = 50.0f;
    if (p->stock_z <= 0.0f) p->stock_z = 75.0f;
    if (p->stock_i < 0.0f) p->stock_i = 0.0f;
    if (p->stock_i >= p->stock_x) p->stock_i = 0.0f;
    if (p->stock_e < 0.0f) p->stock_e = 0.0f;
    p->stock_visible_z = p->stock_z + p->stock_e;
    if (p->stock_visible_z <= 0.0f) p->stock_visible_z = p->stock_z;
    if (p->min_x > p->max_x) {
        p->min_x = 0.0f;
        p->max_x = p->stock_x;
    }
    if (p->min_z > p->max_z) {
        p->min_z = 0.0f;
        p->max_z = p->stock_z;
    }
}
