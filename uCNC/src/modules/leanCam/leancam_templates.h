#ifndef LEANCAM_TEMPLATES_H
#define LEANCAM_TEMPLATES_H

#if defined(__GNUC__)
#define LC_TEMPLATE_UNUSED __attribute__((unused))
#else
#define LC_TEMPLATE_UNUSED
#endif

#include "leancam_menu.h"
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
 *   {(SETUP.FIELD)}     default from setup line
 *   {(THIS.FIELD)}      default from this same cycle line
 */

static const char *g_leancam_setup_template =
    "SETUP L{} OD{} ID{(0)} CLAMP{(0)} EXTRA{(0)} CLR{(1)}";

static const char *g_leancam_tool_template LC_TEMPLATE_UNUSED =
    "TOOL T{(1)} R{(0.8)} ORIENT{(3)} R_FEED{(120)} FIN_FEED{(60)} DOC{(2.0)} FIN_DOC{(0.5)} RPM{(800)} XOFF{(0)} ZOFF{(0)}";

static const char *g_leancam_tool_call_template LC_TEMPLATE_UNUSED =
    "TOOLCALL T{(1)} R_FEED{(TOOL.R_FEED)} FIN_FEED{(TOOL.FIN_FEED)} DOC{(TOOL.DOC)} FIN_DOC{(TOOL.FIN_DOC)} RPM{(TOOL.RPM)}";

static const char *g_leancam_process_call_template LC_TEMPLATE_UNUSED =
    "PROCESSCALL N{}";

/* Raw stored G-code-like templates:
 *   G1.C is chamfer.
 *   G1.R is tangent radius / rounding.
 *   G1 C and R are mutually exclusive.
 *   G2/G3 are explicit arcs, not tangent corner shortcuts.
 */
static const char *g_leancam_gcode_templates[] = {
"G71 U{} R{} X{} Z{} F{}",
"G72 W{} R{} X{} Z{} F{}",
"G1 X{} Z{} C{(0)} R{(0)}",
"G2 X{} Z{} R{}",
"G3 X{} Z{} R{}",
"G80",
"G74 Z{} K{} F{}",
"G84 Z{} PITCH{} RPM{}",
"G33 X{} Z{} K{}",
"G76 Z{} P{} K{} J{} H{(1)} Q{(29.5)} R{(2)}"
};

static const char *g_leancam_preset_templates[] = {
"OD T{(TOOLCALL.T)} O{(TOOL.ORIENT)} U{(TOOLCALL.DOC)} R{(SETUP.CLR)} X{(TOOLCALL.FIN_DOC)} Z{(TOOLCALL.FIN_DOC)} F_R{(TOOLCALL.R_FEED)} F_F{(TOOLCALL.FIN_FEED)} RPM{(TOOLCALL.RPM)}",
"ID T{(TOOLCALL.T)} O{(TOOL.ORIENT)} U{(TOOLCALL.DOC)} R{(SETUP.CLR)} X{(TOOLCALL.FIN_DOC)} Z{(TOOLCALL.FIN_DOC)} F_R{(TOOLCALL.R_FEED)} F_F{(TOOLCALL.FIN_FEED)} RPM{(TOOLCALL.RPM)}",
"FACE T{(TOOLCALL.T)} O{(TOOL.ORIENT)} W{(TOOLCALL.DOC)} R{(SETUP.CLR)} X{(TOOLCALL.FIN_DOC)} Z{(TOOLCALL.FIN_DOC)} F_R{(TOOLCALL.R_FEED)} F_F{(TOOLCALL.FIN_FEED)} RPM{(TOOLCALL.RPM)}",
"RECESS T{(TOOLCALL.T)} O{(TOOL.ORIENT)} U{(TOOLCALL.DOC)} R{(SETUP.CLR)} X{(TOOLCALL.FIN_DOC)} Z{(TOOLCALL.FIN_DOC)} F_R{(TOOLCALL.R_FEED)} F_F{(TOOLCALL.FIN_FEED)} RPM{(TOOLCALL.RPM)}"
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
    LC_GCODE_TMPL_G76
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

static inline const char *lc_template_setup(void)
{
    return g_leancam_setup_template;
}

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
        case LC_MENU_TEMPLATE_TOOLCALL:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_tool_call_template;
            break;
        case LC_MENU_TEMPLATE_PROCESSCALL:
            selection.action = LC_TEMPLATE_ACTION_DRAFT;
            selection.text = g_leancam_process_call_template;
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
        default:
            break;
    }

    return selection;
}

#undef LC_TEMPLATE_UNUSED

#endif

