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
    "SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLR{(1)}|MAT{(ST45)}|WOFF{(G54)}";

static const char *g_leancam_tool_template LC_TEMPLATE_UNUSED =
    "TOOL|T{(1)}|D{(6)}|S{(800)}|R_FEED{(120)}|FIN_FEED{(60)}|R_DOC{(2.0)}|FIN_DOC{(0.5)}";

static const char *g_leancam_templates[] = {
    "OD|D1{(SETUP.OD)}|Z1{(0)}|Z2{(-50)}|D2{(THIS.D1)}|CLR{(SETUP.CLR)}",
"ID|D1{(10)}|Z1{(0)}|Z2{(-50)}|D2{(20)}|CLR{(SETUP.CLR)}",
"FACE|D{(SETUP.OD)}|Z1{(1)}|Z{(0)}|DOC{(1.0)}|CLR{(SETUP.CLR)}",
"DRILL|Z1{(0)}|DEPTH{}|PECK{(0)}|FEED{(120)}|S{(800)}",
"TAP|Z1{(0)}|DEPTH{}|PITCH{}|RPM{(300)}",
"CUT|D{(0)}|Z{(-50)}|WIDTH{(3)}|DOC{(1.0)}|CLR{(SETUP.CLR)}",
"CHAMFER|D{}|Z{}|SIZE{(1.0)}|CLR{(SETUP.CLR)}",
"THREAD_OD|D{}|Z2{(-50)}|PITCH{}|THR_DEPTH{}|Z1{(0)}|CLR{(SETUP.CLR)}",
"THREAD_ID|D{}|Z2{(-50)}|PITCH{}|THR_DEPTH{}|Z1{(0)}|CLR{(SETUP.CLR)}",
"RADIUS_OD|D{}|Z1{(0)}|Z2{(-20)}|R{}|CLR{(SETUP.CLR)}",
"RADIUS_ID|D{}|Z1{(0)}|Z2{(-20)}|R{}|CLR{(SETUP.CLR)}",
"GROOVE|D1{(SETUP.OD)}|D2{(40)}|Z1{(-20)}|Z2{(-40)}|WIDTH{(3)}|CLR{(SETUP.CLR)}",
"PART|D{(0)}|Z{(-50)}|WIDTH{(3)}|CLR{(SETUP.CLR)}"
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
