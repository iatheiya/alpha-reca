#!/usr/bin/env python3
"""check_htable_reentrancy.py -- precise, call-depth-aware htable tracer.

WHY THIS EXISTS:
Ad-hoc PC-based "am I inside HT_LOOKUP" tracing gives false positives,
because HT_LOOKUP is called constantly, for many unrelated names, all
over the self-hosted run. A PC-only bracket ("somewhere between one
entry into HT_LOOKUP and the next return") catches *any* call, not the
one specific call instance you care about.

This tool instead uses RA_SP (the real call-stack depth register, see
callstack.re) to bracket the *exact* call frame: it records RA_SP the
moment execution jumps into HT_LOOKUP or HT_INSERT right after the
target name was read, and only stops watching once RA_SP has climbed
back above that value (i.e. that specific call, and everything it
called, has genuinely returned) -- not just "some HT_LOOKUP call
somewhere finished".

During that precise window, it watches every write to the registers a
name comparison depends on (BS_TOKBUF_BASE's content, RA_HT_NAME,
RA_HT_NLEN, RA_LOAD_HASH) and reports any that don't match what they
were expected to hold. A genuine mid-call corruption (e.g. from
LOAD_MAIN's interleaved file scheduling interrupting what looks like an
atomic operation) shows up as a real change *inside* this precise
window; a change that only happens *after* the window closes is
expected (the caller moves on to read the next token) and is not
reported.

USAGE:
    python3 check_htable_reentrancy.py SYNTH_WISR_START
    python3 check_htable_reentrancy.py SYNTH_WISR_START --fi 26 --max-hits 5

Exits 0 with "no corruption observed" or prints each corruption event
found (declare_only phase, changed register, before/after values).
"""
import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

import interpreter as _imod
from loader import load_symbols, freeze


def trace_reentrancy(target_name: str, only_fi: int | None, max_hits: int):
    syms = load_symbols()
    sk_fidx = syms.get('SK_FIDX')
    sk_declare_only = syms.get('SK_DECLARE_ONLY')
    tlen_sym = syms.get('RA_LOAD_TLEN')
    tokbuf = syms.get('BS_TOKBUF_BASE')
    ra_sp = syms.get('RA_SP')
    ht_lookup = syms.get('HT_LOOKUP')
    ht_insert = syms.get('HT_INSERT')
    ra_ht_name = syms.get('RA_HT_NAME')
    ra_ht_nlen = syms.get('RA_HT_NLEN')
    ra_ht_hash = syms.get('RA_HT_HASH')

    missing = [n for n, v in [('SK_FIDX', sk_fidx), ('SK_DECLARE_ONLY', sk_declare_only),
                               ('RA_LOAD_TLEN', tlen_sym), ('BS_TOKBUF_BASE', tokbuf),
                               ('RA_SP', ra_sp), ('HT_LOOKUP', ht_lookup),
                               ('HT_INSERT', ht_insert), ('RA_HT_NAME', ra_ht_name),
                               ('RA_HT_NLEN', ra_ht_nlen), ('RA_HT_HASH', ra_ht_hash)]
               if v is None]
    if missing:
        print(f'ERROR: missing expected symbols: {missing}')
        print('(the register/function names this tool depends on may have '
              'been renamed -- update the symbol list above)')
        return 1

    target_bytes = target_name.encode('ascii')
    target_len = len(target_bytes)

    events = []  # (declare_only, register_name, before, after)

    orig_run = _imod.Interpreter._run

    def patched_run(self, start, progress_every=0, sym_arg=None, use_cache=True, max_steps=0):
        watch = {'active': False, 'sp_entry': None, 'entry_snapshot': None, 'hits': 0}
        orig_dispatch = dict(self._dispatch)

        def snapshot(ae):
            tb = int(ae[tokbuf])
            tl = int(ae[tlen_sym])
            name_bytes = bytes(int(ae[tb + i]) & 0xFF for i in range(min(max(tl, 0), 64)))
            return {
                'tokbuf': name_bytes,
                'RA_HT_NAME': int(ae[ra_ht_name]),
                'RA_HT_NLEN': int(ae[ra_ht_nlen]),
                'RA_HT_HASH': int(ae[ra_ht_hash]),
            }

        def make_wrap(fn):
            def w(a1, a2, ex, nxt, ae):
                r = fn(a1, a2, ex, nxt, ae)

                fi = int(ae[sk_fidx])
                if only_fi is not None and fi != only_fi:
                    return r

                # Did we just finish reading the target token? Arm the watch
                # so the *next* HT_LOOKUP/HT_INSERT jump is the one we track.
                if ex == tlen_sym and watch['hits'] < max_hits:
                    tb = int(ae[tokbuf])
                    tl = int(ae[tlen_sym])
                    if tl == target_len:
                        txt = bytes(int(ae[tb + i]) & 0xFF for i in range(target_len))
                        if txt == target_bytes:
                            watch['armed'] = True

                # Entry into the specific call instance we're tracking.
                if watch.get('armed') and r in (ht_lookup, ht_insert) and not watch['active']:
                    watch['active'] = True
                    watch['armed'] = False
                    watch['sp_entry'] = int(ae[ra_sp])
                    watch['entry_snapshot'] = snapshot(ae)
                    watch['declare_only'] = int(ae[sk_declare_only])

                elif watch['active']:
                    cur_sp = int(ae[ra_sp])
                    if cur_sp > watch['sp_entry']:
                        # This exact call frame has returned -- stop watching.
                        watch['active'] = False
                    else:
                        cur = snapshot(ae)
                        for key, before in watch['entry_snapshot'].items():
                            after = cur[key]
                            if after != before:
                                watch['hits'] += 1
                                events.append((watch['declare_only'], key, before, after))
                                watch['entry_snapshot'][key] = after  # avoid re-reporting same delta
                return r
            return w

        for op_id, fn in orig_dispatch.items():
            self._dispatch[op_id] = make_wrap(fn)
        return orig_run(self, start, progress_every, sym_arg, use_cache, max_steps)

    _imod.Interpreter._run = patched_run
    try:
        freeze()
    finally:
        _imod.Interpreter._run = orig_run

    if not events:
        print(f'No corruption observed for "{target_name}" -- every HT_LOOKUP/HT_INSERT '
              f'call instance completed with its inputs unchanged.')
        return 0

    print(f'Found {len(events)} in-flight change(s) for "{target_name}":')
    for declare_only, key, before, after in events:
        print(f'  declare_only={declare_only}  {key}: {before!r} -> {after!r}')
    return 1


def trace_all_attempts(target_name: str, only_fi: int | None, max_hits: int):
    """Log every HT_LOOKUP/HT_INSERT call instance for target_name, in order,
    with its phase (declare_only), file index, and resulting address -- to see
    the full sequence without guessing which phase creates which duplicate."""
    syms = load_symbols()
    sk_fidx = syms.get('SK_FIDX')
    sk_declare_only = syms.get('SK_DECLARE_ONLY')
    tlen_sym = syms.get('RA_LOAD_TLEN')
    tokbuf = syms.get('BS_TOKBUF_BASE')
    ra_sp = syms.get('RA_SP')
    ht_lookup = syms.get('HT_LOOKUP')
    ht_insert = syms.get('HT_INSERT')
    ra_ht_result = syms.get('RA_HT_RESULT')

    target_bytes = target_name.encode('ascii')
    target_len = len(target_bytes)
    events = []

    orig_run = _imod.Interpreter._run

    def patched_run(self, start, progress_every=0, sym_arg=None, use_cache=True, max_steps=0):
        watch = {'armed': False, 'active': False, 'sp_entry': None, 'kind': None}
        orig_dispatch = dict(self._dispatch)

        def make_wrap(fn):
            def w(a1, a2, ex, nxt, ae):
                r = fn(a1, a2, ex, nxt, ae)
                fi = int(ae[sk_fidx])
                if only_fi is not None and fi != only_fi:
                    return r

                if ex == tlen_sym and len(events) < max_hits * 3:
                    tb = int(ae[tokbuf]); tl = int(ae[tlen_sym])
                    if tl == target_len:
                        txt = bytes(int(ae[tb + i]) & 0xFF for i in range(target_len))
                        if txt == target_bytes:
                            watch['armed'] = True

                if watch['armed'] and r in (ht_lookup, ht_insert) and not watch['active']:
                    watch['active'] = True
                    watch['armed'] = False
                    watch['sp_entry'] = int(ae[ra_sp])
                    watch['kind'] = 'HT_LOOKUP' if r == ht_lookup else 'HT_INSERT'
                    watch['declare_only'] = int(ae[sk_declare_only])
                    watch['fi'] = fi
                elif watch['active']:
                    cur_sp = int(ae[ra_sp])
                    if cur_sp > watch['sp_entry']:
                        watch['active'] = False
                        result = int(ae[ra_ht_result]) if ra_ht_result is not None else None
                        events.append((watch['kind'], watch['declare_only'], watch['fi'], result))
                return r
            return w

        for op_id, fn in orig_dispatch.items():
            self._dispatch[op_id] = make_wrap(fn)
        return orig_run(self, start, progress_every, sym_arg, use_cache, max_steps)

    _imod.Interpreter._run = patched_run
    try:
        freeze()
    finally:
        _imod.Interpreter._run = orig_run

    print(f'{len(events)} call instance(s) for "{target_name}":')
    for kind, declare_only, fi, result in events:
        print(f'  {kind}  declare_only={declare_only}  fi={fi}  RA_HT_RESULT={result}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name', help='the interned name to track (e.g. SYNTH_WISR_START)')
    ap.add_argument('--fi', type=int, default=None, help='restrict to this SK_FIDX only')
    ap.add_argument('--max-hits', type=int, default=5,
                     help='stop arming new watches after this many token-matches (default 5)')
    ap.add_argument('--mode', choices=['reentrancy', 'sequence'], default='reentrancy',
                     help='reentrancy: check for mid-call corruption; '
                          'sequence: log every call instance in order (default: reentrancy)')
    args = ap.parse_args()
    if args.mode == 'sequence':
        return trace_all_attempts(args.name, args.fi, args.max_hits)
    return trace_reentrancy(args.name, args.fi, args.max_hits)


# ── iris hub contract ─────────────────────────────────────────────────────
# Stays a standalone file: a focused tracing tool with its own multi-option
# CLI (name, --fi, --max-hits, --mode) that needs a target name to do
# anything. NEEDS="trace" and is_check=False -- it answers "what happened to
# THIS name", which only a human investigating a specific symbol can ask.
from iris import Command


def _cmd_htable_reentrancy(ctx, args) -> int:
    """Trace interning of one name through the self-hosted htable."""
    positional = [x for x in args if not x.startswith('-')]
    if not positional:
        print("Usage: python3 iris.py --htable-reentrancy NAME "
              "[--fi N] [--max-hits N] [--mode reentrancy|sequence]")
        return 0
    name = positional[0]
    fi = None
    max_hits = 5
    mode = 'reentrancy'
    for i, tok in enumerate(args):
        if tok == '--fi' and i + 1 < len(args):
            fi = int(args[i+1])
        elif tok == '--max-hits' and i + 1 < len(args):
            max_hits = int(args[i+1])
        elif tok == '--mode' and i + 1 < len(args):
            mode = args[i+1]
    if mode == 'sequence':
        return trace_all_attempts(name, fi, max_hits)
    return trace_reentrancy(name, fi, max_hits)


COMMANDS = [
    Command("htable-reentrancy", "trace", _cmd_htable_reentrancy,
            is_check=False, takes_args=True,
            help="trace one name's interning for mid-call corruption"),
]


if __name__ == '__main__':
    sys.exit(main())
