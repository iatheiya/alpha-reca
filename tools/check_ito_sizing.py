#!/usr/bin/env python3
"""Systematic check: for every name that is the TARGET of an ITO line or any
macro discovered by loader.py's own _discover_ito_naming_commands() (i.e.
needs a full ITO_SIZE=7 lux), verify its final address actually ended up in
loader._ito_addrs_set (allocated via _alloc_ito). Report every name where
this invariant is violated — these are all instances of the same
under-allocation bug class."""
import sys, os, glob, re as relib

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'symphony.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import freeze

l = freeze()
R = l.symbols
ito_set = l._ito_addrs_set
naming_cmds = l._discover_ito_naming_commands()  # same universal discovery
                                                   # loader.py itself uses —
                                                   # no separate hardcoded list

all_re = sorted(glob.glob(os.path.join(_ROOT, '**', '*.re'), recursive=True))

needs_ito_names = set()
for fp in all_re:
    try:
        with open(fp, encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
    except OSError:
        continue
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        toks = stripped.split()
        cmd = toks[0]
        if cmd == 'ITO' and len(toks) >= 2:
            needs_ito_names.add(toks[1])
        elif cmd in naming_cmds and len(toks) >= 2:
            needs_ito_names.add(toks[1])

print(f"Names that appear as ITO/{'/'.join(sorted(naming_cmds))} targets across all files: {len(needs_ito_names)}")

violations = []
for name in sorted(needs_ito_names):
    addr = R.get(name)
    if addr is None:
        continue  # not in final symbol table (template/anon-name artifact etc.)
    if addr not in ito_set:
        violations.append((name, addr))

print(f"\nVIOLATIONS (need full ITO, but address NOT in _ito_addrs_set): {len(violations)}")
for name, addr in violations:
    print(f"  {name:30s} addr={addr}")
