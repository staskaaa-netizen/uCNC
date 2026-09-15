#ifndef LEANCAM_TEMPLATES_H
#define LEANCAM_TEMPLATES_H

#if defined(__GNUC__)
#define LC_TEMPLATE_UNUSED __attribute__((unused))
#else
#define LC_TEMPLATE_UNUSED
#endif

#include "leancam_menu.h"
#include "leancam_dictionary.h"
#include "../../cnc_hal_config_helper.h"

#ifndef LC_VISIBLE_PROGRAM_LINES
#define LC_VISIBLE_PROGRAM_LINES 18
#endif

#ifndef LC_VISIBLE_TOOL_LINES
#define LC_VISIBLE_TOOL_LINES 12
#endif

#ifndef LC_FILE_REFRESH_RETRY_MS
#define LC_FILE_REFRESH_RETRY_MS 1500u
#endif

#ifndef LC_AUTOSAVE_RETRY_MS
#define LC_AUTOSAVE_RETRY_MS 500u
#endif

#ifndef LC_AUTOSAVE_BUSY_MAX_TRIES
#define LC_AUTOSAVE_BUSY_MAX_TRIES 3u
#endif

/* Template syntax:
 *   {}                  required user input
 *   {(literal)}         default literal shown as value in friendly UI
 *   {(THIS.FIELD)}      default from this same cycle line
 *
 * G20/G21 are uCNC unit-mode words only. LeanCam setup/graphics uses the
 * private G970-G973 range and must not reuse G20/G21 for metadata.
 */

static const char *g_leancam_tool_template LC_TEMPLATE_UNUSED =
    LC_DICT_WORD_T "{(1)} "
    LC_DICT_WORD_R "{(0.8)} "
    LC_DICT_WORD_O "{(3)} "
    LC_DICT_WORD_F "{(120)} "
    LC_DICT_WORD_FF "{(60)} "
    LC_DICT_WORD_DOC "{(2.0)} "
    LC_DICT_WORD_FDOC "{(0.5)} "
    LC_DICT_WORD_S "{(800)} "
    LC_DICT_WORD_XO "{(0)} "
    LC_DICT_WORD_ZO "{(0)}";

static const char *g_leancam_process_call_template LC_TEMPLATE_UNUSED =
    "PROCESSCALL N{}";

/* Raw stored G-code-like templates:
 *   G1.C is chamfer.
 *   G1.R is tangent radius / rounding.
 *   G1 C and R are mutually exclusive.
 *   G2/G3 are explicit arcs, not tangent corner shortcuts.
 */
static const char *g_leancam_gcode_templates[] = {
LC_DICT_COMMAND_G71 " U{} " LC_DICT_WORD_R "{} X{} Z{} " LC_DICT_WORD_F "{}",
LC_DICT_COMMAND_G72 " W{} " LC_DICT_WORD_R "{} X{} Z{} " LC_DICT_WORD_F "{}",
"G1 X{} Z{} C{(0)} R{(0)}",
"G2 X{} Z{} R{}",
"G3 X{} Z{} R{}",
"G80",
"G74 Z{} K{} F{}",
"G84 Z{} PITCH{} RPM{}",
"G33 X{} Z{} K{}",
LC_DICT_COMMAND_G76 " Z{} P{} K{} J{} H{(1)} Q{(29.5)} " LC_DICT_WORD_R "{(2)}",
LC_DICT_COMMAND_G970 " X{(-5)} U{(60)} Z{(-60)} W{(5)}",
LC_DICT_COMMAND_G971 " X{(50)} Z{(50)} I{(0)} E{(0)}",
LC_DICT_COMMAND_G972 " C{(12)}",
LC_DICT_COMMAND_G973 " P{(7)}"
};

static const char *g_leancam_preset_templates[] = {
"OD T{(TOOL." LC_DICT_WORD_T ")} O{(TOOL." LC_DICT_WORD_O ")} U{(TOOL." LC_DICT_WORD_DOC ")} R{(1)} X{(TOOL." LC_DICT_WORD_FDOC ")} Z{(TOOL." LC_DICT_WORD_FDOC ")} F_R{(TOOL." LC_DICT_WORD_F ")} F_F{(TOOL." LC_DICT_WORD_FF ")} RPM{(TOOL." LC_DICT_WORD_S ")}",
"ID T{(TOOL." LC_DICT_WORD_T ")} O{(TOOL." LC_DICT_WORD_O ")} U{(TOOL." LC_DICT_WORD_DOC ")} R{(1)} X{(TOOL." LC_DICT_WORD_FDOC ")} Z{(TOOL." LC_DICT_WORD_FDOC ")} F_R{(TOOL." LC_DICT_WORD_F ")} F_F{(TOOL." LC_DICT_WORD_FF ")} RPM{(TOOL." LC_DICT_WORD_S ")}",
"FACE T{(TOOL." LC_DICT_WORD_T ")} O{(TOOL." LC_DICT_WORD_O ")} W{(TOOL." LC_DICT_WORD_DOC ")} R{(1)} X{(TOOL." LC_DICT_WORD_FDOC ")} Z{(TOOL." LC_DICT_WORD_FDOC ")} F_R{(TOOL." LC_DICT_WORD_F ")} F_F{(TOOL." LC_DICT_WORD_FF ")} RPM{(TOOL." LC_DICT_WORD_S ")}",
"RECESS T{(TOOL." LC_DICT_WORD_T ")} O{(TOOL." LC_DICT_WORD_O ")} U{(TOOL." LC_DICT_WORD_DOC ")} R{(1)} X{(TOOL." LC_DICT_WORD_FDOC ")} Z{(TOOL." LC_DICT_WORD_FDOC ")} F_R{(TOOL." LC_DICT_WORD_F ")} F_F{(TOOL." LC_DICT_WORD_FF ")} RPM{(TOOL." LC_DICT_WORD_S ")}"
};



enum
{
    LC_GCODE_TMPL_G71 = 0,
    LC_GCODE_TMPL_G72,
    LC_GCODE_TMPL_G1,
    LC_GCODE_TMPL_G2,
    LC_GCODE_TMPL_G3,
    LC_GCODE_TMPL_G80,
    LC_GCODE_TMPL_G74,
    LC_GCODE_TMPL_G84,
    LC_GCODE_TMPL_G33,
    LC_GCODE_TMPL_G76,
    LC_GCODE_TMPL_G970,
    LC_GCODE_TMPL_G971,
    LC_GCODE_TMPL_G972,
    LC_GCODE_TMPL_G973
};

enum
{
    LC_PRESET_TMPL_OD = 0,
    LC_PRESET_TMPL_ID,
    LC_PRESET_TMPL_FACE,
    LC_PRESET_TMPL_RECESS
};

typedef enum
{
    LC_TEMPLATE_ACTION_NONE = 0,
    LC_TEMPLATE_ACTION_DRAFT,
    LC_TEMPLATE_ACTION_PRESET
} lc_template_action_t;

typedef struct
{
    lc_template_action_t action;
    const char *text;
} lc_template_selection_t;

static inline const char *lc_template_catalog(lc_menu_catalog_kind_t catalog)
{
    switch (catalog)
    {
        case LC_MENU_CATALOG_TOOLS:
            return g_leancam_tool_template;
        default:
            return NULL;
    }
}

static inline lc_template_selection_t lc_template_select(lc_menu_template_t tmpl)
{
    lc_template_selection_t selection = {LC_TEMPLATE_ACTION_NONE, NULL};

    switch (tmpl)
    {
        case LC_MENU_TEMPLATE_PROCESSCALL:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_process_call_template;
            break;
        case LC_MENU_TEMPLATE_TOOL:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_tool_template;
            break;
        case LC_MENU_TEMPLATE_OD:
            selection.action = LC_TEMPLATE_ACTION_PRESET;
            selection.text = g_leancam_preset_templates[LC_PRESET_TMPL_OD];
            break;
        case LC_MENU_TEMPLATE_ID:
            selection.action = LC_TEMPLATE_ACTION_PRESET;
            selection.text = g_leancam_preset_templates[LC_PRESET_TMPL_ID];
            break;
        case LC_MENU_TEMPLATE_FACE:
            selection.action = LC_TEMPLATE_ACTION_PRESET;
            selection.text = g_leancam_preset_templates[LC_PRESET_TMPL_FACE];
            break;
        case LC_MENU_TEMPLATE_RECESS:
            selection.action = LC_TEMPLATE_ACTION_PRESET;
            selection.text = g_leancam_preset_templates[LC_PRESET_TMPL_RECESS];
            break;
        case LC_MENU_TEMPLATE_L:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G1];
            break;
        case LC_MENU_TEMPLATE_C:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G2];
            break;
        case LC_MENU_TEMPLATE_DRILL:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G74];
            break;
        case LC_MENU_TEMPLATE_TAP:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G84];
            break;
        case LC_MENU_TEMPLATE_THREAD:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G76];
            break;
        case LC_MENU_TEMPLATE_END:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G80];
            break;
        case LC_MENU_TEMPLATE_G970:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G970];
            break;
        case LC_MENU_TEMPLATE_G971:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G971];
            break;
        case LC_MENU_TEMPLATE_G972:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G972];
            break;
        case LC_MENU_TEMPLATE_G973:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_gcode_templates[LC_GCODE_TMPL_G973];
            break;
        default:
            break;
    }

    return selection;
}

#undef LC_TEMPLATE_UNUSED

#endif

