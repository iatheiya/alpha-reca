"""
trace.py — Reca execution tracer.

Usage:
    python3 trace.py                          # run P0_NID, show crash/summary
    python3 trace.py --from SYMBOL            # start from named lux
    python3 trace.py --steps N                # cap at N steps (default 10M)
    python3 trace.py --until SYMBOL           # stop when PC reaches symbol
    python3 trace.py --verbose                # print every step
    python3 trace.py --watch SYM [--watch S2] # print when register changes
    python3 trace.py --profile                # hotspot counter (top-N PCs)
    python3 trace.py --fault                  # stop at first unknown-op or fault-vector trip
    python3 trace.py --stack                  # show call stack on Voca/Redi
    python3 trace.py --compile                # run compiler, show EP_BUF state after
    python3 trace.py --loadmain               # trace LOAD_MAIN: show file+line+token for each dispatch
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loader import freeze
from symphony import SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, ITO_SIZE

_HERE = os.path.dirname(os.path.abspath(__file__))


def _parse_args(els):
    cfg = dict(
        start_name=None, max_steps=10_000_000,
        until_name=None, verbose=False,
        watch_names=[], profile=False,
        fault=False, stack=False, compile_mode=False,
        loadmain=False,
    )
    i = 0
    while i < len(els):
        a = els[i]
        if a == '--from' and i + 1 < len(els):
            cfg['start_name'] = els[i + 1]; i += 2
        elif a == '--steps' and i + 1 < len(els):
            cfg['max_steps'] = int(els[i + 1]); i += 2
        elif a == '--until' and i + 1 < len(els):
            cfg['until_name'] = els[i + 1]; i += 2
        elif a == '--watch' and i + 1 < len(els):
            cfg['watch_names'].append(els[i + 1]); i += 2
        elif a == '--verbose':
            cfg['verbose'] = True; i += 1
        elif a == '--profile':
            cfg['profile'] = True; i += 1
        elif a == '--fault':
            cfg['fault'] = True; i += 1
        elif a == '--stack':
            cfg['stack'] = True; i += 1
        elif a == '--compile':
            cfg['compile_mode'] = True; i += 1
        elif a == '--loadmain':
            cfg['loadmain'] = True; i += 1
        else:
            i += 1
    return cfg


def run_loadmain():
    """Trace LOAD_MAIN: show file opens, top-level dispatch tokens,
    MA arg reads, and RA_LOAD_BYTE=LF anomalies (PRELF check).

    This is the dedicated diagnostic for the self-hosting loop:
    'Why does LOAD_MAIN stop early, and at exactly which file+line?'
    """
    import io as _io

    # Boot without executing LOAD_MAIN yet — we'll call freeze() ourselves
    # but intercept at the interpreter level.
    buf = _io.StringIO(); old = sys.stderr; sys.stderr = buf
    l = freeze()
    sys.stderr = old

    a   = l.interp.aether.aether
    R   = l.symbols
    sym = {v: k for k, v in R.items()}

    r_ref = {}
    state = dict(current_file='', line_n=0, open_count=0)

    # ── openat → track current file ──────────────────────────────────────────
    orig_do = l.interp._do_syscall

    def _do(self, nr, a0, a1, a2, ae):
        if nr == getattr(self, '_sys_openat', None):
            pb = bytearray(); i = a1
            try:
                while i < len(ae):
                    w = ae[i]
                    if not w: break
                    for sh in range(0, 64, 8):
                        b = (w >> sh) & 0xFF
                        if b == 0: break
                        pb.append(b)
                    else:
                        i += 1; continue
                    break
                fname = pb.decode('utf-8', 'replace').split('/')[-1]
                state['current_file'] = fname
                state['open_count'] += 1
                state['line_n'] = 0
                print(f"\n[FILE #{state['open_count']}] {fname}")
            except Exception:
                pass
        return orig_do(self, nr, a0, a1, a2, ae)

    l.interp._do_syscall = _do.__get__(l.interp, type(l.interp))

    # ── Redi → track LOAD_DL_TLENCK (top-level dispatch), LOAD_MA_TLENCK, LOAD_CU_MA0CK
    orig_r = l.interp._redi

    def _redi(self, a1, a2, e, n, ae):
        result = orig_r(self, a1, a2, e, n, ae)
        tl    = r_ref.get('tl', 0)
        tb    = r_ref.get('tb', 0)
        tlen  = ae[tl] if tl else 0
        buf   = ae[tb] if tb else 0

        def _tok():
            if buf and tlen > 0:
                return bytes(ae[buf + i] & 0xFF for i in range(min(tlen, 30))).decode('ascii', 'replace')
            return f'(tlen={tlen})'

        if result == r_ref.get('tlenck', 0) and r_ref.get('tlenck'):
            state['line_n'] += 1
            tok = _tok()
            if tlen > 0:
                print(f"  L{state['line_n']:04d}  dispatch  {tok!r}")

        elif result == r_ref.get('ma_tlenck', 0) and r_ref.get('ma_tlenck'):
            tok = _tok()
            if tlen > 0:
                print(f"         MA-tok   {tok!r}")
            else:
                print(f"         MA-tok   (EOL — stops here)")

        elif result == r_ref.get('ma0ck', 0) and r_ref.get('ma0ck'):
            ra_ma0 = r_ref.get('ra_ma0', 0); ra_ma1 = r_ref.get('ra_ma1', 0)
            ma0 = ae[ra_ma0] if ra_ma0 else 0; ma1 = ae[ra_ma1] if ra_ma1 else 0
            ma0n = sym.get(ma0, f'#{ma0}'); ma1n = sym.get(ma1, f'#{ma1}')
            ra_lb = r_ref.get('ra_lb', 0)
            lb = ae[ra_lb] if ra_lb else '?'
            print(f"         builder  MA0={ma0n}  MA1={ma1n}  RA_LOAD_BYTE={lb}")

        return result

    l.interp._redi = _redi.__get__(l.interp, type(l.interp))

    # ── Equal → detect LOAD_MA_PRELF (RA_LOAD_BYTE == LF) ────────────────────
    orig_eq = l.interp._cmpeq

    def _cmpeq(self, a1, a2, exit, nxt, ae):
        ra_lb   = r_ref.get('ra_lb', 0)
        ld_lf   = r_ref.get('ld_lf', 0)
        ld_flag = r_ref.get('ld_flag', 0)
        if a1 == ra_lb and a2 == ld_lf and exit == ld_flag and ra_lb:
            lb = ae[ra_lb]
            tl = r_ref.get('tl', 0); tlen = ae[tl] if tl else 0
            tb = r_ref.get('tb', 0); buf  = ae[tb] if tb else 0
            tok = bytes(ae[buf + i] & 0xFF for i in range(min(tlen, 15))).decode('ascii', 'replace') \
                  if (buf and tlen > 0) else '?'
            if lb == 10:  # LF — MA will return 0, skipping this arg
                print(f"         !! PRELF=LF  byte={lb}  tok={tok!r}  ← MA1 will be SKIPPED")
        return orig_eq(self, a1, a2, exit, nxt, ae)

    l.interp._cmpeq = _cmpeq.__get__(l.interp, type(l.interp))

    # ── update_relations → cache symbol addresses ─────────────────────────────
    orig_ur = l.interp.update_relations

    def _ur(self, symbols):
        result = orig_ur(self, symbols)
        r_ref.clear()
        r_ref.update({v: k for k, v in symbols.items()})
        r_ref['tlenck']    = symbols.get('LOAD_DL_TLENCK', 0)
        r_ref['ma_tlenck'] = symbols.get('LOAD_MA_TLENCK', 0)
        r_ref['ma0ck']     = symbols.get('LOAD_CU_MA0CK', 0)
        r_ref['ra_lb']     = symbols.get('RA_LOAD_BYTE', 0)
        r_ref['ra_ma0']    = symbols.get('RA_MA0', 0)
        r_ref['ra_ma1']    = symbols.get('RA_MA1', 0)
        r_ref['tl']        = symbols.get('RA_LOAD_TLEN', 0)
        r_ref['tb']        = symbols.get('BS_TOKBUF_BASE', 0)
        r_ref['ld_flag']   = symbols.get('LD_FLAG', 0)
        # LD_LF: lux whose word == 10
        for k, v in symbols.items():
            if a[v] == 10 and 'LF' in k:
                r_ref['ld_lf'] = v; break
        return result

    l.interp.update_relations = _ur.__get__(l.interp, type(l.interp))

    print("=== LOAD_MAIN trace ===")
    print("Shows: [FILE] opens │ L#### dispatch TOKEN │ MA-tok args │ !! PRELF=LF anomalies")
    print()

    # Run LOAD_MAIN (it already ran in freeze(); re-freeze to see it live)
    # Actually freeze() already ran above. We need to re-run just LOAD_MAIN.
    # We can do this by directly executing it:
    import io as _io2
    buf2 = _io2.StringIO(); old2 = sys.stderr; sys.stderr = buf2
    try:
        l2 = freeze()
        # The hooks are on l, not l2. But freeze() creates a new loader.
        # Instead: patch the loader module's freeze to add hooks first.
        # Simpler: just call gen_compiler-style directly on fresh loader.
    except Exception:
        pass
    sys.stderr = old2

    # The hooks above are on `l` which already ran. Re-patch and re-execute LOAD_MAIN:
    # We need a fresh freeze with patches. Use the module-level approach:
    import loader as _ldr_mod
    import interpreter as _imod

    r_ref2 = {}
    state2 = dict(current_file='', line_n=0, open_count=0)

    orig_do2  = _imod.Interpreter._do_syscall
    orig_r2   = _imod.Interpreter._redi
    orig_eq2  = _imod.Interpreter._cmpeq
    orig_ur2  = _imod.Interpreter.update_relations

    def _do2(self, nr, a0, a1, a2, ae):
        if nr == getattr(self, '_sys_openat', None):
            pb = bytearray(); i = a1
            try:
                while i < len(ae):
                    w = ae[i]
                    if not w: break
                    for sh in range(0, 64, 8):
                        b = (w >> sh) & 0xFF
                        if b == 0: break
                        pb.append(b)
                    else:
                        i += 1; continue
                    break
                fname = pb.decode('utf-8', 'replace').split('/')[-1]
                state2['current_file'] = fname
                state2['open_count'] += 1
                state2['line_n'] = 0
                print(f"\n[FILE #{state2['open_count']}] {fname}")
            except Exception:
                pass
        return orig_do2(self, nr, a0, a1, a2, ae)

    def _r2(self, a1, a2, e, n, ae):
        result = orig_r2(self, a1, a2, e, n, ae)
        tl   = r_ref2.get('tl', 0); tb = r_ref2.get('tb', 0)
        tlen = ae[tl] if tl else 0;  buf = ae[tb] if tb else 0

        def _tok2():
            if buf and tlen > 0:
                return bytes(ae[buf + i] & 0xFF for i in range(min(tlen, 30))).decode('ascii', 'replace')
            return f'(tlen={tlen})'

        if result == r_ref2.get('tlenck', 0) and r_ref2.get('tlenck'):
            state2['line_n'] += 1
            tok = _tok2()
            if tlen > 0:
                print(f"  L{state2['line_n']:04d}  dispatch  {tok!r}")
        elif result == r_ref2.get('ma_tlenck', 0) and r_ref2.get('ma_tlenck'):
            tok = _tok2()
            if tlen > 0:
                print(f"         MA-tok   {tok!r}")
            else:
                print(f"         MA-tok   (EOL — stops here)")
        elif result == r_ref2.get('ma0ck', 0) and r_ref2.get('ma0ck'):
            ra_ma0 = r_ref2.get('ra_ma0', 0); ra_ma1 = r_ref2.get('ra_ma1', 0)
            ma0 = ae[ra_ma0] if ra_ma0 else 0; ma1 = ae[ra_ma1] if ra_ma1 else 0
            sym2 = r_ref2.get('sym', {})
            ma0n = sym2.get(ma0, f'#{ma0}'); ma1n = sym2.get(ma1, f'#{ma1}')
            ra_lb = r_ref2.get('ra_lb', 0); lb = ae[ra_lb] if ra_lb else '?'
            print(f"         builder  MA0={ma0n}  MA1={ma1n}  RA_LOAD_BYTE={lb}")
        return result

    def _eq2(self, a1, a2, exit, nxt, ae):
        ra_lb = r_ref2.get('ra_lb', 0); ld_lf = r_ref2.get('ld_lf', 0)
        ld_flag = r_ref2.get('ld_flag', 0)
        if a1 == ra_lb and a2 == ld_lf and exit == ld_flag and ra_lb:
            lb = ae[ra_lb]
            tl = r_ref2.get('tl', 0); tlen = ae[tl] if tl else 0
            tb = r_ref2.get('tb', 0); buf = ae[tb] if tb else 0
            tok = bytes(ae[buf + i] & 0xFF for i in range(min(tlen, 15))).decode('ascii', 'replace') \
                  if (buf and tlen > 0) else '?'
            if lb == 10:
                print(f"         !! PRELF=LF  byte={lb}  tok={tok!r}  ← MA1 will be SKIPPED")
        return orig_eq2(self, a1, a2, exit, nxt, ae)

    def _ur2(self, symbols):
        result = orig_ur2(self, symbols)
        r_ref2.clear()
        r_ref2.update({v: k for k, v in symbols.items()})
        r_ref2['sym']       = {v: k for k, v in symbols.items()}
        r_ref2['tlenck']    = symbols.get('LOAD_DL_TLENCK', 0)
        r_ref2['ma_tlenck'] = symbols.get('LOAD_MA_TLENCK', 0)
        r_ref2['ma0ck']     = symbols.get('LOAD_CU_MA0CK', 0)
        r_ref2['ra_lb']     = symbols.get('RA_LOAD_BYTE', 0)
        r_ref2['ra_ma0']    = symbols.get('RA_MA0', 0)
        r_ref2['ra_ma1']    = symbols.get('RA_MA1', 0)
        r_ref2['tl']        = symbols.get('RA_LOAD_TLEN', 0)
        r_ref2['tb']        = symbols.get('BS_TOKBUF_BASE', 0)
        r_ref2['ld_flag']   = symbols.get('LD_FLAG', 0)
        a2_ = self.aether.aether
        for k, v in symbols.items():
            if a2_[v] == 10 and 'LF' in k:
                r_ref2['ld_lf'] = v; break
        return result

    _imod.Interpreter._do_syscall     = _do2
    _imod.Interpreter._redi           = _r2
    _imod.Interpreter._cmpeq          = _eq2
    _imod.Interpreter.update_relations = _ur2

    print("=== LOAD_MAIN trace ===")
    print("Shows: [FILE] opens │ L#### dispatch TOKEN │ MA-tok args │ !! PRELF=LF anomalies\n")

    try:
        _ldr_mod.freeze()
    except Exception as exc:
        print(f"\n[EXCEPTION] {exc}")
    finally:
        _imod.Interpreter._do_syscall     = orig_do2
        _imod.Interpreter._redi           = orig_r2
        _imod.Interpreter._cmpeq          = orig_eq2
        _imod.Interpreter.update_relations = orig_ur2

    print(f"\n=== Done. {state2['open_count']} files opened, last: {state2['current_file']!r} ===")




def run(els=None):
    if els is None:
        els = sys.argv[1:]
    cfg = _parse_args(els)

    import io
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    l = freeze()
    sys.stderr = old

    a        = l.interp.aether.aether
    R        = l.symbols
    sym      = {v: k for k, v in R.items()}
    dispatch = l.interp._dispatch
    bump     = l._bump

    def name(addr):
        if addr == 0: return 'NULL'
        return sym.get(addr, f'#{addr}')

    def reg(addr):
        return a[addr] if addr and addr < len(a) else 0

    # Entry point
    if cfg['compile_mode']:
        pc = R.get('P0_NID', 0)
        if not pc:
            print("ERROR: P0_NID not found"); return
    elif cfg['start_name']:
        raw = cfg['start_name']
        pc = R.get(raw, int(raw) if raw.lstrip('-').isdigit() else 0)
        if not pc:
            print(f"ERROR: symbol {raw!r} not found"); return
    else:
        pc = R.get('P0_NID', 0) or R.get('PS_MAIN', 0)
        if not pc:
            print("ERROR: no entry point (P0_NID or PS_MAIN)"); return

    until_addr   = R.get(cfg['until_name'], 0) if cfg['until_name'] else 0
    watch_addrs  = {R[s]: s for s in cfg['watch_names'] if s in R}
    watch_prev   = {addr: reg(addr) for addr in watch_addrs}
    fault_vector = l.interp._fault_vector
    ether_entry  = a[fault_vector] if fault_vector else 0

    # Aspect op-ids for structural detection
    voca_op = a[R['Voca'] + SLOT_OP] if 'Voca' in R else 0
    redi_op = a[R['Redi'] + SLOT_OP] if 'Redi' in R else 0

    # Key registers
    ra_link    = R.get('RA_LINK', 0)
    ra_instr   = R.get('RA_INSTR', 0)
    ra_nxt_n   = R.get('RA_NXT_N', 0)
    ra_rule    = R.get('RA_RULE', 0)
    ra_ob_pos  = R.get('RA_OB_POS', 0)
    ra_sr_out  = R.get('RA_SR_OUT', 0)
    ra_ep_cnt  = R.get('RA_EP_BUF_CNT', 0)
    ra_ep_base = R.get('RA_EP_BUF_BASE', 0)

    hotspot    = {} if cfg['profile'] else None
    call_stack = [] if cfg['stack'] else None
    anomalies  = []
    steps      = 0

    print(f"Trace: start={name(pc)} ({pc})  max={cfg['max_steps']:,}")
    if fault_vector:
        print(f"Fault vector: {fault_vector} -> {name(ether_entry)} (active)")
    print()

    while pc and steps < cfg['max_steps']:
        if until_addr and pc == until_addr:
            print(f"  [UNTIL] reached {name(pc)} at step {steps}")
            break

        steps += 1
        pn    = name(pc)
        op_id = a[pc + SLOT_OP]
        a1    = a[pc + SLOT_E1]
        a2    = a[pc + SLOT_E2]
        ex    = a[pc + SLOT_EXIT]
        nxt   = a[pc + SLOT_NEXT]   # correct: read from SLOT_NEXT, not pc+ITO_SIZE

        # Watch registers
        for waddr, wiris_str in watch_addrs.items():
            cur = reg(waddr)
            if cur != watch_prev[waddr]:
                print(f"  WATCH {wiris_str}: {name(watch_prev[waddr])} -> {name(cur)}  (step {steps}, pc={pn})")
                watch_prev[waddr] = cur

        if hotspot is not None:
            hotspot[pc] = hotspot.get(pc, 0) + 1

        # Fault vector trip: execution arrived at the fault handler
        if ether_entry and pc == ether_entry:
            anomalies.append({
                'step': steps, 'pc': pc, 'pc_name': pn,
                'type': 'FAULT_VECTOR_TRIP',
                'ra_link': reg(ra_link), 'ra_instr': reg(ra_instr),
                'ra_rule': reg(ra_rule),
            })
            if cfg['fault']:
                break

        # Call stack tracking
        if call_stack is not None:
            if op_id == voca_op and a1:
                target = a[a1]
                call_stack.append({'caller': pc, 'caller_name': pn,
                                   'target': target, 'step': steps})
            elif op_id == redi_op and call_stack:
                call_stack.pop()

        fn = dispatch.get(op_id)
        if fn is None:
            anomalies.append({
                'step': steps, 'pc': pc, 'pc_name': pn,
                'type': 'UNKNOWN_OP', 'op_id': op_id,
                'op_name': name(op_id),
                'a1': a1, 'a2': a2, 'ex': ex, 'nxt': nxt,
                'ra_link': reg(ra_link), 'ra_instr': reg(ra_instr),
                'ra_rule': reg(ra_rule),
            })
            if cfg['fault'] or not cfg['verbose']:
                break
            pc = nxt
            continue

        if cfg['verbose']:
            print(f"s={steps:8d}  {pn:35s}  {name(op_id):12s}  E1={name(a1):25s}  Exit={name(ex)}")

        pc = fn(a1, a2, ex, nxt, a)

    # ── Summary ───────────────────────────────────────────────────────────────

    print(f"\n=== {steps:,} steps, {len(anomalies)} anomalies ===\n")

    if anomalies:
        print(f"ANOMALIES ({len(anomalies)}):")
        for an in anomalies[:20]:
            atype = an['type']
            print(f"  [{atype}] step={an['step']} pc={an['pc_name']}({an['pc']})")
            if atype == 'UNKNOWN_OP':
                print(f"    op_id    = {an['op_id']} ({an['op_name']})")
                print(f"    a1       = {an['a1']} ({name(an['a1'])})")
                print(f"    ex       = {an['ex']} ({name(an['ex'])})")
                print(f"    nxt      = {an['nxt']} ({name(an['nxt'])})")
                print(f"    RA_LINK  = {an['ra_link']} ({name(an['ra_link'])})")
                print(f"    RA_INSTR = {an['ra_instr']} ({name(an['ra_instr'])})")
                print(f"    RA_RULE  = {an['ra_rule']} ({name(an['ra_rule'])})")
            elif atype == 'FAULT_VECTOR_TRIP':
                print(f"    RA_LINK  = {an['ra_link']} ({name(an['ra_link'])})")
                print(f"    RA_INSTR = {an['ra_instr']} ({name(an['ra_instr'])})")
                print(f"    RA_RULE  = {an['ra_rule']} ({name(an['ra_rule'])})")
            print()
        if len(anomalies) > 20:
            print(f"  ... and {len(anomalies)-20} more")
    else:
        print(f"No anomalies.  Final PC: {name(pc)} ({pc})")

    # Register summary
    print(f"\nRegisters:")
    for rn, ra in [('RA_LINK', ra_link), ('RA_INSTR', ra_instr),
                   ('RA_RULE', ra_rule), ('RA_NXT_N', ra_nxt_n),
                   ('RA_SR_OUT', ra_sr_out), ('OB_pos', ra_ob_pos)]:
        if ra:
            print(f"  {rn:12s} = {name(reg(ra))}")

    if ra_ep_cnt:
        cnt = reg(ra_ep_cnt); ep_base = reg(ra_ep_base)
        print(f"\nEntry buffer: {cnt} found, base={name(ep_base)}")
        if ep_base and cnt > 0:
            for i in range(min(cnt, 16)):
                ep_addr = a[ep_base + i]
                print(f"  [{i}] {name(ep_addr)} ({ep_addr})")

    if call_stack:
        print(f"\nCall stack ({len(call_stack)} frames):")
        for frame in reversed(call_stack[-10:]):
            print(f"  -> {name(frame['target'])}  from {frame['caller_name']} step {frame['step']}")

    if hotspot:
        print(f"\nTop-20 hotspots ({steps:,} steps):")
        top = sorted(hotspot.items(), key=lambda x: -x[1])[:20]
        for addr, cnt in top:
            pct = cnt * 100 / steps if steps else 0
            print(f"  {name(addr):45s}  {cnt:8,}  ({pct:.1f}%)")

    if cfg['compile_mode']:
        print(f"\nCompiler registers after run:")
        for rn in ['RA_EP_BUF_CNT','RA_EP_BUF_BASE','RA_EP_COUNT',
                   'RA_VS_BASE','RA_SSA_BASE','RA_RT_BASE','RA_BQ_BASE']:
            addr = R.get(rn)
            if addr:
                print(f"  {rn:20s} = {a[addr]} ({name(a[addr])})")


if __name__ == '__main__':
    if '--loadmain' in sys.argv:
        run_loadmain()
    else:
        run()
