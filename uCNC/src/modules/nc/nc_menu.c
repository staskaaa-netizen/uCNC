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
    /* The entries that are *not* text: the rest of every pad is the card's
       (`nc_presets.c`), filled into the slot its id names. `3` is the word pad -
       the field that writes one line by its name or number, plus whatever else
       the card puts beside it (`CHMF`, `RND`, ...). */
    { '1', "OPS", NC_FOOTER_ACTION_OPS },
    { '2', "TOOL", NC_FOOTER_ACTION_TOOL_MENU },
    { '3', "WORD", NC_FOOTER_ACTION_GCODE },
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
    { '5', "TRACE", NC_FOOTER_ACTION_PATH },
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
    /* Every entry here is the card's (`[11]` the new line, `[16]` the stock
       block): the pad is filled from the ids, so there is nothing to list. */
    { 0, 0, NC_FOOTER_ACTION_NONE }
};

static const nc_footer_item_t g_nc_submenu_tool[] = {
    /* The two entries that are not text: the `T` field and the tool table. The
       machine words (`M6`, `M3`, `M5`, `M4`) are sections - `[23]`..`[26]` - so
       the operator's own speed is what the key writes. */
    { '1', "SELECT", NC_FOOTER_ACTION_TOOL_SELECT },
    { '2', "EDIT", NC_FOOTER_ACTION_TOOL_EDIT }
};

static const nc_footer_item_t g_nc_submenu_g7x[] = {
    /* The pad's text entries are the card's sections (`[41]`..`[43]`, `[46]`,
       `[48]`); what is left here is what a key *does* rather than writes. */
    { '4', "Q", NC_FOOTER_ACTION_G7X_Q },
    { '5', "N", NC_FOOTER_ACTION_G7X_N }
};

static const nc_footer_item_t g_nc_submenu_sync[] = {
    /* `[51]`..`[53]`: every entry is the card's, so there is nothing here. */
    { 0, 0, NC_FOOTER_ACTION_NONE }
};

static const nc_footer_item_t g_nc_submenu_word[] = {
    /* The field that writes one line by its name or number (a G-code from the
       dialects's vocabulary, or a section of the card's). `[32]` and `[33]` ship
       as the two corner words, which are entries and not a second syntax: the
       card's own rows, each continuing the line above. */
    { '1', "G", NC_FOOTER_ACTION_GCODE }
};

static const nc_footer_item_t g_nc_footer_tools[] = {
    { '1', "ADD", NC_FOOTER_ACTION_TOOL },
    { '7', "INS", NC_FOOTER_ACTION_INSERT },
    { '8', "FILES", NC_FOOTER_ACTION_FILES },
    /* No save key: the tool table is written by the same idle task that writes
       the program. */
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
        /* `[61]`..`[63]`, the card's; empty like the thread pad. */
        *count = sizeof(g_nc_submenu_sync) / sizeof(g_nc_submenu_sync[0]);
        return g_nc_submenu_sync;
    case NC_FOOTER_ACTION_GCODE:
        *count = sizeof(g_nc_submenu_word) / sizeof(g_nc_submenu_word[0]);
        return g_nc_submenu_word;
    default:
        *count = 0;
        return 0;
    }
}

/* Which footer key a submenu hangs off: the first digit of every id in that pad.
   The footer is the one table that says it, so it is read rather than written
   down a second time. */
char nc_menu_submenu_digit(nc_footer_action_t parent)
{
    size_t count = 0u;
    const nc_footer_item_t *items = nc_menu_footer(NC_MODE_PROGRAM, false, &count);
    size_t i;

    for (i = 0u; i < count; i++) {
        if (items[i].action == parent && items[i].key >= '1' && items[i].key <= '9') {
            return items[i].key;
        }
    }
    return 0;
}
