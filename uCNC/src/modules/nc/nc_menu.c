#include "nc_menu.h"

static const nc_footer_item_t g_nc_footer_manual[] = {
    { 'B', "JOG-", NC_FOOTER_ACTION_JOG_NEG },
    { 'C', "JOG+", NC_FOOTER_ACTION_JOG_POS },
    { 'D', "INC", NC_FOOTER_ACTION_INCREMENT },
    { '1', "ZERO", NC_FOOTER_ACTION_ZERO },
    { '2', "TOOL", NC_FOOTER_ACTION_TOOL },
    { '3', "FILE", NC_FOOTER_ACTION_FILE },
    { '#', "SPECIAL", NC_FOOTER_ACTION_SPECIAL }
};

static const nc_footer_item_t g_nc_footer_program[] = {
    { '0', "TOOL", NC_FOOTER_ACTION_TOOL },
    { '1', "OD", NC_FOOTER_ACTION_PRESET_OD },
    { '2', "ID", NC_FOOTER_ACTION_PRESET_ID },
    { '3', "FACE", NC_FOOTER_ACTION_PRESET_FACE },
    { '4', "LINE", NC_FOOTER_ACTION_PRESET_LINE },
    { '5', "ARC", NC_FOOTER_ACTION_PRESET_ARC },
    { '6', "SETUP", NC_FOOTER_ACTION_PRESET_SETUP },
    { '7', "INS", NC_FOOTER_ACTION_INSERT },
    { '8', "FILES", NC_FOOTER_ACTION_FILES },
    { '9', "END", NC_FOOTER_ACTION_PRESET_END },
    { '#', "SAVE", NC_FOOTER_ACTION_SAVE },
    { '*', "DEL", NC_FOOTER_ACTION_DELETE }
};

static const nc_footer_item_t g_nc_footer_sim[] = {
    { 'B', "UP", NC_FOOTER_ACTION_BACK },
    { 'C', "DOWN", NC_FOOTER_ACTION_STEP },
    { '0', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '1', "STEP", NC_FOOTER_ACTION_STEP },
    { '4', "STOCK", NC_FOOTER_ACTION_STOCK },
    { '5', "PATH", NC_FOOTER_ACTION_PATH },
    { '#', "RESET", NC_FOOTER_ACTION_RESET }
};

static const nc_footer_item_t g_nc_footer_files[] = {
    { 'B', "UP", NC_FOOTER_ACTION_BACK },
    { 'C', "DOWN", NC_FOOTER_ACTION_STEP },
    { 'D', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '4', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '5', "NEW", NC_FOOTER_ACTION_NEW },
    { '6', "DEL", NC_FOOTER_ACTION_DELETE },
    { '8', "REF", NC_FOOTER_ACTION_REFRESH },
    { '#', "RUN", NC_FOOTER_ACTION_FULL },
    { '*', "BACK", NC_FOOTER_ACTION_RESET }
};

static const nc_footer_item_t g_nc_footer_mdi[] = {
    { 'B', "UP", NC_FOOTER_ACTION_BACK },
    { 'C', "DOWN", NC_FOOTER_ACTION_STEP },
    { 'D', "FIELD", NC_FOOTER_ACTION_FIELD },
    { '0', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '1', "INS", NC_FOOTER_ACTION_INSERT },
    { '2', "RUN", NC_FOOTER_ACTION_SINGLE },
    { '#', "SEND", NC_FOOTER_ACTION_SEND },
    { '*', "CLEAR", NC_FOOTER_ACTION_CLEAR },
    { '9', "SAVE", NC_FOOTER_ACTION_SAVE }
};

static const nc_footer_item_t g_nc_footer_tools[] = {
    { 'B', "UP", NC_FOOTER_ACTION_BACK },
    { 'C', "DOWN", NC_FOOTER_ACTION_STEP },
    { '0', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '1', "ADD", NC_FOOTER_ACTION_TOOL },
    { '7', "INS", NC_FOOTER_ACTION_INSERT },
    { '8', "FILES", NC_FOOTER_ACTION_FILES },
    { '9', "SAVE", NC_FOOTER_ACTION_SAVE },
    { '*', "DEL", NC_FOOTER_ACTION_DELETE }
};

static const nc_footer_item_t g_nc_footer_run[] = {
    { 'B', "UP", NC_FOOTER_ACTION_BACK },
    { 'C', "DOWN", NC_FOOTER_ACTION_STEP },
    { '0', "OPEN", NC_FOOTER_ACTION_OPEN },
    { '1', "SINGLE", NC_FOOTER_ACTION_SINGLE },
    { '2', "FROM", NC_FOOTER_ACTION_FROM },
    { '3', "FULL", NC_FOOTER_ACTION_FULL },
    { '4', "HOLD", NC_FOOTER_ACTION_HOLD },
    { '5', "STOP", NC_FOOTER_ACTION_STOP },
    { '#', "RUN", NC_FOOTER_ACTION_SINGLE },
    { '*', "RESET", NC_FOOTER_ACTION_RESET }
};

const char *nc_menu_mode_name(nc_mode_t mode)
{
    switch (mode) {
    case NC_MODE_MANUAL: return "MANUAL";
    case NC_MODE_PROGRAM: return "EDIT";
    case NC_MODE_SIM: return "SIM";
    case NC_MODE_MDI: return "MDI";
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
    case NC_MODE_SIM:
        *count = sizeof(g_nc_footer_sim) / sizeof(g_nc_footer_sim[0]);
        return g_nc_footer_sim;
    case NC_MODE_MDI:
        *count = sizeof(g_nc_footer_mdi) / sizeof(g_nc_footer_mdi[0]);
        return g_nc_footer_mdi;
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
