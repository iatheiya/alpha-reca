"""Unit tests for loader.IndentStack -- run before any integration into
loader.py. All cases here were explicitly confirmed with the project
author before being locked in."""
import sys
import os
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)
from loader import IndentStack


def check(name, opener_indent, lines, expected):
    """lines: [(text, indent)], expected: [(action, level)] -- must match
    classify() output exactly, in order."""
    stk = IndentStack(opener_indent)
    got = [stk.classify(indent) for _text, indent in lines]
    ok = got == expected
    status = 'PASS' if ok else 'FAIL'
    print(f'[{status}] {name}')
    if not ok:
        for (text, indent), g, e in zip(lines, got, expected):
            marker = '  ' if g == e else '  <-- MISMATCH'
            print(f'    {text!r:10} indent={indent:3} got={g!r:20} expected={e!r:20}{marker}')
    return ok


def main():
    results = []

    # Confirmed with the project author directly:
    results.append(check(
        "user's full worked example",
        0,
        [('TEXT1', 4), ('TEXT2', 8), ('TEXT3', 12), ('TEXT4', 8),
         ('TEXT5', 12), ('TEXT6', 4), ('TEXT7', 8)],
        [('deeper', 1), ('deeper', 2), ('deeper', 3), ('return', 2),
         ('deeper', 3), ('return', 1), ('deeper', 2)],
    ))

    results.append(check(
        "flat body: every item at the same single level",
        0,
        [('A', 4), ('B', 4), ('C', 4)],
        [('deeper', 1), ('same', 1), ('same', 1)],
    ))

    results.append(check(
        "stray/garbage indent (not a multiple of STEP from any known level) is skipped, doesn't disturb the stack",
        0,
        [('A', 4), ('B', 8), ('STRAY', 6), ('C', 8), ('D', 4)],
        [('deeper', 1), ('deeper', 2), ('skip', None), ('same', 2), ('return', 1)],
    ))

    results.append(check(
        "stray indent shallower than opener_indent is skipped (never matches, never negative-indexes)",
        4,
        [('A', 8), ('STRAY', 2), ('B', 8)],
        [('deeper', 1), ('skip', None), ('same', 1)],
    ))

    results.append(check(
        "immediate jump of +8 from opener (skips a level) is inert, not silently accepted as deeper",
        0,
        [('WEIRD', 8)],
        [('skip', None)],
    ))

    results.append(check(
        "return exactly to opener's own level (indent == opener_indent) is inert -- treated as skip, not a meaningful return",
        0,
        [('A', 4), ('B', 8), ('BACK_TO_OPENER', 0)],
        [('deeper', 1), ('deeper', 2), ('skip', None)],
    ))

    results.append(check(
        "nested opener (context itself starts indented, e.g. hypothetical future macro-in-macro)",
        4,
        [('A', 8), ('B', 12), ('C', 8), ('D', 4)],
        # D lands exactly on opener_indent (4) -> normalized to skip,
        # matching the rule that level 0 is always inert regardless of
        # whether opener_indent itself is 0 or nonzero.
        [('deeper', 1), ('deeper', 2), ('return', 1), ('skip', None)],
    ))

    results.append(check(
        "depth() reflects current top after operations",
        0,
        [('A', 4), ('B', 8)],
        [('deeper', 1), ('deeper', 2)],
    ))

    n_pass = sum(results)
    n_total = len(results)
    print(f'\n{n_pass}/{n_total} passed')
    return n_pass == n_total


if __name__ == '__main__':
    ok = main()
    sys.exit(0 if ok else 1)
