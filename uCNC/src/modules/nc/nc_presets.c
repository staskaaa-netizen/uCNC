#include "nc_presets.h"

static nc_result_t nc_preset_insert_lines(nc_document_t *doc, const char *const *lines, int count)
{
    int i;
    size_t at;
    nc_result_t r;

    if (!doc || !lines || count <= 0) {
        return NC_ERR_BAD_ARG;
    }

    at = doc->cursor_line + 1;
    if (doc->line_count == 0) {
        at = 0;
    }

    for (i = 0; i < count; i++) {
        r = nc_insert_line(doc, at + (size_t)i, lines[i]);
        if (r != NC_OK) {
            return r;
        }
    }

    doc->cursor_line = at;
    doc->selected_word = -1;
    return NC_OK;
}

nc_result_t nc_insert_preset(nc_document_t *doc, nc_preset_t preset)
{
    static const char *const setup[] = {
        "G970 X-5 U60 Z-60 W5",
        "G971 X50 Z50 I0 E0",
        "G972 C12",
        "G973 P7"
    };
    static const char *const od[] = {
        "G71 U2 R1 X0.5 Z0.5 F120",
        "\tG1 X50 Z0",
        "\tG1 X30 Z-20",
        "G80"
    };
    static const char *const id[] = {
        "G71 U2 R1 X0.5 Z0.5 F120",
        "\tG1 X20 Z0",
        "\tG1 X35 Z-20",
        "G80"
    };
    static const char *const face[] = {
        "G72 W2 R1 X0.5 Z0.5 F120",
        "\tG1 X50 Z0",
        "\tG1 X0 Z0",
        "G80"
    };
    static const char *const line[] = {
        "G1 X50 Z-20"
    };
    static const char *const arc[] = {
        "G2 X50 Z-20 R5"
    };
    static const char *const end[] = {
        "G80"
    };

    switch (preset) {
    case NC_PRESET_OD:
        return nc_preset_insert_lines(doc, od, (int)(sizeof(od) / sizeof(od[0])));
    case NC_PRESET_ID:
        return nc_preset_insert_lines(doc, id, (int)(sizeof(id) / sizeof(id[0])));
    case NC_PRESET_FACE:
        return nc_preset_insert_lines(doc, face, (int)(sizeof(face) / sizeof(face[0])));
    case NC_PRESET_LINE:
        return nc_preset_insert_lines(doc, line, (int)(sizeof(line) / sizeof(line[0])));
    case NC_PRESET_ARC:
        return nc_preset_insert_lines(doc, arc, (int)(sizeof(arc) / sizeof(arc[0])));
    case NC_PRESET_SETUP:
        return nc_preset_insert_lines(doc, setup, (int)(sizeof(setup) / sizeof(setup[0])));
    case NC_PRESET_END:
        return nc_preset_insert_lines(doc, end, (int)(sizeof(end) / sizeof(end[0])));
    default:
        return NC_ERR_BAD_ARG;
    }
}
