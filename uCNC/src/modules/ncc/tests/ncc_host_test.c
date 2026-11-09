/* What `ncc` changed, on the host: the entries held in RAM and the two layouts.

   The screens themselves are nc2's, and nc2's own suite is what pins them
   (`tools/test_nc2.py`); this file exists for the three things nc2 does not
   have, so a change to any of them fails here instead of on the bench:

   - the shipped table arrives in RAM at boot, whole, and a key's rows are the
     rows the shipped entry always wrote;
   - `/D/ncc_presets.txt` is read once and overrides only the addresses it
     names, with the continuation rule the card always had;
   - the layout's bands fit inside the picture in whichever shape the build is
     for - compile it twice, `-DLVDS_PANEL_TURN=0` and not.

   Build and run it through `tools/test_ncc.py`, which knows the flags and the
   two shapes. `--files <dir>` is a scratch card, the way every station check
   takes one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_fs.h"
#include "ncc_layout.h"
#include "ncc_presets.h"

static int g_failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("ncc_host: FAIL %s\n", what);
        g_failures++;
    }
}

/* The rows one address answers with, or NULL when it has none. */
static const char *rows_of(const char *address, char *name, size_t name_sz,
                           char *rows, size_t rows_sz)
{
    int count = ncc_preset_read(address, name, name_sz, rows, rows_sz);

    return count > 0 ? rows : 0;
}

/* --- the entries ---------------------------------------------------------- */

static void test_shipped(void)
{
    char name[NCC_PRESET_NAME_MAX];
    char rows[NCC_PRESET_ROWS_MAX];

    ncc_presets_init();
    check(ncc_presets_sync(), "the host card answers at once");

    check(rows_of("41", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(name, "G71") == 0 &&
          strcmp(rows, "G71 U0 R0 X0 Z0 F0 P0 Q0") == 0,
          "the shipped `41` is the OD cycle it always was");

    check(ncc_slot("", '1', name, sizeof(name)) == NCC_SLOT_PAD &&
          strcmp(name, "OPS") == 0, "the root's `1` is the OPS pad");
    check(ncc_slot("", '4', name, sizeof(name)) == NCC_SLOT_PAD &&
          strcmp(name, "G7X") == 0, "the root's `4` is the G7X pad");
    check(ncc_slot("4", '1', name, sizeof(name)) == NCC_SLOT_ENTRY &&
          strcmp(name, "G71") == 0, "G7X `1` is the OD entry");
    /* The path builder is a key that does something, not an entry. */
    check(ncc_slot("4", '7', name, sizeof(name)) == NCC_SLOT_EMPTY,
          "G7X `7` is not an entry (the path builder)");
    check(ncc_slot("", '8', name, sizeof(name)) == NCC_SLOT_EMPTY,
          "the root's `8` is empty");

    /* An entry that writes one empty line IS an entry (the line the operator
       starts by hand), and a name over it that writes nothing is a group. */
    check(ncc_preset_read("45", name, sizeof(name), rows, sizeof(rows)) == 1 &&
          strcmp(name, "PATH") == 0 && rows[0] == '\0',
          "`45` is one empty row");
    /* A pad's name with no rows answers 0, not -1. */
    check(ncc_preset_read("1", name, sizeof(name), 0, 0u) == 0 &&
          strcmp(name, "OPS") == 0, "`1` names the pad and has no rows");
    check(ncc_preset_read("19", name, sizeof(name), 0, 0u) == -1,
          "an address nothing lives at answers -1");
}

static void write_card(const char *root)
{
    char path[512];
    FILE *fp;

    snprintf(path, sizeof(path), "%s\\ncc_presets.txt", root);
    fp = fopen(path, "wb");
    if (!fp) {
        printf("ncc_host: FAIL cannot write %s\n", path);
        g_failures++;
        return;
    }
    /* 41 and 34 change; 11 is renamed only (its shipped rows must survive);
       32 shows the continuation rule; 99 is an address nothing ships. */
    fputs("[41] MY OD\nG71 U1 R0.5 X0 Z0 F0 P0 Q0\n"
          "[34] MY INC\n U2\n"
          "[11] RENAMED\n"
          "[32] CH\n C0.5\n"
          "[99] MINE\nG0 X0 Z0\n", fp);
    fclose(fp);
}

static void test_card_file(const char *root)
{
    char name[NCC_PRESET_NAME_MAX];
    char rows[NCC_PRESET_ROWS_MAX];

    write_card(root);
    ncc_presets_init();                 /* as a reboot would */
    check(ncc_presets_sync(), "the card with the file answers");

    check(rows_of("41", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(name, "MY OD") == 0 &&
          strcmp(rows, "G71 U1 R0.5 X0 Z0 F0 P0 Q0") == 0,
          "`41` is the card's entry, not the shipped one");
    check(rows_of("34", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(name, "MY INC") == 0 && strcmp(rows, " U2") == 0,
          "`34` keeps its leading space (it continues the row above)");
    check(rows_of("32", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(rows, " C0.5") == 0, "`32` is the card's own continuation rows");
    check(rows_of("99", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(name, "MINE") == 0 && strcmp(rows, "G0 X0 Z0") == 0,
          "an address nothing ships can be written");

    /* A block that names an address and writes no rows keeps the shipped rows. */
    check(ncc_preset_read("11", name, sizeof(name), rows, sizeof(rows)) == 4 &&
          strcmp(name, "RENAMED") == 0 &&
          strcmp(rows, "G970 X0 U0 Z0 W0\nG971 X0 Z0 I0 E0\nG972 C0\nG973 P0") == 0,
          "`11` was renamed and kept its shipped rows");
    check(rows_of("23", name, sizeof(name), rows, sizeof(rows)) &&
          strcmp(name, "M6") == 0 && strcmp(rows, "M6") == 0,
          "an address the card does not name keeps the shipped entry");
}

/* The shipped file (`--dump-presets` wrote it) loaded back: every address must
   answer exactly what the table says, groups, blank-row entries and all. This
   is what keeps the release's readable copy and the module's table from
   drifting. */
static void test_shipped_file(const char *root, const char *shipped)
{
    char path[512];
    char name[NCC_PRESET_NAME_MAX];
    char rows[NCC_PRESET_ROWS_MAX];
    char got_name[NCC_PRESET_NAME_MAX];
    char got_rows[NCC_PRESET_ROWS_MAX];
    size_t i;
    FILE *in;
    FILE *out;
    char buffer[512];
    size_t got;

    snprintf(path, sizeof(path), "%s\\ncc_presets.txt", root);
    in = fopen(shipped, "rb");
    if (!in) {
        printf("ncc_host: FAIL cannot read the shipped file %s\n", shipped);
        g_failures++;
        return;
    }
    out = fopen(path, "wb");
    if (!out) {
        fclose(in);
        printf("ncc_host: FAIL cannot write %s\n", path);
        g_failures++;
        return;
    }
    while ((got = fread(buffer, 1u, sizeof(buffer), in)) > 0u) {
        fwrite(buffer, 1u, got, out);
    }
    fclose(in);
    fclose(out);

    ncc_presets_init();                 /* as a reboot would */
    check(ncc_presets_sync(), "the card with the shipped file answers");
    for (i = 0u; i < ncc_preset_builtin_count(); i++) {
        const char *address = 0;
        const char *want_name = 0;
        const char *want_rows = 0;
        int count;

        if (!ncc_preset_builtin(i, &address, &want_name, &want_rows)) {
            continue;
        }
        count = ncc_preset_read(address, got_name, sizeof(got_name),
                                got_rows, sizeof(got_rows));
        if (want_rows) {
            check(count > 0 && strcmp(got_name, want_name) == 0 &&
                  strcmp(got_rows, want_rows) == 0,
                  "the shipped file reads back as the table");
        } else {
            check(count == 0 && strcmp(got_name, want_name) == 0,
                  "the shipped file keeps a group a group");
        }
        if (g_failures && count < 0) {
            break;                      /* one report is enough */
        }
    }
}

/* --- the layout ----------------------------------------------------------- */

/* Every band inside the picture, and none on top of another. The drawing, the
   strip and the bottom band are the three that share the height, so they are
   the ones that have to be checked in whichever shape this build is. */
static void test_layout(void)
{
    char what[96];

    check(NCC_PREVIEW_X >= 0 && NCC_PREVIEW_Y >= NCC_HEADER_H &&
          NCC_PREVIEW_X + NCC_PREVIEW_W <= LVDS_VIEW_WIDTH,
          "the drawing is inside the picture");
    check(NCC_PREVIEW_Y + NCC_PREVIEW_PANE_H <= NCC_DRO_Y,
          "the drawing stops above the strip");
    check(NCC_DRO_Y + NCC_DRO_H <= NCC_BODY_Y && NCC_BODY_Y < NCC_BODY_BOTTOM &&
          NCC_BODY_BOTTOM <= LVDS_VIEW_HEIGHT,
          "the strip sits between the drawing and the bottom band");
    check(NCC_TEXT_X + NCC_TEXT_W <= NCC_PAD_X &&
          NCC_PAD_X + NCC_PAD_W <= LVDS_VIEW_WIDTH,
          "the text pane and the pad both fit the width");
    check(NCC_PAD_Y + NCC_PAD_H <= NCC_BODY_BOTTOM && NCC_PAD_Y >= NCC_BODY_Y,
          "the pad is inside the bottom band");
    check(NCC_NOTES_H > 0 && NCC_NOTES_X + NCC_NOTES_W <= LVDS_VIEW_WIDTH &&
          NCC_TEXT_Y + NCC_TEXT_H <= NCC_BODY_BOTTOM,
          "the notes and the text pane fit the bottom band");
    check(NCC_CODE_ROWS >= 6, "the text pane holds a program");

    snprintf(what, sizeof(what),
             "the shape is %dx%d, %s (%d program rows)",
             (int)LVDS_VIEW_WIDTH, (int)LVDS_VIEW_HEIGHT,
             LVDS_VIEW_WIDTH < LVDS_VIEW_HEIGHT ? "portrait" : "landscape",
             NCC_CODE_ROWS);
    printf("ncc_host: %s\n", what);
}

int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : "ncc-test-files";
    const char *shipped = (argc > 2) ? argv[2] : 0;

    if (!host_fs_mount(root)) {
        puts("ncc_host: FAIL the host card did not mount");
        return 1;
    }
    test_shipped();
    test_card_file(root);
    if (shipped) {
        test_shipped_file(root, shipped);
    }
    test_layout();
    if (g_failures) {
        printf("ncc_host: %d check(s) failed\n", g_failures);
        return 1;
    }
    puts("ncc_host: PASS the entries are in RAM, the card file overrides, and the bands fit");
    return 0;
}
