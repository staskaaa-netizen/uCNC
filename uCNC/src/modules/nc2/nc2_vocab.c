#include "nc2_vocab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A word's meaning where the line's own G code gives it one. `gcode` 0 is "no
   cycle" - the plain words and the increments, which mean the same anywhere. */
typedef struct {
    int gcode;
    char word;
    const char *label;
} nc2_vocab_entry_t;

static const nc2_vocab_entry_t g_nc2_vocab[] = {
    { 1,  'C', "Chamfer" },
    { 1,  'R', "Corner radius" },
    /* Fanuc's increments: a line may name its axis as a distance from where the
       tool is instead of a position. */
    { 0,  'U', "X increment" },
    { 0,  'W', "Z increment" },
    { 1,  'U', "X increment" },
    { 1,  'W', "Z increment" },
    { 4,  'P', "Dwell time" },
    { 33, 'K', "Thread pitch" },
    /* A cycle's P/Q range: P names the block the profile starts at, Q the one it
       ends at. The panel's own entries write all of them, so none may fall
       through to "NC word". */
    { 70, 'P', "Profile start block" },
    { 70, 'Q', "Profile end block" },
    { 71, 'U', "Depth/pass" },
    { 71, 'R', "Retract" },
    { 71, 'X', "X finish allowance" },
    { 71, 'Z', "Z finish allowance" },
    { 71, 'F', "Feed" },
    { 71, 'P', "Profile start block" },
    { 71, 'Q', "Profile end block" },
    { 72, 'W', "Depth/pass" },
    { 72, 'R', "Retract" },
    { 72, 'X', "X finish allowance" },
    { 72, 'Z', "Z finish allowance" },
    { 72, 'F', "Feed" },
    { 72, 'P', "Profile start block" },
    { 72, 'Q', "Profile end block" },
    { 76, 'X', "Thread end X" },
    { 76, 'Z', "Thread end Z" },
    { 76, 'P', "Thread height" },
    { 76, 'Q', "First cut" },
    { 76, 'F', "Thread lead" },
    { 76, 'I', "Radial taper" },
    { 76, 'L', "Spring passes" },
    { 76, 'R', "Finish allowance" },
    { 2,  'R', "Arc radius" },
    { 2,  'I', "Arc centre X" },
    { 2,  'K', "Arc centre Z" },
    { 3,  'R', "Arc radius" },
    { 3,  'I', "Arc centre X" },
    { 3,  'K', "Arc centre Z" },
    { 970, 'X', "Preview min X" },
    { 970, 'U', "Preview max X" },
    { 970, 'Z', "Preview min Z" },
    { 970, 'W', "Preview max Z" },
    { 971, 'X', "Stock OD" },
    { 971, 'Z', "Stock length" },
    { 971, 'I', "Stock ID" },
    { 971, 'E', "Extra stock" },
    { 972, 'C', "Clamp length" },
    { 973, 'P', "Preview mode" }
};

/* The G code the line names, or -1 when it names none. */
static int nc2_vocab_line_gcode(const char *line)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int i;

    count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        if (fields[i].letter != 'G') {
            continue;
        }
        if (fields[i].value == fields[i].end) {
            return -1;                  /* a `G` waiting for its number */
        }
        return (int)(strtod(line + fields[i].value, 0) + 0.5);
    }
    return -1;
}

/* A tool table's own line: a `T` word and no G or M, which is what the file the
   TOOLS screen edits holds. Its letters are readings, not motion. */
static bool nc2_vocab_line_is_tool(const char *line)
{
    nc2_field_t fields[NC2_MAX_FIELDS];
    int count;
    int i;
    bool has_t = false;

    count = nc2_fields(line, fields, NC2_MAX_FIELDS);
    for (i = 0; i < count; i++) {
        if (fields[i].letter == 'G' || fields[i].letter == 'M') {
            return false;
        }
        if (fields[i].letter == 'T') {
            has_t = true;
        }
    }
    return has_t;
}

const char *nc2_vocab_label(const char *line, const nc2_field_t *field)
{
    static char g_label[16];
    int gcode;
    size_t i;
    char letter;

    if (!line || !field) {
        return "";
    }
    letter = (char)toupper((unsigned char)field->letter);
    if (letter == 'G') {
        snprintf(g_label, sizeof(g_label), "G-code %.0f",
                 (double)strtod(line + field->value, 0));
        return g_label;
    }
    gcode = nc2_vocab_line_gcode(line);
    if (gcode < 0 && nc2_vocab_line_is_tool(line)) {
        switch (letter) {
        case 'T': return "Tool number";
        case 'R': return "Radius";
        case 'O': return "Orient";
        case 'F': return "Rough feed";
        case 'Q': return "Finish feed";
        case 'D': return "Rough DOC";
        case 'E': return "Finish DOC";
        case 'S': return "Spindle speed";
        case 'X': return "X offset";
        case 'Z': return "Z offset";
        default: return "Tool field";
        }
    }
    for (i = 0u; i < sizeof(g_nc2_vocab) / sizeof(g_nc2_vocab[0]); i++) {
        if (g_nc2_vocab[i].gcode == gcode && g_nc2_vocab[i].word == letter) {
            return g_nc2_vocab[i].label;
        }
    }
    switch (letter) {
    case 'X': return "X position";
    case 'Z': return "Z position";
    case 'U': return "X increment";
    case 'W': return "Z increment";
    case 'F': return "Feed";
    case 'S': return "Spindle speed";
    case 'T': return "Tool";
    case 'M': return "M-code";
    case 'N': return "Block number";
    default: return "NC word";
    }
}
