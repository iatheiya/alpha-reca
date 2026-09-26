#!/usr/bin/env python3
"""fast_profile.py — hotspot profiler for long self-hosted LOAD_MAIN runs.

The tool that actually found the `RA_HT_SLOT`-reset bug (BUGS.md,
forty-fifth round): `trace.py --profile` runs a fully-traced loop (real
per-instruction Python overhead for its generic callback machinery),
which is far too slow to reach deep into a multi-hundred-million-step
run within reasonable wall time. This uses the same fast raw-dispatch
loop as `long_run.py`, adding only a single dict increment per step, to
profile orders of magnitude further within the same wall-clock budget
— at the cost of only tracking PC hit-counts, nothing else (no register
values, no PC history, no conditional watches). Use `trace.py`'s
`make_patched_run` instead for anything needing more than "which PCs
are hot."

USAGE
    # Profile a fresh run for up to N steps (default 200M):
    python3 fast_profile.py --steps 200000000

    # Profile a short burst resumed from an existing long_run.py
    # checkpoint, instead of starting over -- the way this tool is
    # usually actually used: something is suspiciously slow deep into
    # a long_run.py session, profile *from there* instead of replaying
    # the whole prefix under the profiler too:
    python3 fast_profile.py --from-checkpoint --steps 60000000

Prints the top N (--top, default 40) hottest PCs by symbolic name
(resolved via reca.sym) with their hit counts, plus final SK_FIDX/
RA_LOAD_RPOS for context. A single label (or a tight cluster of
labels with near-identical counts, like one macro's whole body) far
ahead of the rest is the signal worth chasing -- see BUGS.md's
forty-fifth round for what that looked like in practice (an entire
per-character comparison loop, 9 labels, all within a few hundred of
each other, at counts ~1000x every other label in the run).
"""
import sys, time, os, argparse
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)
import loader as _ldr
import interpreter as _imod

SYM_PATH = os.path.join(_ROOT, 'reca.sym')
CKPT_PATH = os.path.join(_ROOT, 'long_run_ckpt.bin')
PC_SLOT = 7_999_999


def _load_symbols():
    symbols = {}
    with open(SYM_PATH) as f:
        for line in f:
            addr, name = line.rstrip('\n').split('\t')
            symbols[name] = int(addr)
    return symbols


def profile_fresh(steps: int, top: int):
    counts = {}

    def patched_run(self, start, progress_every=0, sym_arg=None, use_cache=True, max_steps=0):
        a = self.aether.aether
        SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT = 1, 2, 3, 4, 5
        NOVA_SIZE = 7
        dispatch = self._dispatch
        pc = start
        s = 0
        get = counts.get
        while pc and s < steps:
            s += 1
            counts[pc] = get(pc, 0) + 1
            op_id = a[pc + SLOT_OP]
            fn = dispatch.get(op_id)
            if fn is None:
                break
            e1 = a[pc + SLOT_E1]; e2 = a[pc + SLOT_E2]; ex = a[pc + SLOT_EXIT]
            raw = a[pc + SLOT_NEXT]
            nxt = pc + NOVA_SIZE if raw == 0 else raw
            res = fn(e1, e2, ex, nxt, a)
            pc = res if res is not None else nxt
        return pc

    _imod.Interpreter._run = patched_run
    import io as _io
    buf = _io.StringIO(); old_err = sys.stderr; sys.stderr = buf
    t0 = time.time()
    try:
        ldr = _ldr.freeze()
    finally:
        sys.stderr = old_err
    _print_report(counts, time.time() - t0, top)


def profile_from_checkpoint(steps: int, top: int):
    if not os.path.exists(CKPT_PATH):
        print(f"ERROR: {CKPT_PATH} not found -- run long_run.py first to "
              f"create a checkpoint to profile from.", file=sys.stderr)
        sys.exit(1)
    symbols = _load_symbols()
    interp = _imod.Interpreter()
    interp.aether.thaw(CKPT_PATH)
    interp.update_relations(symbols)
    a = interp.aether.aether
    pc = int(a[PC_SLOT])

    counts = {}
    SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT = 1, 2, 3, 4, 5
    NOVA_SIZE = 7
    dispatch = interp._dispatch
    s = 0
    get = counts.get
    t0 = time.time()
    while pc and s < steps:
        s += 1
        counts[pc] = get(pc, 0) + 1
        op_id = a[pc + SLOT_OP]
        fn = dispatch.get(op_id)
        if fn is None:
            break
        e1 = a[pc + SLOT_E1]; e2 = a[pc + SLOT_E2]; ex = a[pc + SLOT_EXIT]
        raw = a[pc + SLOT_NEXT]
        nxt = pc + NOVA_SIZE if raw == 0 else raw
        res = fn(e1, e2, ex, nxt, a)
        pc = res if res is not None else nxt
    print(f"SK_FIDX: {int(a[symbols['SK_FIDX']])}  RA_LOAD_RPOS: {int(a[symbols['RA_LOAD_RPOS']])}")
    _print_report(counts, time.time() - t0, top)


def _print_report(counts, wall, top):
    symbols = _load_symbols()
    rev = {v: k for k, v in symbols.items()}
    print(f"wall: {wall:.1f}s   distinct PCs hit: {len(counts)}   "
          f"total steps: {sum(counts.values())}")
    print()
    ranked = sorted(counts.items(), key=lambda kv: -kv[1])[:top]
    for pc, cnt in ranked:
        print(f"{cnt:>12}  {rev.get(pc, f'#{pc}')}")


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--steps', type=int, default=200_000_000)
    p.add_argument('--top', type=int, default=40)
    p.add_argument('--from-checkpoint', action='store_true',
                    help='profile a burst resumed from long_run_ckpt.bin instead of a fresh run')
    args = p.parse_args()
    if args.from_checkpoint:
        profile_from_checkpoint(args.steps, args.top)
    else:
        profile_fresh(args.steps, args.top)
