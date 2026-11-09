#include "ncc_vocab.h"

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
} ncc_vocab_entry_t;

static const ncc_vocab_entry_t g_ncc_vocab[] = {
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

/* The G codes themselves: the name the operator reads, the words the code
   takes, and the line a key writes for it. nc kept this table (`nc_vocab.c`)
   and nc2 dropped it - which is why a typed `G71` stayed a bare `G71` with
   nothing to walk into. `G70` and `G80` are here because the panel's own
   entries write them; the rest are nc's. */
typedef struct {
    int gcode;
    const char *name;
    const char *parameters;
} ncc_gcode_info_t;

static const ncc_gcode_info_t g_ncc_gcodes[] = {
    { 0, "Rapid move", "X Z" },
    /* A plain linear move: the words a general `G1` takes. `C` (chamfer) and
       `R` (corner radius) belong to a *contour* row, which is the G7X path
       builder's business, not to every feed move (bench: "g1 with c and r is
       wrong. it does fit only to the g7x path but not general code"). */
    { 1, "Linear move", "X Z F" },
    { 2, "Arc CW", "X Z R I K F" },
    { 3, "Arc CCW", "X Z R I K F" },
    { 4, "Dwell", "P" },
    { 17, "XY plane", "" },
    { 18, "XZ plane", "" },
    { 19, "YZ plane", "" },
    { 20, "Inch units", "" },
    { 21, "Metric units", "" },
    { 28, "Home", "X Z" },
    { 30, "Home secondary", "X Z" },
    { 33, "Spindle sync", "X Z K F" },
    /* The header's words. `N` is not one of them: it numbers a profile row, and
       the cycle reads the range from P and Q. */
    { 70, "Finish profile", "P Q" },
    { 71, "OD roughing", "U R X Z F P Q" },
    { 72, "ID roughing", "W R X Z F P Q" },
    { 76, "Threading", "X Z P Q F I L R" },
    /* Cutter radius compensation. The panel applies it itself - the controller
       parses the group and does nothing with it - so these are the words the
       operator picks and the pad writes. */
    { 40, "Compensation off", "" },
    { 41, "Compensation left", "" },
    { 42, "Compensation right", "" },
    { 80, "Cycle end", "" },
    { 90, "Absolute distance", "" },
    { 91, "Incremental distance", "" },
    { 94, "Feed per minute", "" },
    { 95, "Feed per revolution", "" },
    { 96, "Constant surface speed", "S" },
    { 97, "Constant spindle speed", "S" },
    { 970, "Preview extents", "X U Z W" },
    { 971, "Stock setup", "X Z I E" },
    { 972, "Chuck clamp", "C" },
    { 973, "Preview mode", "P" }
};

static const ncc_gcode_info_t *ncc_gcode_info(int gcode)
{
    size_t i;

    for (i = 0u; i < sizeof(g_ncc_gcodes) / sizeof(g_ncc_gcodes[0]); i++) {
        if (g_ncc_gcodes[i].gcode == gcode) {
            return &g_ncc_gcodes[i];
        }
    }
    return 0;
}

const char *ncc_vocab_gcode_name(int gcode)
{
    const ncc_gcode_info_t *info = ncc_gcode_info(gcode);

    return info ? info->name : 0;
}

const char *ncc_vocab_gcode_parameters(int gcode)
{
    const ncc_gcode_info_t *info = ncc_gcode_info(gcode);

    return info ? info->parameters : 0;
}

const char *ncc_vocab_gcode_template(int gcode)
{
    switch (gcode) {
    case 0: return "G0 X0 Z0";
    case 1: return "G1 X0 Z0 F0";
    case 2: return "G2 X0 Z0 R0 I0 K0 F0";
    case 3: return "G3 X0 Z0 R0 I0 K0 F0";
    case 4: return "G4 P0";
    case 28: return "G28 X0 Z0";
    case 30: return "G30 X0 Z0";
    case 33: return "G33 X0 Z0 K0 F0";
    case 40: return "G40";
    case 41: return "G41";
    case 42: return "G42";
    case 70: return "G70 P0 Q0";
    case 71: return "G71 U0 R0 X0 Z0 F0 P0 Q0";
    case 72: return "G72 W0 R0 X0 Z0 F0 P0 Q0";
    case 76: return "G76 X0 Z0 P0 Q0 F0 I0 L0 R0";
    case 80: return "G80";
    case 90: return "G90";
    case 91: return "G91";
    case 94: return "G94";
    case 95: return "G95";
    case 96: return "G96 S0";
    case 97: return "G97 S0";
    case 970: return "G970 X0 U0 Z0 W0";
    case 971: return "G971 X0 Z0 I0 E0";
    case 972: return "G972 C0";
    case 973: return "G973 P0";
    default: return 0;
    }
}

/* The G code the line names, or -1 when it names none. */
static int ncc_vocab_line_gcode(const char *line)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int i;

    count = ncc_fields(line, fields, NCC_MAX_FIELDS);
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
static bool ncc_vocab_line_is_tool(const char *line)
{
    ncc_field_t fields[NCC_MAX_FIELDS];
    int count;
    int i;
    bool has_t = false;

    count = ncc_fields(line, fields, NCC_MAX_FIELDS);
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

const char *ncc_vocab_label(const char *line, const ncc_field_t *field)
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
    gcode = ncc_vocab_line_gcode(line);
    if (gcode < 0 && ncc_vocab_line_is_tool(line)) {
        switch (letter) {
        case 'T': return "Tool number";
        case 'R': return "Radius";
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
    for (i = 0u; i < sizeof(g_ncc_vocab) / sizeof(g_ncc_vocab[0]); i++) {
        if (g_ncc_vocab[i].gcode == gcode && g_ncc_vocab[i].word == letter) {
            return g_ncc_vocab[i].label;
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
