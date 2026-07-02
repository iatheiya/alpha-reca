#!/usr/bin/env python3
"""check_comments.py — Block comment span finder and classifier.

Usage:
    python3 check_comments.py [--threshold N]   # find + classify suspicious spans (default)
    python3 check_comments.py --spans-only       # just list spans

For every suspicious block-comment span (>= threshold lines, or never
closed), classify it: does the swallowed region contain lines that look
like REAL commands as first token? If so, this is very likely a
missing-closing-'//' bug swallowing real code.

When a flagged line is actually legitimate prose (e.g. "NEW MyNote" as
an example), see CONVENTIONS.md "Command-like words inside prose" for
the quoting convention used to silence the false positive.
"""
import sys, os, glob

# Path discovery: works whether this file is at project root or in a subdirectory.
# Marker: loader.py lives at the project root.
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'symphony.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import discover_newref_macro_names
THRESHOLD = 8
if '--threshold' in sys.argv:
    _ti = sys.argv.index('--threshold')
    if _ti + 1 < len(sys.argv):
        THRESHOLD = int(sys.argv[_ti + 1])
SPANS_ONLY = '--spans-only' in sys.argv

all_re = sorted(glob.glob(os.path.join(_ROOT, '*.re')))

def find_spans(filepath):
    with open(filepath, encoding='utf-8', errors='replace') as f:
        raw_lines = f.readlines()
    spans = []
    in_block = False
    block_start = None
    for lineno, raw in enumerate(raw_lines, 1):
        line = raw.rstrip('\n')
        stripped_line = line.lstrip()
        if not in_block and stripped_line.startswith('/') and not stripped_line.startswith('//'):
            continue
        in_str = False
        i = 0
        while i < len(line):
            ch = line[i]
            if in_str:
                if ch == '"': in_str = False
                i += 1
            elif in_block:
                if (ch == '/' and i + 1 < len(line) and line[i+1] == '/'
                        and (i == 0 or line[i-1] != '/')
                        and (i + 2 >= len(line) or line[i+2] != '/')):
                    in_block = False
                    spans.append((block_start, lineno, lineno - block_start + 1))
                    block_start = None
                    break
                else:
                    i += 1
            else:
                if ch == '"':
                    in_str = True; i += 1
                elif (ch == '/' and i + 1 < len(line) and line[i+1] == '/'
                      and (i == 0 or line[i-1] != '/')
                      and (i + 2 >= len(line) or line[i+2] != '/')):
                    in_block = True
                    block_start = lineno
                    i += 2
                else:
                    i += 1
    if in_block:
        spans.append((block_start, None, len(raw_lines) - block_start + 1))
    return spans

print(f"Scanning {len(all_re)} .re files (threshold={THRESHOLD} lines) via real _strip_comments char-scan logic...\n")
total = 0
for fp in all_re:
    for start, end, n in find_spans(fp):
        if n >= THRESHOLD or end is None:
            total += 1
            end_str = str(end) if end is not None else "EOF (NEVER CLOSED)"
            print(f"  {os.path.basename(fp):20s} lines {start}-{end_str}  ({n} lines)")
print(f"\nTotal suspicious spans: {total}")

if SPANS_ONLY:
    sys.exit(0)

# True language primitives: a small, stable set that isn't NEWREF-defined in
# macros.re (ITO/NEW/etc. are foundational keywords, not user-extensible
# macros, so hardcoding these specifically is fine -- they don't drift the
# way a list of *macro* names does).
_PRIMITIVE_CMDS = {
    'ITO', 'NEW', 'NEWSET', 'SET', 'LINK', 'SETREF', 'BLOCK', 'NOLINK',
    'NORESTORE', 'ALLOC_RAW', 'READ_BODY', 'WRITE_OUT',
}

_REAL_CMDS = _PRIMITIVE_CMDS | discover_newref_macro_names()

def classify(filepath, start, end):
    with open(filepath, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    real_end = end if end is not None else len(lines)
    hits = []
    for ln in range(start, real_end):  # 0-indexed slice covers start+1..end-1 (1-indexed)
        if ln >= len(lines):
            break
        text = lines[ln].strip()
        if not text:
            continue
        first = text.split()[0].upper() if text.split() else ''
        if first in _REAL_CMDS:
            hits.append((ln + 1, text[:70]))
    return hits

print(f"Classifying suspicious spans (threshold={THRESHOLD})...\n")
for fp in all_re:
    for start, end, n in find_spans(fp):
        if n < THRESHOLD and end is not None:
            continue
        hits = classify(fp, start, end)
        if hits:
            end_str = str(end) if end is not None else "EOF"
            print(f"=== {os.path.basename(fp)} {start}-{end_str} ({n} lines) — "
                  f"{len(hits)} real-command line(s) swallowed: LIKELY BUG ===")
            for ln, text in hits[:5]:
                print(f"     {ln}: {text}")
            if len(hits) > 5:
                print(f"     ... and {len(hits)-5} more")
