#!/usr/bin/env python3
"""check_comments.py -- Comment-integrity checker for .re files.

The single authoritative comment checker. `iris.py --lost` delegates here, so
the "did a comment swallow real code?" logic lives in exactly one place.

Ground truth is the loader's own `_strip_comments` -- the exact routine every
load uses. No reimplemented comment scanner (that would drift). A line's
post-strip text is what the loader really sees: if a real command line strips
to empty, its code was eaten by a comment.

Bug signal (deterministic, low-noise):
  A comment that swallows code always eats a *run* of real commands -- every
  command line from the bad `//` until parity re-balances (wax.re L203 ate
  ~60). Prose that merely *starts* with a command word ("RCALL_AT redirects
  ...", "For each ...") is isolated (one line) and carries prose tokens
  (lowercase words, ':' '(' ',' ...). So:

      >= 2 swallowed *real-command* lines in a file  ==  DEFINITE BUG.

  A single swallowed real-command line is reported as advisory (usually a
  usage example written inside a comment), never as a hard bug.

  A "real-command line" = first token is a command (primitive or NEWREF macro)
  and every token is code-like (identifier / KEY=VALUE / number / aspect) with
  no prose tokens. FOR-template bases (a token containing '{', e.g.
  `CHAIN_MODSCAN{N}`) are not real symbols and are skipped.

Usage:
    python3 check_comments.py            # definite bugs (silent+OK when clean)
    python3 check_comments.py --advisory # also show isolated/long spans
    python3 check_comments.py --spans FILE.re # unfiltered listing, one file
"""
import sys, os, re

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import _strip_comments, re_sources
from iris_common import wax_cmd_macro_names

_PRIMITIVE_CMDS = {
    'NOVA', 'NEW', 'SET', 'LINK',
    'NOLINK', 'RVOCA', 'RREDI', 'JEQ', 'JZ', 'CLEAR',
    'NOP', 'NOVAE', 'FOR', 'SWITCH', 'SAVE', 'ALLOC_TO', 'ALLOC_RAW',
}

# A code token: identifier, KEY=VALUE, signed integer, or aspect/keyword.
# Deliberately excludes prose (lowercase-only words, or anything with :(),.').
_CODE_TOK = re.compile(r'^(?:[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_]+)?|-?[0-9]+)$')
_ADVISORY_SPAN_LINES = 8


def _real_cmds():
    try:
        return _PRIMITIVE_CMDS | wax_cmd_macro_names()
    except Exception:
        return set(_PRIMITIVE_CMDS)


def _looks_like_command(raw_stripped, real):
    """True if the raw line is a genuine command line (not prose, not template)."""
    toks = raw_stripped.split()
    if len(toks) < 2:
        return False
    if toks[0].upper() not in real:
        return False
    if '{' in toks[1]:            # FOR-template base -- not a real symbol
        return False
    # every token must be code-like; a single prose token disqualifies the line
    for t in toks:
        if not _CODE_TOK.match(t):
            return False
    return True


def analyze_file(filepath, real=None):
    """Analyse one .re file. Returns dict with swallowed runs and spans."""
    real = real if real is not None else _real_cmds()
    with open(filepath, encoding='utf-8', errors='replace') as f:
        raw_lines = f.readlines()
    stripped = _strip_comments(raw_lines)
    # _strip_comments returns (lineno, text) only. The raw source line is no
    # longer carried in it (it was there for the old blank-line closing rule),
    # so this checker -- whose whole job is comparing the stripped result
    # against the raw source -- reads the raw line from its own copy.
    raw_by_lineno = {n: l.rstrip('\n') for n, l in enumerate(raw_lines, 1)}

    swallowed = []          # (lineno, text, region_start) real commands eaten
    region_start = None     # start of current empty-strip run (the opening //)
    for (lineno, text) in stripped:
        s = raw_by_lineno.get(lineno, '').lstrip()
        empty = (text.strip() == '')
        if empty:
            if region_start is None:
                region_start = lineno
            # only flag if this empty line is actually a real command in the raw
            if s and s[0] != '/' and _looks_like_command(s, real):
                swallowed.append((lineno, s[:72], region_start))
        else:
            region_start = None

    # block-comment spans (advisory)
    # The canonical rule (comments.re, COMMENTS.md): a marker is a line that
    # is exactly "//", whitespace aside -- nothing else opens or closes a
    # block. This used to test `s[:2] == '//'`, which also treated a "//text"
    # line as an opener; under the simplified rule that line is just an
    # ordinary dropped slash-line, and counting it as a marker put every
    # following span off by one block.
    spans, in_block, start = [], False, None
    for (lineno, _text) in stripped:
        orig = raw_by_lineno.get(lineno, '')
        if orig.strip() != '//':
            continue
        if not in_block:
            in_block, start = True, lineno
        else:
            spans.append((start, lineno, lineno - start + 1))
            in_block = False
    if in_block:
        spans.append((start, None, len(raw_lines) - start + 1))

    return {'file': filepath, 'swallowed': swallowed, 'spans': spans}


def analyze_all(root=None):
    real = _real_cmds()
    return [analyze_file(fp, real) for fp in re_sources(root or _ROOT)]


def definite_bug(res):
    return len(res['swallowed']) >= 2


def report(results, advisory=False):
    buggy = 0
    for res in results:
        name = os.path.basename(res['file'])
        sw = res['swallowed']
        if definite_bug(res):
            buggy += 1
            region = sw[0][2]
            print(f"\n### {name} -- DEFINITE BUG: {len(sw)} command line(s) swallowed by a comment")
            print(f"    likely cause: a '//' delimiter at/after line {region} "
                  f"(unclosed block or embedded '//' in a comment body)")
            for ln, txt, _rs in sw[:8]:
                print(f"       L{ln}: {txt}")
            if len(sw) > 8:
                print(f"       ... +{len(sw)-8} more")
    if buggy == 0:
        print("Comment integrity: clean -- no runs of swallowed commands.")
    if advisory:
        print("\n-- advisory --")
        for res in results:
            name = os.path.basename(res['file'])
            if len(res['swallowed']) == 1:
                ln, txt, _ = res['swallowed'][0]
                print(f"   {name} L{ln}: single command-like line inside a comment "
                      f"(usually a usage example): {txt}")
            for start, end, n in res['spans']:
                if end is None:
                    print(f"   {name}: block comment opened at line {start} never closed (runs to EOF)")
                elif n >= _ADVISORY_SPAN_LINES:
                    print(f"   {name}: long comment span {start}-{end} ({n} lines)")
    return buggy


def spans_report(filepath):
    """Unfiltered comment-span + swallowed-line listing for one file.

    No threshold gating, unlike `report()`/`definite_bug()` (which only
    surface >=2-swallowed-line runs as bugs and >=_ADVISORY_SPAN_LINES spans
    as advisory): every span and every swallowed line this file's real
    `_strip_comments` pass produces is shown, so a single file can be
    inspected directly against the loader's own ground truth. This is the
    surviving use case of the old `sim.py --lexer`/`--prepass` file
    inspection -- same question ("where does the real comment/token scan
    see this file's structure"), answered by calling `_strip_comments`
    itself rather than a separate reimplementation that could drift from it.
    """
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return 1
    res = analyze_file(filepath)
    name = os.path.basename(filepath)
    print(f"=== Comment spans: {name} ===\n")

    if not res['spans']:
        print("Comment blocks (//...//): none")
    else:
        print(f"Comment blocks (//...//): {len(res['spans'])}")
        for start, end, n in res['spans']:
            if end is None:
                print(f"  L{start}: opened, never closed (runs to EOF, {n} lines)")
            else:
                print(f"  L{start}-{end}: {n} line(s)")

    print()
    if not res['swallowed']:
        print("Swallowed real-command lines: none")
    else:
        print(f"Swallowed real-command lines: {len(res['swallowed'])}")
        for ln, txt, region_start in res['swallowed']:
            print(f"  L{ln} (comment opened L{region_start}): {txt}")

    return 0


def lost_symbols(root=None):
    """Structured result for iris.py --lost delegation.

    Returns {basename: [ (lineno, text), ... ]} for files with a definite
    swallowing bug (>= 2 swallowed real commands). Empty dict = clean.
    """
    out = {}
    for res in analyze_all(root):
        if definite_bug(res):
            out[os.path.basename(res['file'])] = [(ln, txt) for ln, txt, _ in res['swallowed']]
    return out


def main(argv):
    if '--spans' in argv:
        rest = [a for a in argv if a != '--spans' and not a.startswith('-')]
        if not rest:
            print("Usage: python3 check_comments.py --spans FILE.re")
            return 0
        return spans_report(rest[0])
    advisory = '--advisory' in argv
    return 1 if report(analyze_all(), advisory=advisory) else 0


# ── iris hub contract ─────────────────────────────────────────────────────
# check_comments stays a standalone file (the authoritative comment
# analyser, large and self-contained) but plugs into the iris hub via these
# commands. NEEDS="text": pure source analysis, no build.
from iris import Command


def _cmd_comments(ctx) -> int:
    """Comment integrity: any run of real command lines swallowed by a
    comment block (>= 2 in a file == definite bug)."""
    return 1 if report(analyze_all()) else 0


def _cmd_lost(ctx) -> int:
    """Report real code swallowed by broken comment blocks. Same detection
    as the comments check, presented as a per-file listing (was diag --lost,
    which only ever delegated here)."""
    print("=== Lost code (real commands swallowed by comment blocks) ===\n")
    lost = lost_symbols(_ROOT)
    if not lost:
        print("No swallowed code — comment integrity clean  \u2713")
        return 0
    total = 0
    for fname, items in sorted(lost.items()):
        total += len(items)
        print(f"{fname}: {len(items)} swallowed —")
        for ln, txt in items[:8]:
            print(f"    L{ln}: {txt}")
        if len(items) > 8:
            print(f"    ... +{len(items) - 8} more")
    print(f"\nTotal swallowed command lines: {total}")
    return 1


def _cmd_spans(ctx, args) -> int:
    """Unfiltered comment-span + swallowed-line listing for one file, no
    threshold gating -- the surviving use case of the old `sim.py
    --lexer`/`--prepass` file inspection, built on the same
    `_strip_comments` ground truth instead of a separate reimplementation."""
    targets = [x for x in args if not x.startswith('-')]
    if not targets:
        print("Usage: python3 iris.py --spans FILE.re")
        return 0
    target_path = targets[0]
    if not os.path.isabs(target_path):
        target_path = os.path.join(_ROOT, target_path)
    return spans_report(target_path)


COMMANDS = [
    Command("comments", "text", _cmd_comments, is_check=True,
            help="find real command lines swallowed by comment blocks"),
    Command("lost", "text", _cmd_lost, is_check=False,
            help="list code swallowed by broken comment blocks"),
    Command("spans", "text", _cmd_spans, is_check=False, takes_args=True,
            help="unfiltered comment-span + swallowed-line listing for one file"),
]


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
