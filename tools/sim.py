#!/usr/bin/env python3
"""sim.py — Reca lexer/prepass simulators.

Merges sim_prepass.py and sim_reca_lexer.py into one tool.

Usage:
    python3 sim.py --prepass FILE.re   # simulate LOAD_PREPASS_FILE's byte scanner
    python3 sim.py --lexer FILE.re     # simulate BS_READ_TOKEN's full comment logic
    python3 sim.py --diff FILE.re      # run both and show where they disagree
"""
import sys, os
_HERE = os.path.dirname(os.path.abspath(__file__))
# If this file is in a subdirectory (e.g. tooling/), root is one level up.
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'symphony.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

SLASH = ord('/'); LF = ord('\n'); SP = ord(' '); TAB = ord('\t'); CR = ord('\r')

# ── Prepass simulator (from sim_prepass.py) ──────────────────────────────────
# Faithful simulation of LOAD_PREPASS_FILE's hand-rolled byte scanner (saku.re).

def prepass_scan(data: bytes):
    """Returns list of (line, src_token) for every LINK line detected."""
    i = 0; n = len(data); found_links = []
    while i < n:
        b = data[i]
        if b == SLASH:
            # block comment?
            if i + 1 < n and data[i+1] == SLASH:
                i += 2
                while i < n:
                    if data[i] == SLASH and i+1 < n and data[i+1] == SLASH:
                        i += 2; break
                    i += 1
                continue
            # line comment
            while i < n and data[i] != LF: i += 1
            continue
        if b == LF: i += 1; continue
        if b in (SP, TAB): i += 1; continue
        # read a token
        tok_start = i
        while i < n and data[i] not in (SP, TAB, LF): i += 1
        tok = data[tok_start:i]
        line = data[:tok_start].count(LF) + 1
        if tok == b'LINK':
            found_links.append((line, 'LINK'))
    return found_links


def prepass_main(path):
    data = open(path, 'rb').read()
    links = prepass_scan(data)
    print(f"Prepass scan of {path}: {len(links)} LINK lines")
    for ln, tok in links:
        print(f"  line {ln}: {tok}")


# ── Reca lexer simulator (from sim_reca_lexer.py) ────────────────────────────
# Faithful simulation of BS_READ_TOKEN's comment handling + BS_BLOCK_SKIP.

def reca_tokenize(data: bytes):
    """Returns list of (start_offset, token_bytes) for every real token."""
    i = 0; n = len(data); tokens = []
    while i < n:
        # skip whitespace
        while i < n and data[i] in (SP, TAB, CR, LF): i += 1
        if i >= n: break
        # line comment?
        if data[i] == SLASH:
            if i+1 < n and data[i+1] != SLASH:
                # single /: line comment
                while i < n and data[i] != LF: i += 1
                continue
            if i+1 < n and data[i+1] == SLASH:
                # block comment: skip until //
                i += 2
                while i < n:
                    if data[i] == SLASH and i+1 < n and data[i+1] == SLASH:
                        i += 2; break
                    i += 1
                continue
        tok_start = i
        while i < n and data[i] not in (SP, TAB, CR, LF): i += 1
        tokens.append((tok_start, data[tok_start:i]))
    return tokens


def python_tokenize(data: bytes):
    """Simulate Python _strip_comments line-by-line tokenisation."""
    tokens = []
    for lineno, raw in enumerate(data.split(b'\n'), 1):
        line = raw.rstrip(b'\r')
        # strip inline /
        slash = line.find(b'/')
        if slash >= 0: line = line[:slash]
        for tok in line.split():
            tokens.append((lineno, tok))
    return tokens


def lexer_main(path):
    data = open(path, 'rb').read()
    toks = reca_tokenize(data)
    print(f"Reca lexer sim of {path}: {len(toks)} tokens")
    for off, tok in toks[:40]:
        print(f"  @{off}: {tok!r}")
    if len(toks) > 40:
        print(f"  ... and {len(toks)-40} more")


def diff_main(path):
    data = open(path, 'rb').read()
    reca = [tok for _, tok in reca_tokenize(data)]
    py   = [tok for _, tok in python_tokenize(data)]
    reca_set = set(reca); py_set = set(py)
    only_reca = reca_set - py_set
    only_py   = py_set   - reca_set
    print(f"Diff for {path}:")
    print(f"  Reca tokens: {len(reca)}  Python tokens: {len(py)}")
    if only_reca:
        print(f"  Only in Reca ({len(only_reca)}):", sorted(only_reca)[:20])
    if only_py:
        print(f"  Only in Python ({len(only_py)}):", sorted(only_py)[:20])
    if not only_reca and not only_py:
        print("  ✓ No differences")


if __name__ == '__main__':
    args = sys.argv[1:]
    if '--prepass' in args:
        idx = args.index('--prepass')
        prepass_main(args[idx+1])
    elif '--lexer' in args:
        idx = args.index('--lexer')
        lexer_main(args[idx+1])
    elif '--diff' in args:
        idx = args.index('--diff')
        diff_main(args[idx+1])
    else:
        print(__doc__)
