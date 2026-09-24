#include "nc_vocab.h"
#include "nc_tools.h"

#include <ctype.h>
#include <stdio.h>

typedef struct {
    int gcode;
    char word;
    const char *label;
} nc_vocab_entry_t;

static const nc_vocab_entry_t g_nc_vocab[] = {
    { 1,  'C', "Chamfer" },
    { 1,  'R', "Corner radius" },
    { 4,  'P', "Dwell time" },
    { 33, 'K', "Thread pitch" },
    /* The P/Q range of a cycle: P names the block the profile starts at, Q the
       one it ends at. N numbers a profile block. All three are words the panel's
       own templates write, so none of them may fall through to "NC word". */
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

typedef struct {
    int gcode;
    const char *name;
    const char *parameters;
} nc_gcode_info_t;

static const nc_gcode_info_t g_nc_gcodes[] = {
    { 0, "Rapid move", "X Z" },
    { 1, "Linear move", "X Z F C R" },
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
    { 71, "OD roughing", "U R X Z F P Q" },
    { 72, "ID roughing", "W R X Z F P Q" },
    { 76, "Threading", "X Z P Q F I L R" },
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

const char *nc_vocab_gcode_name(int gcode)
{
    size_t i;
    for (i = 0; i < sizeof(g_nc_gcodes) / sizeof(g_nc_gcodes[0]); i++) {
        if (g_nc_gcodes[i].gcode == gcode) {
            return g_nc_gcodes[i].name;
        }
    }
    return 0;
}

const char *nc_vocab_gcode_parameters(int gcode)
{
    size_t i;
    for (i = 0; i < sizeof(g_nc_gcodes) / sizeof(g_nc_gcodes[0]); i++) {
        if (g_nc_gcodes[i].gcode == gcode) {
            return g_nc_gcodes[i].parameters;
        }
    }
    return 0;
}

const char *nc_vocab_gcode_template(int gcode)
{
    switch (gcode) {
    case 0: return "G0 X0 Z0";
    case 1: return "G1 X0 Z0 C0 R0";
    case 2: return "G2 X0 Z0 R0 I0 K0 F0";
    case 3: return "G3 X0 Z0 R0 I0 K0 F0";
    case 4: return "G4 P0";
    case 28: return "G28 X0 Z0";
    case 30: return "G30 X0 Z0";
    case 33: return "G33 X0 Z0 K0 F0";
    case 71: return "G71 U0 R0 X0 Z0 F0 P0 Q0";
    case 72: return "G72 W0 R0 X0 Z0 F0 P0 Q0";
    case 76: return "G76 X0 Z0 P0 Q0 F0 I0 L0 R0";
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

static int nc_vocab_line_gcode(const char *line)
{
    nc_word_t words[12];
    int count;
    int i;

    count = nc_parse_words(line, words, 12);
    for (i = 0; i < count; i++) {
        float value;
        if (words[i].letter == 'G') {
            if (nc_word_value(line, &words[i], &value)) {
                return (int)(value + 0.5f);
            }
            return -1;
        }
    }
    return -1;
}

const char *nc_vocab_label_for_word(const char *line, const nc_word_t *word)
{
    int gcode;
    size_t i;
    char letter;

    if (!line || !word) {
        return "";
    }

    letter = (char)toupper((unsigned char)word->letter);
    if (letter == 'G') {
        static char label[16];
        float value = 0.0f;
        (void)nc_word_value(line, word, &value);
        snprintf(label, sizeof(label), "G-code %.0f", (double)value);
        return label;
    }

    gcode = nc_vocab_line_gcode(line);
    if (gcode < 0 && nc_tool_line_is_tool(line)) {
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

    for (i = 0; i < sizeof(g_nc_vocab) / sizeof(g_nc_vocab[0]); i++) {
        if (g_nc_vocab[i].gcode == gcode && g_nc_vocab[i].word == letter) {
            return g_nc_vocab[i].label;
        }
    }

    switch (letter) {
    case 'X': return "X position";
    case 'Z': return "Z position";
    case 'F': return "Feed";
    case 'S': return "Spindle speed";
    case 'T': return "Tool";
    case 'M': return "M-code";
    case 'N': return "Block number";
    default: return "NC word";
    }
}
