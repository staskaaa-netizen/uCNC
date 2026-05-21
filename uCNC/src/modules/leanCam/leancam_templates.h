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
    "SETUP|L{}|OD{}|ID{(0)}|CLAMP{(0)}|EXTRA{(0)}|CLR{(1)}";

static const char *g_leancam_tool_template LC_TEMPLATE_UNUSED =
    "TOOL|T{(1)}|R{(0.8)}|ORIENT{(3)}|R_FEED{(120)}|FIN_FEED{(60)}|DOC{(2.0)}|FIN_DOC{(0.5)}|XOFF{(0)}|ZOFF{(0)}";

/* Q retract mode:
 *   Q0 direct diagonal rapid out
 *   Q1 X first, then Z
 *   Q2 Z first, then X
 */
static const char *g_leancam_templates[] = {
"OD|T{(1)}|D1{(SETUP.OD)}|Z1{(0)}|Z2{(-50)}|D2{(THIS.D1)}|RND{(0)}|CHMF{(0)}|DT{(THIS.D2)}|Q{(0)}",
"ID|T{(1)}|D1{(10)}|Z1{(0)}|Z2{(-50)}|D2{(20)}|RND{(0)}|CHMF{(0)}|DT{(THIS.D2)}|Q{(2)}",
"FACE|T{(1)}|D{(SETUP.OD)}|Z1{(1)}|Z{(0)}|Q{(0)}",
"DRILL|T{(1)}|Z1{(0)}|DEPTH{}|PECK{(0)}|FEED{(120)}|S{(800)}",
"TAP|T{(1)}|Z1{(0)}|DEPTH{}|PITCH{}|RPM{(300)}",
"CUT|T{(1)}|D{(0)}|Z{(-50)}|WIDTH{(3)}|Q{(1)}",
"CHAMFER|T{(1)}|D{}|Z{}|SIZE{(1.0)}|Q{(0)}",
"THR_OD|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(0)}",
"THR_ID|T{(1)}|M{}|P{}|Z1{(0)}|Z2{(-50)}|N{(0)}|ST{(1)}|Q{(2)}",
"RADIUS_OD|T{(1)}|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(0)}",
"RADIUS_ID|T{(1)}|D{}|Z1{(0)}|Z2{(-20)}|R{}|Q{(2)}",
"GROOVE|T{(1)}|D1{(SETUP.OD)}|D2{(40)}|Z1{(-20)}|Z2{(-40)}|WIDTH{(3)}|Q{(1)}",
"PART|T{(1)}|D{(0)}|Z{(-50)}|WIDTH{(3)}|Q{(1)}"
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
    LC_TMPL_THR_OD,
    LC_TMPL_THR_ID,
    LC_TMPL_RADIUS_OD,
    LC_TMPL_RADIUS_ID,
    LC_TMPL_GROOVE,
    LC_TMPL_PART
};

#undef LC_TEMPLATE_UNUSED

#endif
