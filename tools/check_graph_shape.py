#!/usr/bin/env python3
"""check_graph_shape.py -- structural snapshot of the built graph.

Why this exists: the obvious way to compare two builds is to list every
NAMED lux and its operands. That is what was being done, and it is blind
in exactly the place it matters. Dissolving the CHAIN blocks made most
body luces anonymous; a break inside a run of anonymous luces then showed
up as "0 lost, 0 new, 0 changed" while the comment stripper was in fact
completely dead. Names are not the graph.

So this walks instead of listing. From every named lux it follows the flow
forward, step by step, and records the sequence of opcodes it passes
through -- naming a lux or not naming it changes nothing about that
sequence, but breaking a link changes it immediately. Anonymous stretches
are covered because they are walked through, not looked up.

Addresses are never compared: they shift on every edit for reasons that
mean nothing. Opcodes are compared by name, and a step into a named lux
records that name, so a rerouted jump is visible as a changed sequence
rather than a changed number.

    python3 check_graph_shape.py save  out.json
    python3 check_graph_shape.py diff  before.json after.json
"""
import sys, os, io, json
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

WALK_DEPTH = 24
_TERMINAL = frozenset({'Jump', 'JumpReg', 'Redi', 'End'})


def _build():
    import loader as L
    err, sys.stderr = sys.stderr, io.StringIO()
    try:
        return L.freeze()
    finally:
        sys.stderr = err


def snapshot() -> dict:
    ldr = _build()
    a = ldr.interp.aether.aether
    S = ldr.symbols
    rev = {}
    for k, v in S.items():
        rev.setdefault(v, k)
    ops = {v: k for k, v in S.items()}

    def opname(addr):
        return rev.get(addr, '?')

    out = {}
    for name, addr in S.items():
        if a[addr] != addr or opname(a[addr + 1]) == '?':
            continue      # data cell, or a lux carrying no real opcode
        seq, pc, seen = [], addr, set()
        limit = len(a) - 8
        for _ in range(WALK_DEPTH):
            if not pc or pc in seen or pc > limit:
                break
            seen.add(pc)
            step = opname(a[pc + 1])
            if step == '?':
                # Not an instruction. Data cells sit next to code, and a
                # walk that keeps stepping +7 into them just reports the
                # memory layout -- which shifts on every edit and means
                # nothing. Flow stops where instructions stop.
                break
            here = rev.get(pc)
            # A named stop is worth recording by name -- that is how a
            # rerouted jump becomes visible rather than merely "some Move".
            seq.append(step if here is None or pc == addr else f"{here}:{step}")
            if step.split(':')[-1] in _TERMINAL:
                # Jump/Redi/JumpReg/End compute their own successor and never
                # read the next slot. Walking past one steps into whatever
                # happens to sit after it in memory, which reports the layout
                # rather than the flow -- and layout shifts on every edit.
                break
            nxt = a[pc + 5]
            pc = pc + 7 if nxt == 0 else nxt
        out[name] = seq
    return out


def diff(before: dict, after: dict) -> int:
    lost = sorted(set(before) - set(after))
    new = sorted(set(after) - set(before))
    changed = sorted(k for k in set(before) & set(after) if before[k] != after[k])
    print(f"lost {len(lost)}  new {len(new)}  reshaped {len(changed)}")
    for k in changed[:40]:
        print(f"  {k}\n    before {before[k]}\n    after  {after[k]}")
    if len(changed) > 40:
        print(f"  ... {len(changed) - 40} more")
    for k in lost[:20]:
        print(f"  LOST {k}")
    return 1 if (lost or changed) else 0


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'save':
        snap = snapshot()
        json.dump(snap, open(sys.argv[2], 'w'), indent=0, sort_keys=True)
        print(f"walked {len(snap)} named entry points, depth {WALK_DEPTH}")
    elif len(sys.argv) >= 4 and sys.argv[1] == 'diff':
        sys.exit(diff(json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))))
    else:
        print(__doc__)
        sys.exit(2)
