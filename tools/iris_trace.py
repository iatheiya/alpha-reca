#!/usr/bin/env python3
"""iris_trace.py — traced-run diagnostics: everything that has to actually
execute the self-hosted LOAD_MAIN and watch it happen.

Commands here declare NEEDS="trace": each drives its own patched/instrumented
run and cannot share a build with anything else, so the hub runs them one at
a time after the shared-build group.

Each command's logic is the original check_*.py / diag mode algorithm, moved
verbatim; only its harness is replaced by the iris Command contract.
"""
import time

from aether import (SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT,
                      NOVA_SIZE)

from iris import Command


# ── LOAD_MAIN run (was diag.py --load-main) ───────────────────────────────
def _load_main(ctx, args) -> int:
    """Run the self-hosted LOAD_MAIN and report stats: steps, files swept,
    command counts, allocation cursor, htable fill, last instructions.

    Optional args: --max-steps N (abort after N steps), --watch LUX
    (print every Write whose destination dereferences to LUX), --watch-exit
    LUX (print every OTHER op whose own Exit slot writes LUX directly --
    Write goes through a pointer and --watch alone catches it, but any op
    (Add/Move/...) can also write a register directly via its own Exit
    field, which --watch never sees).

    Uses loader._freeze_prepare() directly, NOT load_or_freeze(). The
    latter calls freeze(), which already runs LOAD_MAIN once internally
    as part of building reca.bin -- this function's own loop below would
    then be a SECOND, separate execution stacked on top of that already-
    mutated state (hashtable entries already inserted, K_CURSOR already
    advanced), not a repeat of the same computation. _freeze_prepare()
    builds everything up to but not including that one real run, so this
    function's own loop below IS the one real run, matching what freeze()
    itself would produce (confirmed: same step count, same halting point).
    """
    import loader as _loader_mod

    max_steps = 0
    watch_lux = 0
    watch_exit_lux = 0
    for i, tok in enumerate(args):
        if tok == '--max-steps' and i + 1 < len(args):
            max_steps = int(args[i+1])
        elif tok == '--watch' and i + 1 < len(args):
            watch_lux = int(args[i+1])
        elif tok == '--watch-exit' and i + 1 < len(args):
            watch_exit_lux = int(args[i+1])

    ldr, _all_files, _file_list_base = _loader_mod._freeze_prepare()
    interp_obj = ldr.interp
    symbols = ldr.symbols

    a = interp_obj.aether.aether
    R = symbols
    sym = {v: k for k, v in R.items()}

    # _setup_load_main's own job (flag/buffer self-refs, K_CURSOR/K_WATERMARK
    # double-deref) is already done by _freeze_prepare() itself -- it is the
    # exact same setup freeze() performs, not a separate reimplementation.

    # __LT_ALLOC_NOVA as a native op (allocates a NOVA at K_CURSOR).
    lt_addr = R.get("__LT_ALLOC_NOVA", 0)
    ra_ma_ret = R.get("RA_MA_RET", 0)
    K_C_ref = R["K_CURSOR"]
    if lt_addr:
        def _lt_alloc(a1, a2, ex, nxt, aether):
            from loader import _NOVA_SIZE, _WORD
            ptr = aether[K_C_ref + 1]
            aether[K_C_ref + 1] = ptr + _NOVA_SIZE
            aether[ptr + _WORD] = ptr  # self-ref
            if ra_ma_ret:
                aether[ra_ma_ret] = ptr
            return nxt
        interp_obj._dispatch[lt_addr] = _lt_alloc

    track = {
        'LOAD_MAIN_INC': 'files', 'LOAD_MAIN_BF2': 'done',
        'EXP_CMD_NOVA': 'nova', 'PS_CMD_LINK': 'link',
        'LOAD_CU_BME1S': 'builder', 'LOAD_CU_SKPL': 'skip',
    }
    counts = {v: 0 for v in track.values()}
    tracked = {R.get(k, 0): v for k, v in track.items()}
    tracked.pop(0, None)
    Write_op = R.get('Write', 0)
    K_C = R['K_CURSOR']
    dispatch = interp_obj._dispatch
    flux_bottom = interp_obj.aether.flux_bottom
    pc = R['LOAD_MAIN']
    history = []
    crash = None
    t0 = time.time()
    steps = 0
    while pc:
        if pc in tracked:
            counts[tracked[pc]] += 1
        steps += 1
        history.append(pc)
        history = history[-8:]
        if max_steps and steps > max_steps:
            crash = f'step limit {max_steps:,}'
            break
        if watch_lux:
            op = a[pc + SLOT_OP]
            e1 = a[pc + SLOT_E1]
            e2 = a[pc + SLOT_E2]
            if op == Write_op and e1 and a[e1] == watch_lux:
                val = a[e2] if e2 else 0
                print(f'  [WATCH] {sym.get(pc, pc)} → lux {watch_lux} = {val} ({sym.get(val, val)})')
        op = a[pc+SLOT_OP]
        e1 = a[pc+SLOT_E1]
        e2 = a[pc+SLOT_E2]
        ex = a[pc+SLOT_EXIT]
        raw_nx = a[pc+SLOT_NEXT]
        # Mirror interpreter._run's own "next" fallback exactly: an unset
        # (0) next is not "stop", _run auto-advances to pc+NOVA_SIZE.
        if raw_nx == 0:
            nx = pc + NOVA_SIZE
        elif raw_nx >= flux_bottom:
            nx = interp_obj._exec_flux(raw_nx, a)
        else:
            nx = raw_nx
        h = dispatch.get(op)
        if not h:
            crash = (f'unknown op={op} at {sym.get(pc, pc)} '
                     f'(landed here via next=pc+NOVA_SIZE fallback: {raw_nx == 0})')
            break
        if watch_exit_lux and ex == watch_exit_lux and op != Write_op:
            orig_pc = pc
            before = a[watch_exit_lux]
            pc = h(e1, e2, ex, nx, a)
            after = a[watch_exit_lux]
            if after != before:
                print(f'  [WATCH-EXIT] {sym.get(orig_pc, orig_pc)}: '
                      f'{sym.get(op, op)} wrote lux {watch_exit_lux} '
                      f'{before} -> {after} ({sym.get(after, after)})')
        else:
            pc = h(e1, e2, ex, nx, a)

    alloc = a[K_C + 1]
    ht_base = a[R.get('BS_HT_BASE', 0)] if R.get('BS_HT_BASE') else 0
    ht_sz = int(a[R.get('BS_HT_SIZE', 0)]) if R.get('BS_HT_SIZE') else 0
    ht_fill = sum(1 for i in range(ht_sz) if ht_base and a[ht_base + i] != 0)
    print(f"LOAD_MAIN  {'✗ ' + crash if crash else '✓ DONE'}")
    print(f"  Steps  : {steps:,}  ({time.time()-t0:.1f}s)")
    total = int(a[R.get('BS_FILE_COUNT', 0)]) if R.get('BS_FILE_COUNT') else '?'
    print(f"  Files  : {counts['files']} / {total}  (sweep-done={counts['done']})")
    print(f"  Cmds   : NOVA={counts['nova']}  LINK={counts['link']}  "
          f"builder={counts['builder']}  skip={counts['skip']}")
    print(f"  Alloc  : K_CURSOR={int(alloc)}  |  HT: {ht_fill} entries")

    # Name pool / backfill usage. These are fixed-size buffers with no
    # overflow check, and every deferred reference now copies its name into
    # the pool, so the headroom needs to be a measured number. It can only
    # be measured here: on the frozen state the backfill path never runs,
    # leaving the pool untouched and the figure meaningless.
    pool = R.get('SK_SEAL_NAMEPOOL', 0)
    pool_ptr = R.get('SK_SEAL_NAMEPOOL_PTR', 0)
    bf_base = R.get('SK_BACKFILL_BASE', 0)
    bf_ptr = R.get('SK_BACKFILL_PTR', 0)
    if pool and pool_ptr and bf_base and bf_ptr:
        pool_used = int(a[pool_ptr]) - int(a[pool])
        bf_used = int(a[bf_ptr]) - int(a[bf_base])
        pool_cap, bf_cap = 16384, 32768
        pool_pct = pool_used * 100 // pool_cap if pool_cap else 0
        bf_pct = bf_used * 100 // bf_cap if bf_cap else 0
        flag = '' if (0 <= pool_used <= pool_cap and 0 <= bf_used <= bf_cap) \
               else '  ✗ OUT OF BOUNDS'
        print(f"  Buffers: name-pool {pool_used}/{pool_cap} ({pool_pct}%)  |  "
              f"backfill {bf_used}/{bf_cap} ({bf_pct}%){flag}")

    print(f"  Last   : {' → '.join(sym.get(p, str(p)) for p in history[-5:])}")
    return 1 if crash else 0


# ── autolink invariants (was check_autolink_invariants.py) ────────────────
_AUTOLINK_MAX_GAP = 10  # generous: the real gap is 1-3 interpreter steps


def _autolink(ctx, args) -> int:
    """Verify two load-time invariants over a fully traced LOAD_MAIN run:
    AutoNext adjacency (a chain tail's SLOT_NEXT is 0 exactly when the next
    NOVA is physically adjacent) and BS_LAST_NOVA's capture-on-NOLINK
    signature (every capture is immediately followed by an RA_MC_PREV clear).
    """
    from trace import make_patched_run

    verbose = '--verbose' in args
    mc_prev_events = []
    bs_last_events = []

    result = make_patched_run(
        on_write_dest={'RA_MC_PREV': mc_prev_events.append,
                       'BS_LAST_NOVA': bs_last_events.append},
        suppress_stderr=not verbose,
    )
    loader_obj = result['loader']
    a = loader_obj.interp.aether.aether
    a_len = len(a)

    # Check 1: AutoNext adjacency, replayed against the final aether state.
    # Valid because SLOT_NEXT is never touched again after load time, so
    # checking the final content for each observed (prev, new) pair is
    # equivalent to checking it at the moment of the transition.
    autonext_violations = []
    prev_val = 0
    for ev in mc_prev_events:
        new_val = ev['value']
        if prev_val and 0 <= prev_val + SLOT_NEXT < a_len:
            slot_next_val = int(a[prev_val + SLOT_NEXT])
            adjacent = (new_val == prev_val + NOVA_SIZE)
            expected = 0 if adjacent else new_val
            if slot_next_val != expected:
                autonext_violations.append(dict(
                    step=ev['step'], pc_name=ev['pc_name'],
                    prev=prev_val, new=new_val, adjacent=adjacent,
                    slot_next=slot_next_val, expected=expected,
                ))
        prev_val = new_val

    # Check 2: BS_LAST_NOVA's narrow capture-on-NOLINK signature. trace.py's
    # pc_name names the instruction a step transfers TO, not the one
    # executing -- unreliable for "which NOVA wrote this" by label alone.
    # Temporal correlation is the robust signature instead: NOLINK's body is
    # "Move RA_MC_PREV->BS_LAST_NOVA" immediately followed by the clear
    # "Move C_0->RA_MC_PREV". Any capture without that following clear is
    # not part of the designed pattern.
    bslast_violations = []
    mc_by_step = [(e['step'], e['value']) for e in mc_prev_events]
    for bev in bs_last_events:
        s = bev['step']
        nxt = next(((st, v) for st, v in mc_by_step if st > s), None)
        if nxt is None or nxt[0] - s > _AUTOLINK_MAX_GAP or nxt[1] != 0:
            bslast_violations.append(dict(
                step=s, value=bev['value'],
                next_mc_prev_step=nxt[0] if nxt else None,
                next_mc_prev_value=nxt[1] if nxt else None,
            ))

    print(f"RA_MC_PREV write events observed:  {len(mc_prev_events)}")
    print(f"BS_LAST_NOVA write events observed: {len(bs_last_events)}")
    print()

    print(f"[1] AutoNext adjacency check: {len(autonext_violations)} violation(s)")
    for v in autonext_violations[:20]:
        print(f"    step={v['step']:>8} at {v['pc_name']:24s} "
              f"prev={v['prev']} new={v['new']} adjacent={v['adjacent']} "
              f"SLOT_NEXT={v['slot_next']} (expected {v['expected']})")
    print()

    print(f"[2] BS_LAST_NOVA capture-on-NOLINK check: {len(bslast_violations)} violation(s)")
    for v in bslast_violations[:20]:
        print(f"    step={v['step']:>8} value={v['value']} -- next RA_MC_PREV "
              f"write is at step={v['next_mc_prev_step']} value={v['next_mc_prev_value']} "
              f"(expected: within {_AUTOLINK_MAX_GAP} steps, value=0)")
    print()

    if autonext_violations or bslast_violations:
        print("✗ autolink: FAILED — see violations above.")
        return 1
    print("✓ autolink: clean — AutoNext adjacency and BS_LAST_NOVA's narrow "
          "capture-on-NOLINK invariant both hold over the full traced "
          "self-hosted LOAD_MAIN run.")
    return 0


COMMANDS = [
    Command("autolink", "trace", _autolink, is_check=True, takes_args=True,
            help="AutoNext adjacency + BS_LAST_NOVA capture invariants"),
    Command("load-main", "trace", _load_main, is_check=False, takes_args=True,
            help="run the self-hosted LOAD_MAIN and report stats"),
]
