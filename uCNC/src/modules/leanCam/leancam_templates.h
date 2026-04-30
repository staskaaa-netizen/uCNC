#ifndef LEANCAM_TEMPLATES_H
#define LEANCAM_TEMPLATES_H

#if defined(__GNUC__)
#define LC_TEMPLATE_UNUSED __attribute__((unused))
#else
#define LC_TEMPLATE_UNUSED
#endif

/* Template syntax:
 *   {}                  required user input
 *   {(literal)}         default literal shown as value in friendly UI
 *   {(SETUP.FIELD)}     default from setup line
 *   {(THIS.FIELD)}      default from this same cycle line
 */

static const char *g_leancam_setup_template =
    "SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLEAR{(1)}|MAT{(ST45)}|UNITS{(mm)}|WOFF{(G54)}";

static const char *g_leancam_tool_template LC_TEMPLATE_UNUSED =
    "TOOL|T{(1)}|RPM{(800)}|ROUGH_FEED{(120)}|FIN_FEED{(60)}|ROUGH_DOC{(2.0)}|FIN_DOC{(0.5)}";

static const char *g_leancam_templates[] = {
    "OD|D1{(SETUP.OD)}|Z1{(0)}|Z2{(THIS.Z1)}|D2{(THIS.D1)}|CLEAR{(SETUP.CLEAR)}",
"ID|D1{(SETUP.ID)}|Z1{(0)}|Z2{(THIS.Z1)}|D2{(THIS.D1)}|CLEAR{(SETUP.CLEAR)}",
"FACE|D{(SETUP.OD)}|Z1{(0)}|Z{}|DOC{(1.0)}|CLEAR{(SETUP.CLEAR)}",
"DRILL|Z1{(0)}|DEPTH{}|PECK{(0)}|FEED{(120)}|RPM{(800)}",
"TAP|Z1{(0)}|DEPTH{}|PITCH{}|RPM{(300)}",
"CUT|D{}|Z{}|WIDTH{}|DOC{(1.0)}|CLEAR{(SETUP.CLEAR)}",
"CHAMFER|D{}|Z{}|SIZE{(1.0)}|CLEAR{(SETUP.CLEAR)}",
"THREAD_OD|D{}|Z2{}|PITCH{}|THR_DEPTH{}|Z1{(0)}|CLEAR{(SETUP.CLEAR)}",
"THREAD_ID|D{}|Z2{}|PITCH{}|THR_DEPTH{}|Z1{(0)}|CLEAR{(SETUP.CLEAR)}",
"RADIUS_OD|D{}|Z1{(0)}|Z2{}|R{}|CLEAR{(SETUP.CLEAR)}",
"RADIUS_ID|D{}|Z1{(0)}|Z2{}|R{}|CLEAR{(SETUP.CLEAR)}",
"GROOVE|D1{(SETUP.OD)}|D2{}|Z1{}|Z2{(THIS.Z1)}|WIDTH{}|CLEAR{(SETUP.CLEAR)}",
"PART|D{(0)}|Z{}|WIDTH{}|CLEAR{(SETUP.CLEAR)}"
};



enum
{
    LC_TMPL_OD = 0,
    LC_TMPL_ID,
    LC_TMPL_FACE,
    LC_TMPL_DRILL,
    LC_TMPL_TAP,
    LC_TMPL_CUT,
    LC_TMPL_CHAMFER,
    LC_TMPL_THREAD_OD,
    LC_TMPL_THREAD_ID,
    LC_TMPL_RADIUS_OD,
    LC_TMPL_RADIUS_ID,
    LC_TMPL_GROOVE,
    LC_TMPL_PART
};

#undef LC_TEMPLATE_UNUSED

#endif
