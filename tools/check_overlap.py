#!/usr/bin/env python3
"""check_overlap.py -- do any two allocated bodies sit on top of each other?

Names are unique and addresses are unique; what is not checked anywhere is
whether the CELLS a name occupies stay its own. A lux is not one cell: an
instruction is seven, a region is one plus its contents, and a lux carrying
extra lumina is seven plus two per lumen plus a terminator. Every one of
those widths is computed in more than one place, and the moment two of them
disagree a name's body reaches into its neighbour -- addresses still unique,
symbol table still clean, and the neighbour quietly destroyed.

That is not hypothetical: the prepass loop's own conditional jump had its
first two cells overwritten by the label allocated seven cells after the
comparison, while the jump itself started nine cells after it. Both names
were in the table, neither shared an address, and the loop simply stopped.

    python3 check_overlap.py
"""
import sys, os, io

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)


def widths(ldr):
    """(name, addr, cells) for everything the loader allocated."""
    from loader import _NOVA_SIZE
    out = []
    for name, addr in ldr.symbols.items():
        n_extra = len(ldr._lumen_prepass.get(name, []))
        if name in ldr._nova_names:
            # Extra lumina, when present, start right at the pad slot
            # itself (offset NOVA_SIZE-1) instead of after a separate
            # always-empty slot -- one fewer cell than the old "NOVA_SIZE
            # + 2*n_extra + terminator" scheme, which is exactly what the
            # NOVA_SIZE-1 base plus the +1 terminator cancels out to.
            # n_extra==0 is unaffected (still exactly NOVA_SIZE).
            cells = _NOVA_SIZE + 2 * n_extra
        elif name in getattr(ldr, '_span_elem', {}):
            cells = None          # region: size known only to its own record
        else:
            cells = 1 + 2 * n_extra + 1
        if cells:
            out.append((name, addr, cells))
    return out


def main():
    import loader as L
    err, sys.stderr = sys.stderr, io.StringIO()
    try:
        ldr = L.freeze()
    finally:
        sys.stderr = err

    spans = sorted(widths(ldr), key=lambda t: t[1])
    bad = []
    for (n1, a1, c1), (n2, a2, c2) in zip(spans, spans[1:]):
        if a1 + c1 > a2:
            bad.append((n1, a1, c1, n2, a2, a1 + c1 - a2))

    if not bad:
        print(f"Overlap: clean -- {len(spans)} bodies, none reaching into another.")
        return 0
    print(f"Overlap: {len(bad)} body/bodies reaching into the next one\n")
    for n1, a1, c1, n2, a2, by in bad[:40]:
        print(f"  {n1} @{a1} takes {c1} cells (to {a1 + c1 - 1}), "
              f"but {n2} starts at {a2} -- {by} cell(s) into it")
    if len(bad) > 40:
        print(f"  ... {len(bad) - 40} more")
    return 1


if __name__ == '__main__':
    sys.exit(main())
