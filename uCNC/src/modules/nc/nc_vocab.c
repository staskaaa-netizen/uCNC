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
    { 71, 'U', "Depth/pass" },
    { 71, 'R', "Retract" },
    { 71, 'X', "X finish allowance" },
    { 71, 'Z', "Z finish allowance" },
    { 71, 'F', "Feed" },
    { 72, 'W', "Depth/pass" },
    { 72, 'R', "Retract" },
    { 72, 'X', "X finish allowance" },
    { 72, 'Z', "Z finish allowance" },
    { 72, 'F', "Feed" },
    { 76, 'X', "Thread end X" },
    { 76, 'Z', "Thread end Z" },
    { 76, 'P', "Thread height" },
    { 76, 'Q', "First cut" },
    { 76, 'F', "Thread lead" },
    { 76, 'D', "Taper" },
    { 76, 'H', "Spring passes" },
    { 76, 'R', "Finish allowance" },
    { 2,  'R', "Arc radius" },
    { 3,  'R', "Arc radius" },
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
        case 'R': return "Nose radius";
        case 'O': return "Tool orientation";
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
    case 'N': return "Line number";
    default: return "NC word";
    }
}
