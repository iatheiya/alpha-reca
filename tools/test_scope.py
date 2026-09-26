#!/usr/bin/env python3
"""test_scope.py -- scoped local variables regression test.

Locality is a compile-time resolution layer: inside a block (a DEF body)
a name binds LOCAL by default; the block's entry name and any Vis=Public name
stay global. The runtime never sees a scope -- only the flat addresses this
produces. This test drives a real DEF with a local name that collides with a
global one through the loader's own declare pass and checks:

  - the local and the global with the SAME spelling get DIFFERENT addresses,
  - the global binding is untouched by the block,
  - the block's entry name is public (in the global table),
  - Vis=Public inside the body escapes back to the global table.

Run: python test_scope.py   (exit 0 = pass, non-zero = fail)
"""
import io
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

import loader as loader_mod
from loader import (Loader, _strip_comments, _parse_indent_blocks,
                    _parse_record, _known_commands, _COMMANDS)


def _build_quiet():
    old = sys.stderr
    sys.stderr = io.StringIO()
    try:
        return loader_mod.freeze()
    finally:
        sys.stderr = old


def _parse_block(text, known):
    raw = [ln + '\n' for ln in text.split('\n')]
    items = _parse_indent_blocks(_strip_comments(raw), None, 'test')
    return [(ln, it if isinstance(it, tuple) else _parse_record(it, known))
            for ln, it in items]


def main():
    L = _build_quiet()
    known = _known_commands(L)

    # A global TMP, declared at top level (no scope open) -> global table.
    L._wave2_build_line(('NEW', ['TMP'], {}), 'test', 1, 0)
    assert 'TMP' in L.symbols, "global TMP not registered"
    g_tmp = L.symbols['TMP']

    # A DEF whose body declares its own TMP (local) and a Vis=Public ESCAPED.
    # First body line is anonymous -> takes the entry name MYSUB.
    src = ("DEF MYSUB\n"
           "    NOVA Op=Add El1=TMP El2=TMP Exit=TMP\n"
           "    NEW TMP\n"
           "    NEW ESCAPED Vis=Public\n")
    recs = _parse_block(src, known)
    defrec = recs[0][1]
    assert defrec[0] == 'DEF', defrec

    _COMMANDS['DEF'].declare(L, defrec, 'test', 10, 0)

    sid = ("scope", "MYSUB")
    # 1. TMP was bound locally in the DEF scope, at a DIFFERENT address than
    #    the global TMP -- same spelling, two distinct luces.
    assert sid in L.scopes and 'TMP' in L.scopes[sid], \
        f"local TMP not in scope: {L.scopes.get(sid)}"
    l_tmp = L.scopes[sid]['TMP']
    assert l_tmp != g_tmp, f"local TMP {l_tmp} == global TMP {g_tmp} (not isolated)"

    # 2. The global TMP binding is untouched.
    assert L.symbols['TMP'] == g_tmp, "global TMP was clobbered by the block"

    # 3. The entry name is public (lives in the global table, not the scope).
    assert 'MYSUB' in L.symbols, "entry MYSUB not public"
    assert 'MYSUB' not in L.scopes.get(sid, {}), "entry MYSUB wrongly localized"

    # 4. Vis=Public inside the body escapes to the global table, not the scope.
    assert 'ESCAPED' in L.symbols, "Vis=Public name did not reach the global table"
    assert 'ESCAPED' not in L.scopes.get(sid, {}), "Vis=Public name wrongly localized"

    print("  local/global isolation ok")
    print("  global untouched ok")
    print("  entry public ok")
    print("  Vis=Public escape ok")
    print("SCOPE: all tests pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
