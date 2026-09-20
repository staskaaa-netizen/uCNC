#include "nc_menu.h"

static const nc_footer_item_t g_nc_footer_manual[] = {
    /* MANUAL only touches the machine: the axis, the readout, zero and
       touch-off. The empty entries are not drawn but keep the slot positions:
       the physical buttons under the panel sit in the same places whatever
       screen is up. */
    { 'B', "AXIS-", NC_FOOTER_ACTION_AXIS_PREV },
    { 'C', "AXIS+", NC_FOOTER_ACTION_AXIS_NEXT },
    { 'D', "TOUCH", NC_FOOTER_ACTION_TOUCH },
    { '0', "ZERO", NC_FOOTER_ACTION_ZERO },
    { '#', "FEED", NC_FOOTER_ACTION_NONE },
    { '*', "STOP", NC_FOOTER_ACTION_NONE },
    { ' ', "", NC_FOOTER_ACTION_NONE },
    { ' ', "", NC_FOOTER_ACTION_NONE }
};

static const nc_footer_item_t g_nc_footer_program[] = {
    { '1', "OPS", NC_FOOTER_ACTION_OPS },
    { '2', "TOOL", NC_FOOTER_ACTION_TOOL_MENU },
    { '3', "G", NC_FOOTER_ACTION_GCODE },
    { '4', "G7X", NC_FOOTER_ACTION_G7X_MENU },
    { '5', "THREAD", NC_FOOTER_ACTION_SYNC_MENU },
    { '6', "PECK", NC_FOOTER_ACTION_PECK_MENU },
    /* # gives the preview the whole body - a view of the program, not a run
       and not an edit. The file list is still reached from the editor's own
       file-name row. */
    { '#', "VIEW", NC_FOOTER_ACTION_VIEW },
    { '*', "DEL", NC_FOOTER_ACTION_DELETE }
};

/* EDIT on the whole body: the preview's own keys. `#` is the way back to the
   code, and it is the same key that went full screen - a toggle, not a mode.
   No delete: while the preview is the screen the operator is looking at the
   drawing, not editing the program. */
static const nc_footer_item_t g_nc_footer_preview[] = {
    { '4', "STOCK", NC_FOOTER_ACTION_STOCK },
    { '5', "PATH", NC_FOOTER_ACTION_PATH },
    { '6', "ROUGH", NC_FOOTER_ACTION_ROUGH },
    { '7', "DIM", NC_FOOTER_ACTION_DIMS },
    { '#', "VIEW", NC_FOOTER_ACTION_VIEW }
};

static const nc_footer_item_t g_nc_footer_files[] = {
    { 'D', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '4', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '5', "NEW", NC_FOOTER_ACTION_NEW },
    { '6', "DEL", NC_FOOTER_ACTION_DELETE },
    { '8', "REF", NC_FOOTER_ACTION_REFRESH },
    { '#', "RUN", NC_FOOTER_ACTION_FULL },
    { '*', "BACK", NC_FOOTER_ACTION_RESET }
};

static const nc_footer_item_t g_nc_submenu_ops[] = {
    { '1', "INS", NC_FOOTER_ACTION_INSERT },
    { '2', "FILES", NC_FOOTER_ACTION_FILES },
    { '3', "END", NC_FOOTER_ACTION_PRESET_END },
    { '4', "SAVE", NC_FOOTER_ACTION_SAVE },
    { '5', "DEL", NC_FOOTER_ACTION_DELETE }
};

static const nc_footer_item_t g_nc_submenu_tool[] = {
    { '1', "SELECT", NC_FOOTER_ACTION_TOOL_SELECT },
    { '2', "EDIT", NC_FOOTER_ACTION_TOOL_EDIT },
    { '3', "M6", NC_FOOTER_ACTION_TOOL_CHANGE },
    { '4', "M3", NC_FOOTER_ACTION_SPINDLE_ON },
    { '5', "STOP", NC_FOOTER_ACTION_SPINDLE_STOP },
    { '6', "M4", NC_FOOTER_ACTION_SPINDLE_CCW }
};

static const nc_footer_item_t g_nc_submenu_g7x[] = {
    { '1', "OD", NC_FOOTER_ACTION_PRESET_OD },
    { '2', "ID", NC_FOOTER_ACTION_PRESET_ID },
    { '3', "FACE", NC_FOOTER_ACTION_PRESET_FACE },
    { '4', "Q", NC_FOOTER_ACTION_G7X_Q },
    { '5', "N", NC_FOOTER_ACTION_G7X_N },
    { '6', "G80", NC_FOOTER_ACTION_PRESET_END }
};

static const nc_footer_item_t g_nc_submenu_sync[] = {
    { '1', "OD", NC_FOOTER_ACTION_THREAD_OD },
    { '2', "ID", NC_FOOTER_ACTION_THREAD_ID },
    { '3', "TAP", NC_FOOTER_ACTION_TAP }
};

static const nc_footer_item_t g_nc_submenu_peck[] = {
    { '1', "DRILL", NC_FOOTER_ACTION_PECK_DRILL },
    { '2', "PECK", NC_FOOTER_ACTION_PECK_PECK },
    { '3', "DWELL", NC_FOOTER_ACTION_PECK_DWELL }
};

static const nc_footer_item_t g_nc_footer_tools[] = {
    { '1', "ADD", NC_FOOTER_ACTION_TOOL },
    { '7', "INS", NC_FOOTER_ACTION_INSERT },
    { '8', "FILES", NC_FOOTER_ACTION_FILES },
    { '9', "SAVE", NC_FOOTER_ACTION_SAVE },
    { '*', "DEL", NC_FOOTER_ACTION_DELETE }
};

static const nc_footer_item_t g_nc_footer_run[] = {
    /* No `*` here: that key is the delete key on the screens that edit a
       document, and on RUN it deleted a line of the program being run. A
       running screen has no business deleting program text. */
    { '1', "SINGLE", NC_FOOTER_ACTION_SINGLE },
    { '2', "FROM", NC_FOOTER_ACTION_FROM },
    { '3', "FULL", NC_FOOTER_ACTION_FULL },
    { '4', "HOLD", NC_FOOTER_ACTION_HOLD },
    { '5', "STOP", NC_FOOTER_ACTION_STOP },
    { '6', "DIM", NC_FOOTER_ACTION_DIMS },
    /* Reset the run and read the program back off the card, so a file edited
       since it was opened - or the setup block in it - is what runs next. */
    { '#', "RELOAD", NC_FOOTER_ACTION_RESET }
};

const char *nc_menu_mode_name(nc_mode_t mode)
{
    switch (mode) {
    case NC_MODE_MANUAL: return "MANUAL";
    case NC_MODE_PROGRAM: return "EDIT";
    case NC_MODE_TOOLS: return "TOOLS";
    case NC_MODE_RUN: return "RUN";
    default: return "?";
    }
}

const nc_footer_item_t *nc_menu_footer(nc_mode_t mode, bool file_view, size_t *count)
{
    if (!count) {
        return 0;
    }
    if (file_view) {
        *count = sizeof(g_nc_footer_files) / sizeof(g_nc_footer_files[0]);
        return g_nc_footer_files;
    }

    switch (mode) {
    case NC_MODE_PROGRAM:
        *count = sizeof(g_nc_footer_program) / sizeof(g_nc_footer_program[0]);
        return g_nc_footer_program;
    case NC_MODE_TOOLS:
        *count = sizeof(g_nc_footer_tools) / sizeof(g_nc_footer_tools[0]);
        return g_nc_footer_tools;
    case NC_MODE_RUN:
        *count = sizeof(g_nc_footer_run) / sizeof(g_nc_footer_run[0]);
        return g_nc_footer_run;
    case NC_MODE_MANUAL:
    default:
        *count = sizeof(g_nc_footer_manual) / sizeof(g_nc_footer_manual[0]);
        return g_nc_footer_manual;
    }
}

const nc_footer_item_t *nc_menu_preview_footer(size_t *count)
{
    if (!count) {
        return 0;
    }
    *count = sizeof(g_nc_footer_preview) / sizeof(g_nc_footer_preview[0]);
    return g_nc_footer_preview;
}

const nc_footer_item_t *nc_menu_submenu(nc_footer_action_t parent, size_t *count)
{
    if (!count) {
        return 0;
    }
    switch (parent) {
    case NC_FOOTER_ACTION_OPS:
        *count = sizeof(g_nc_submenu_ops) / sizeof(g_nc_submenu_ops[0]);
        return g_nc_submenu_ops;
    case NC_FOOTER_ACTION_TOOL_MENU:
        *count = sizeof(g_nc_submenu_tool) / sizeof(g_nc_submenu_tool[0]);
        return g_nc_submenu_tool;
    case NC_FOOTER_ACTION_G7X_MENU:
        *count = sizeof(g_nc_submenu_g7x) / sizeof(g_nc_submenu_g7x[0]);
        return g_nc_submenu_g7x;
    case NC_FOOTER_ACTION_SYNC_MENU:
        *count = sizeof(g_nc_submenu_sync) / sizeof(g_nc_submenu_sync[0]);
        return g_nc_submenu_sync;
    case NC_FOOTER_ACTION_PECK_MENU:
        *count = sizeof(g_nc_submenu_peck) / sizeof(g_nc_submenu_peck[0]);
        return g_nc_submenu_peck;
    default:
        *count = 0;
        return 0;
    }
}
