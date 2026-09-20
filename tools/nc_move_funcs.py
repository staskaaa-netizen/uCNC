"""Move named C functions out of one source file into a new one - safely.

The tool exists because doing this by hand-editing is where the NC module's
first three extraction attempts broke: a span rule wrong by one line leaves the
file uncompilable, and the failure is buried in the diff. So:

  - spans are computed from the grammar, not from line guessing:
      * a definition's body ends at the first line that is exactly `}`;
      * a prototype (`;` before any `{`) is never a definition;
      * a `#define` is one line, whatever follows it;
      * a `typedef` ends at its own `};`;
  - every check runs *before* anything is written, and the tool refuses to
    write when a check fails:
      * each moved name is defined exactly once in the source;
      * the moved names are no longer defined in what is left;
      * named survivors (globals the caller still needs) are still there;
      * the braces of what is left still balance.

`draw_functions.txt` also carries two kinds of line that are not function names:
a `!name` marks a global that must survive the move, and `@region FIRST LAST`
moves a whole block of `#define`s (the panel's shared numbers) with the code
that reads them, comments and blank lines included, so nothing is left behind
pointing at nothing.

Usage:

    python tools\\nc_move_funcs.py --check            # report only
    python tools\\nc_move_funcs.py                    # write the move

`--check` is the dry run: it prints every number and writes nothing, so the
move can be inspected against a copy or the committed tree first.
"""
from pathlib import Path
import argparse
import re
import sys

NC = Path('uCNC/src/modules/nc')


def statement_span(lines, start):
    """End index of the statement that starts at `start`, or None."""
    i = start
    while i < len(lines):
        if ';' in lines[i]:
            return i
        if '{' in lines[i]:
            return None
        i += 1
    return None


def definition_span(lines, start):
    """End index of the definition that starts at `start`, or None."""
    i = start
    while i < len(lines) and '{' not in lines[i]:
        if ';' in lines[i]:
            return None
        i += 1
    if i >= len(lines):
        return None
    while i < len(lines):
        if lines[i] == '}':
            return i
        i += 1
    return None


def comment_top(lines, start):
    """First line of the comment block directly above `start`."""
    def is_comment_start(line):
        stripped = line.lstrip()
        return stripped.startswith('/*') or stripped.startswith('//')

    top = start - 1
    if top < 0 or not lines[top].strip():
        return start
    # The block's last line ends with a `*/` (or is a `//` line); anything else
    # - a closing brace, a blank line - is not a comment and must not travel.
    if not (lines[top].strip().endswith('*/') or lines[top].lstrip().startswith('//')):
        return start
    while top > 0 and not is_comment_start(lines[top]):
        if not lines[top].strip():
            break
        top -= 1
    return top if is_comment_start(lines[top]) else start


def typedef_span(lines, start):
    """End of a typedef: `};` or `} name;` - not the next line with a `;`."""
    i = start
    while i < len(lines):
        if re.match(r'^\}\s*\w*\s*;$', lines[i]):
            return i
        i += 1
    return None


def find_definitions(lines):
    found = {}
    start_re = re.compile(r'^(?:static\s+)?[A-Za-z_][\w \*]*?([A-Za-z_]\w*)\s*\(')
    i = 0
    while i < len(lines):
        m = start_re.match(lines[i])
        if m:
            end = definition_span(lines, i)
            if end is not None:
                found[m.group(1)] = (i, end)
                i = end + 1
                continue
        i += 1
    return found


def braces_balanced(lines):
    depth = 0
    for line in lines:
        for char in line:
            if char == '{':
                depth += 1
            elif char == '}':
                depth -= 1
                if depth < 0:
                    return False
    return depth == 0


def find_define(lines, name):
    """Index of `#define <name>` in `lines`, or None."""
    pattern = re.compile(r'^#define\s+' + name + r'\b')
    return next((k for k, line in enumerate(lines) if pattern.match(line)), None)


def insert_include(lines, include):
    """Put `include` into the include block, after the file's own header."""
    if any(line.strip() == include for line in lines):
        return
    target = include.split('"')[1]
    last = 0
    for k, line in enumerate(lines):
        stripped = line.strip()
        match = re.match(r'#include "(nc_[\w]+\.h)"', stripped)
        if not match:
            continue
        last = k
        if k and match.group(1) > target:
            lines.insert(k, include)
            return
    lines.insert(last + 1, include)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='report only')
    parser.add_argument('--source', default=str(NC / 'nc_visual.c'))
    parser.add_argument('--target-c', default=str(NC / 'nc_draw.c'))
    parser.add_argument('--target-h', default=str(NC / 'nc_draw.h'))
    parser.add_argument('--target-layout', default=str(NC / 'nc_layout.h'))
    parser.add_argument('--functions', default=str(NC / 'tests' / 'draw_functions.txt'))
    args = parser.parse_args()

    source = Path(args.source)
    lines = source.read_text(encoding='utf-8').split('\n')
    listed = [n.strip() for n in Path(args.functions).read_text().split('\n')
              if n.strip() and not n.strip().startswith('#')]
    survivors = [n[1:] for n in listed if n.startswith('!')]
    regions = [tuple(n.split()[1:3]) for n in listed if n.startswith('@region')]
    wanted = [n for n in listed if not n.startswith(('!', '@'))]
    wanted = [n for n in wanted if not n.startswith('!')]

    definitions = find_definitions(lines)
    missing = [n for n in wanted if n not in definitions]
    if missing:
        sys.exit(f'refusing: not found in {source}: {missing}')

    spans = sorted((definitions[n][0], definitions[n][1], n) for n in wanted)
    for k in range(1, len(spans)):
        if spans[k][0] <= spans[k - 1][1]:
            sys.exit(f'refusing: overlapping spans around {spans[k][2]}')

    moved = []
    prototypes = []
    for start, end, name in spans:
        top = comment_top(lines, start)
        body = lines[top:end + 1]
        body[0] = re.sub(r'^static\s+', '', body[0])
        moved.append((top, end, name, '\n'.join(body)))
        signature = ' '.join(line.strip() for line in lines[start:end + 1]
                             if not line.startswith('}'))
        signature = signature.split('{')[0].strip()
        prototypes.append(re.sub(r'^static\s+', '', signature) + ';')

    removed = set()
    for top, end, _, _ in moved:
        removed.update(range(top, end + 1))

    kept = []
    i = 0
    while i < len(lines):
        if i in removed:
            i += 1
            continue
        # a moved definition's prototype goes too - statement by statement, so a
        # prototype that spans lines cannot be half removed
        if re.match(r'^static\s', lines[i]):
            end = statement_span(lines, i)
            if end is not None:
                statement = '\n'.join(lines[i:end + 1])
                if any(re.search(r'\b' + n + r'\s*\(', statement) for n in wanted):
                    i = end + 1
                    continue
        kept.append(lines[i])
        i += 1

    # The types and the define a moved signature names travel with them. The
    # list is explicit on purpose: guessing by name once lifted a typedef block
    # plus the module's state globals with it.
    lift_names = ('nc_preview_segment_t', 'nc_preview_v2_t')
    lifted = []
    region_lines = []
    for first, last in regions:
        start = find_define(kept, first)
        end = find_define(kept, last)
        if start is None or end is None or end < start:
            sys.exit(f'refusing: region {first}..{last} is not in the source')
        region_lines.extend(kept[start:end + 1])
        kept = kept[:start] + kept[end + 1:]
    i = 0
    while i < len(kept):
        line = kept[i]
        m = re.match(r'^typedef\s+(?:enum|struct)\s*\{?', line)
        if m:
            end = typedef_span(kept, i)
            if end is not None:
                block = '\n'.join(kept[i:end + 1])
                name_match = re.search(r'\}\s*(\w+)\s*;', block)
                if name_match and name_match.group(1) in lift_names:
                    lifted.extend(kept[i:end + 1])
                    kept = kept[:i] + kept[end + 1:]
                    continue
        i += 1

    # The file that stays is a *user* of the moved vocabulary - it calls the
    # moved functions and reads the numbers they were moved with - so it has to
    # include the new headers either way.
    for include in ('#include "nc_layout.h"', '#include "nc_draw.h"'):
        insert_include(kept, include)

    moved_text = '\n'.join(text for _, _, _, text in moved)
    remaining = '\n'.join(kept)

    errors = []
    for name in wanted:
        if re.search(r'^(?:static\s+)?[A-Za-z_][\w \*]*?\b' + name + r'\s*\([^;]*\)\s*\{',
                     remaining, re.M):
            errors.append(f'{name} is still defined in the source')
    for name in survivors:
        if name not in remaining:
            errors.append(f'survivor {name} is missing from what is left')
    for name in wanted:
        if name not in moved_text:
            errors.append(f'{name} did not make it into the target')
    for first, last in regions:
        for name in (first, last):
            if find_define(region_lines, name) is None:
                errors.append(f'the shared numbers did not carry {name}')
    if not braces_balanced(kept):
        errors.append('braces do not balance after the move')
    if not braces_balanced(moved_text.split('\n')):
        errors.append('braces do not balance in the moved text')

    print(f'{source}: {len(lines)} lines, '
          f'{sum(e - s + 1 for s, e, _, _ in moved)} moved from '
          f'{len(moved)} functions -> {len(kept)} lines left')
    print(f'lifted with them: {len(lifted)} lines of types, '
          f'{len(region_lines)} lines of shared numbers')
    print(f'checks: {len(errors)} error(s)')
    for error in errors:
        print(f'  FAIL {error}')
    if errors or args.check:
        if args.check and not errors:
            print('dry run: nothing written')
        else:
            sys.exit(1)
        return

    target_c = Path(args.target_c)
    target_h = Path(args.target_h)
    target_layout = Path(args.target_layout)
    header = (f'#ifndef {target_h.stem.upper()}_H\n#define {target_h.stem.upper()}_H\n\n'
              '/* The panel\'s drawing vocabulary, moved out of nc_visual.c when that\n'
              '   file passed the size trigger: the screens and the preview both draw\n'
              '   with these and none of them owns state - data in, pixels out. The\n'
              '   numbers they draw with (nc_layout.h) and the footer items they label\n'
              '   (nc_menu.h) come with them. Sources inside the NC module, never a\n'
              '   module of its own.\n\n'
              '   The 3x3 grid drawn by nc_visual_draw_modal_items() is deliberately\n'
              '   here: MANUAL\'s jog pad, EDIT\'s floating helper and the planned path\n'
              '   builder are three users of one visual, not three drawings. */\n\n'
              '#include <stdbool.h>\n#include <stdint.h>\n\n'
              '#include "nc_layout.h"\n#include "nc_menu.h"\n#include "nc_palette.h"\n'
              '#include "nc_preview.h"\n#include "nc_tools.h"\n\n'
              + '\n'.join(lifted) + '\n\n'
              + '\n'.join(sorted(prototypes)) + '\n\n#endif\n')
    layout_h = (f'#ifndef {target_layout.stem.upper()}_H\n'
                f'#define {target_layout.stem.upper()}_H\n\n'
                '/* The panel\'s shared numbers: how wide the panes are, where the\n'
                '   footer starts, how big a 3x3 key is, how much stock the preview\n'
                '   holds. They left nc_visual.c with the drawing code that reads\n'
                '   them and belong to neither half on their own - the screens\n'
                '   place things with them, nc_draw.c paints with them. */\n\n'
                '#include "../lvds_renderer/lvds_hstx.h"\n\n'
                + '\n'.join(region_lines) + '\n\n#endif\n')
    body = ('/* The panel\'s drawing vocabulary - see nc_draw.h. */\n'
            '#include "nc_draw.h"\n\n#include <math.h>\n#include <stdio.h>\n'
            '#include <string.h>\n\n#include "../../cnc.h"\n'
            '#include "../lvds_renderer/lvds_draw_api.h"\n'
            '#include "../lvds_renderer/lvds_hstx.h"\n\n'
            + '\n\n'.join(text for _, _, _, text in moved) + '\n')
    target_layout.write_text(layout_h, encoding='utf-8', newline='\n')
    target_h.write_text(header, encoding='utf-8', newline='\n')
    target_c.write_text(body, encoding='utf-8', newline='\n')
    source.write_text('\n'.join(kept), encoding='utf-8', newline='\n')
    print(f'wrote {target_c}, {target_h} and {target_layout}; '
          f'{source} now {len(kept)} lines')


if __name__ == '__main__':
    main()
