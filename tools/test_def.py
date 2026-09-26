#!/usr/bin/env python3
"""test_def.py -- DEF block regression test.

DEF has zero call sites in the corpus, so a normal build exercises none of
its handler: this test is the ONLY thing that guards it. It drives real DEF
source through the loader's own Wave-0 parse and the Wave-2 expansion
(_def_expand_records) and checks the produced records directly -- entry
naming, {RET} resolution (including a nested DEF's own distinct return), the
appended implicit RREDI, and that a self-named entry is left alone.

Run: python test_def.py   (exit 0 = pass, non-zero = fail)
"""
import io
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

import interpreter as interp_mod
from loader import (Loader, _strip_comments, _parse_indent_blocks,
                    _parse_record, _def_expand_records, _known_commands,
                    seed_candidates)


def _known():
    """The real known-commands vocabulary, from the core definitions."""
    scan = Loader(interp_mod.Interpreter())
    old = sys.stderr
    sys.stderr = io.StringIO()
    try:
        # Seed only: command declarations (Cmd=) are discovered by the loader
        # itself from every .re file, so no command file is named here.
        scan.load_file(seed_candidates()[0])
    finally:
        sys.stderr = old
    return _known_commands(scan)


def _parse_block(text, known):
    raw = [ln + '\n' for ln in text.split('\n')]
    items = _parse_indent_blocks(_strip_comments(raw), None, 'test')
    return [(ln, it if isinstance(it, tuple) else _parse_record(it, known))
            for ln, it in items]


def _expand(rec, known):
    _cmd, args, slots = rec
    return [r for _ln, r in _def_expand_records(
        args[0], slots['__body__'], slots['__tag__'], known) if r is not None]


def test_simple(known):
    """Entry takes the block name, {RET} resolves, RREDI is appended, and an
    anonymous body lux stays anonymous."""
    recs = _parse_block(
        "DEF MYSUB\n"
        "    NOVA Op=Add El1=RA_A El2=RA_B Exit=RA_C\n"
        "    JZ TS El1=RA_C Exit={RET}\n"
        "    NOVA Op=Move El1=RA_C Exit=RA_D\n", known)
    assert len(recs) == 1 and recs[0][1][0] == 'DEF', recs
    out = _expand(recs[0][1], known)
    assert out[0] == ('NOVA', ['MYSUB'],
                      {'Op': 'Add', 'El1': 'RA_A', 'El2': 'RA_B', 'Exit': 'RA_C'}), out[0]
    jz = [r for r in out if r[0] == 'JZ'][0]
    assert jz[2].get('Exit') == 'MYSUB_RET', jz
    anon = out[2]
    assert anon[0] == 'NOVA' and anon[1] == [], anon
    assert out[-1] == ('RREDI', ['MYSUB_RET'], {}), out[-1]


def test_nested(known):
    """A nested DEF keeps its own distinct {RET}; the outer's {RET} is the
    outer return."""
    recs = _parse_block(
        "DEF OUTER\n"
        "    NOVA Op=Add El1=X El2=Y Exit=Z\n"
        "    DEF INNER\n"
        "        NOVA Op=Move El1=Z Exit=W\n"
        "        JZ IJ El1=W Exit={RET}\n"
        "    JZ OJ El1=Z Exit={RET}\n", known)
    outer = _expand(recs[0][1], known)
    ojz = [r for r in outer if r[0] == 'JZ'][0]
    assert ojz[2].get('Exit') == 'OUTER_RET', ojz
    inner_rec = [r for r in outer if r[0] == 'DEF'][0]
    assert inner_rec[1][0] == 'INNER', inner_rec
    inner = _expand(inner_rec, known)
    ijz = [r for r in inner if r[0] == 'JZ'][0]
    assert ijz[2].get('Exit') == 'INNER_RET', ijz
    assert inner[0] == ('NOVA', ['INNER'], {'Op': 'Move', 'El1': 'Z', 'Exit': 'W'}), inner[0]
    assert inner[-1] == ('RREDI', ['INNER_RET'], {}), inner[-1]


def test_self_named_entry(known):
    """An entry that already names itself is left alone."""
    recs = _parse_block(
        "DEF THIRD\n"
        "    NOVA EXPLICIT Op=Add El1=A El2=B Exit=C\n"
        "    JZ TJ El1=C Exit={RET}\n", known)
    out = _expand(recs[0][1], known)
    assert out[0] == ('NOVA', ['EXPLICIT'],
                      {'Op': 'Add', 'El1': 'A', 'El2': 'B', 'Exit': 'C'}), out[0]
    assert out[-1] == ('RREDI', ['THIRD_RET'], {}), out[-1]


def main():
    known = _known()
    for fn in (test_simple, test_nested, test_self_named_entry):
        fn(known)
        print(f"  {fn.__name__} ok")
    print("DEF: all tests pass")


if __name__ == '__main__':
    main()
