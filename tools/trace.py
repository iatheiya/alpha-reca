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

New flags (session 2026-06-23):
    python3 trace.py --write-watch SYM        # print old->new every time SYM address is written
    python3 trace.py --token-at SYM           # dump tokbuf+TLEN every time PC reaches SYM
    python3 trace.py --fidx                   # print SK_FIDX file-index transitions during LOAD_MAIN
    python3 trace.py --context N              # history depth for --write-watch / --token-at (default 30)

    python3 trace.py --find-writer SYM [SYM2...]
        Find EVERY instruction that writes to SYM's aether address.
        Catches Write (double-indirect dest), Move/Add/Sub/Mul/Read (direct exit=SYM).
        Prints: fi, NOVA name, op type, value written, and recent PC history.
        Example: python3 trace.py --find-writer LF TAB NUL

Examples:
    # Watch what writes to TAB and LF
    python3 trace.py --write-watch TAB --write-watch LF

    # Find ALL instructions that write to LF (the new find-writer):
    python3 trace.py --find-writer LF

    # Dump tokbuf every time BS_PARSE_INT is entered (max 10 hits, 20 steps context)
    python3 trace.py --token-at BS_PARSE_INT --context 20

    # Track file index transitions + halt info during LOAD_MAIN
    python3 trace.py --fidx

    # Combine: fidx + write-watch
    python3 trace.py --fidx --write-watch LF --write-watch TAB

Reusable API (import from trace):
    from trace import make_patched_run
    results = make_patched_run(
        on_write_dest={'LF': lambda ev: print(ev)},
        on_voca_target={'LOAD_DISPATCH_CORE': lambda ev: print(ev)},
        context=30
    )
    # ev is a dict: {fi, step, op, pc_name, a1, a2, value, history}
"""
import sys, os, io

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import freeze, load_symbols, _BIN, load_or_freeze
from interpreter import Interpreter
from aether import SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, NOVA_SIZE

def _parse_args(els):
    cfg = dict(
        start_name=None, max_steps=10_000_000,
        until_name=None, verbose=False,
        watch_names=[], profile=False,
        fault=False, stack=False, compile_mode=False,
        loadmain=False,
        write_watch=[],   # --write-watch SYM  -- print old->new every time SYM is written
        token_at=[],      # --token-at SYM     -- dump tokbuf when PC reaches SYM
        fidx=False,       # --fidx             -- print SK_FIDX transitions
        context=30,       # --context N        -- lines of PC history for write-watch/token-at
        start_step=0,     # --start-step N     -- only activate watches from step N
        stop_step=0,      # --stop-step N      -- halt entire trace at step N (0=no limit)
        after_step=0,     # --after-step N     -- ignore pc/write hits before step N
        progress_every=0, # --progress-every N -- print SK_FIDX every N steps during freeze
        loop_detect=False,# --loop-detect      -- detect repeating PC-state cycles
        dump_syms=[],     # --dump SYM         -- print lux wiring after freeze
        stack_depth=False,# --stack-depth      -- print call depth on every Voca/Redi
        find_writer=[],   # --find-writer SYM  -- find every write to SYM's aether address
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
        elif a == '--write-watch' and i + 1 < len(els):
            cfg['write_watch'].append(els[i + 1]); i += 2; continue
        elif a == '--token-at' and i + 1 < len(els):
            cfg['token_at'].append(els[i + 1]); i += 2; continue
        elif a == '--fidx':
            cfg['fidx'] = True; i += 1
        elif a == '--context' and i + 1 < len(els):
            cfg['context'] = int(els[i + 1]); i += 2; continue
        elif a == '--start-step' and i + 1 < len(els):
            cfg['start_step'] = int(els[i + 1]); i += 2; continue
        elif a == '--stop-step' and i + 1 < len(els):
            cfg['stop_step'] = int(els[i + 1]); i += 2; continue
        elif a == '--after-step' and i + 1 < len(els):
            cfg['after_step'] = int(els[i + 1]); i += 2; continue
        elif a == '--progress-every' and i + 1 < len(els):
            cfg['progress_every'] = int(els[i + 1]); i += 2; continue
        elif a == '--loop-detect':
            cfg['loop_detect'] = True; i += 1
        elif a == '--dump' and i + 1 < len(els):
            cfg['dump_syms'].append(els[i + 1]); i += 2; continue
        elif a == '--stack-depth':
            cfg['stack_depth'] = True; i += 1
        elif a == '--find-writer' and i + 1 < len(els):
            cfg['find_writer'].append(els[i + 1]); i += 2; continue
        else:
            i += 1
    return cfg


def run_loadmain():
    """Trace LOAD_MAIN: show file opens, top-level dispatch tokens,
    MA arg reads, and RA_LOAD_BYTE=LF anomalies (PRELF check).

    This is the dedicated diagnostic for the self-hosting loop:
    'Why does LOAD_MAIN stop early, and at exactly which file+line?'
    """
    _io = io

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
    orig_do = type(l.interp)._do_syscall

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

    type(l.interp)._do_syscall = _do

    # ── Redi → track LOAD_DL_TLENCK (top-level dispatch), LOAD_MA_TLENCK, LOAD_CU_MA0CK
    orig_r = type(l.interp)._redi

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

    type(l.interp)._redi = _redi

    # ── Equal → detect LOAD_MA_PRELF (RA_LOAD_BYTE == LF) ────────────────────
    orig_eq = type(l.interp)._cmpeq

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

    type(l.interp)._cmpeq = _cmpeq

    # ── update_relations → cache symbol addresses ─────────────────────────────
    orig_ur = type(l.interp).update_relations

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

    type(l.interp).update_relations = _ur

    print("=== LOAD_MAIN trace ===")
    print("Shows: [FILE] opens │ L#### dispatch TOKEN │ MA-tok args │ !! PRELF=LF anomalies")
    print()

    # Run LOAD_MAIN (it already ran in freeze(); re-freeze to see it live)
    # Actually freeze() already ran above. We need to re-run just LOAD_MAIN.
    # We can do this by directly executing it:
    _io2 = io
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
        pc = R.get('P0_NID', 0)
        if not pc:
            print("ERROR: no entry point (P0_NID)"); return

    until_addr   = R.get(cfg['until_name'], 0) if cfg['until_name'] else 0
    watch_addrs  = {R[s]: s for s in cfg['watch_names'] if s in R}
    watch_prev   = {addr: reg(addr) for addr in watch_addrs}
    fault_vector = l.interp._fault_vector
    ether_entry  = a[fault_vector] if fault_vector else 0

    # write-watch: intercept Write/Move ops targeting these addresses
    write_watch_addrs = {R[s]: s for s in cfg['write_watch'] if s in R}
    _step_ctr = [0]  # shared step counter for write-watch closures
    if write_watch_addrs:
        _write_op = a[R['Write'] + SLOT_OP] if 'Write' in R else None
        _move_op  = a[R['Move']  + SLOT_OP] if 'Move'  in R else None
        _ctx = cfg['context']
        _ww_history = []
        _orig_disp = dict(l.interp._dispatch)
        def _traced_write(a1, a2, exit, nxt, ae):
            tgt = int(ae[a1]) if a1 else 0
            if tgt in write_watch_addrs:
                old_v = int(ae[tgt]); new_v = int(ae[a2]) if a2 else 0
                print(f"\n  [write-watch] WRITE {write_watch_addrs[tgt]}@{tgt}: "
                      f"{old_v}({sym.get(old_v,'')}) -> {new_v}({sym.get(new_v,'')})")
                for h in _ww_history[-_ctx:]: print(f"    {h}")
            return _orig_disp[_write_op](a1, a2, exit, nxt, ae)
        def _traced_move(a1, a2, exit, nxt, ae):
            tgt = exit
            if tgt and tgt in write_watch_addrs:
                old_v = int(ae[tgt]); new_v = int(ae[a1]) if a1 else 0
                print(f"\n  [write-watch] MOVE {write_watch_addrs[tgt]}@{tgt}: "
                      f"{old_v}({sym.get(old_v,'')}) -> {new_v}({sym.get(new_v,'')})")
                for h in _ww_history[-_ctx:]: print(f"    {h}")
            return _orig_disp[_move_op](a1, a2, exit, nxt, ae)
        if _write_op and _write_op in l.interp._dispatch:
            l.interp._dispatch[_write_op] = _traced_write
        if _move_op and _move_op in l.interp._dispatch:
            l.interp._dispatch[_move_op] = _traced_move
    else:
        _ww_history = None

    # token-at: dump tokbuf when PC reaches any of these addresses
    token_at_addrs = {R[s]: s for s in cfg['token_at'] if s in R}
    _tokbuf = R.get('BS_TOKBUF_BASE')
    _tlen   = R.get('RA_LOAD_TLEN')
    _sk_fidx = R.get('SK_FIDX')

    # fidx tracking
    _last_fidx = [-1]
    # Plain alphabetical order, matching auto_discover's own (loader.py):
    # "File order is irrelevant" there (Wave 1 sizes every name across all
    # files before Wave 2 mints one), so there is nothing to special-case
    # here either -- 'macros.re first' was never a real requirement, just
    # an assumption copied from a stale comment elsewhere. Found while
    # deleting macros.re (unrelated to that deletion otherwise).
    # Exactly the list LOAD_MAIN walks -- the same loader function freeze()
    # uses, so file-index names can never drift from the real order.
    from loader import load_main_order, seed_candidates
    _file_order = [os.path.relpath(f, _ROOT) for f in load_main_order(seed_candidates()[0])]

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
    stack_depth_log = [] if cfg['stack_depth'] else None
    anomalies  = []
    _start_step   = cfg['start_step']
    _stop_step    = cfg['stop_step']
    _after_step   = cfg['after_step']
    _prog_every   = cfg['progress_every']
    # loop-detect: record (pc, key-register snapshot) for cycle detection
    _loop_snap    = {} if cfg['loop_detect'] else None
    _voca_op_id   = a[R['Voca'] + SLOT_OP] if 'Voca' in R else 0
    _redi_op_id   = a[R['Redi'] + SLOT_OP] if 'Redi' in R else 0
    _call_depth   = [0]
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
        _step_ctr[0] = steps
        pn    = name(pc)
        op_id = a[pc + SLOT_OP]
        a1    = a[pc + SLOT_E1]
        a2    = a[pc + SLOT_E2]
        ex    = a[pc + SLOT_EXIT]
        nxt   = a[pc + SLOT_NEXT]   # correct: read from SLOT_NEXT, not pc+NOVA_SIZE

        # write-watch history
        if _ww_history is not None:
            _ww_history.append(pn)
            if len(_ww_history) > cfg['context'] + 5:
                _ww_history.pop(0)

        # token-at: dump tokbuf when PC is a watched address
        if token_at_addrs and pc in token_at_addrs and steps >= _start_step and steps >= _after_step:
            tl = int(a[_tlen]) if _tlen else 0
            raw = bytes(int(a[_tokbuf + i]) & 0xFF for i in range(max(tl, 1))) if _tokbuf else b''
            tok = ''.join(chr(b) if 32 <= b < 127 else f'\\x{b:02x}' for b in raw[:tl])
            fidx = int(a[_sk_fidx]) if _sk_fidx else -1
            print(f"  [token-at] {token_at_addrs[pc]}  fidx={fidx}  TLEN={tl}  tok={tok!r}")

        # fidx transitions
        if cfg['fidx'] and _sk_fidx:
            fidx = int(a[_sk_fidx])
            if fidx != _last_fidx[0]:
                fname = _file_order[fidx] if 0 <= fidx < len(_file_order) else '?'
                print(f"  [fidx] step={steps}  SK_FIDX={fidx}  {fname}")
                _last_fidx[0] = fidx

        # stop-step: halt entire trace early
        if _stop_step and steps > _stop_step:
            print(f"  [stop-step] halting at step {steps} (--stop-step={_stop_step})")
            break

        # progress-every: print SK_FIDX periodically
        if _prog_every and steps % _prog_every == 0 and _sk_fidx:
            fidx = int(a[_sk_fidx])
            fname = _file_order[fidx] if 0 <= fidx < len(_file_order) else '?'
            print(f"  [progress] step={steps}  SK_FIDX={fidx}  {fname}")

        # loop-detect: check for repeating PC-state cycles
        if _loop_snap is not None and steps % 100 == 0:
            snap_key = (pc, int(a[R['SK_FIDX']]) if _sk_fidx else 0)
            if snap_key in _loop_snap:
                print(f"  [loop-detect] CYCLE DETECTED at step={steps}: PC={name(pc)} " +
                      f"last seen at step={_loop_snap[snap_key]}")
                break
            _loop_snap[snap_key] = steps
            if len(_loop_snap) > 5000: _loop_snap.clear()  # periodic reset

        # stack-depth: print on every Voca/Redi
        if stack_depth_log is not None:
            if op_id == _voca_op_id:
                _call_depth[0] += 1
                if len(stack_depth_log) < 500:
                    stack_depth_log.append(f"step={steps}  depth={_call_depth[0]}  VOCA -> {name(int(a1))}")
            elif op_id == _redi_op_id and _call_depth[0] > 0:
                _call_depth[0] -= 1

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

    # --dump: print lux wiring for requested symbols
    if cfg['dump_syms']:
        print(f"\nLux wiring (--dump):")
        SO, SE1, SE2, SEX, SN = SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT
        for sname in cfg['dump_syms']:
            addr = R.get(sname, 0)
            if not addr:
                print(f"  {sname}: NOT FOUND")
                continue
            word = int(a[addr]); op = int(a[addr+SO]); e1v = int(a[addr+SE1])
            e2v = int(a[addr+2]); exv = int(a[addr+SEX]); nxv = int(a[addr+SN])
            self_ok = "✓" if word == addr else "✗ BAD"
            print(f"  {sname:30s} @{addr}")
            print(f"    word={word} {self_ok}  op={name(op)}  e1={name(e1v)}  e2={name(e2v)}")
            print(f"    exit={name(exv)}  next={'0(implicit)' if nxv==0 else name(nxv)}")

    # --stack-depth: print call depth log
    if stack_depth_log:
        print(f"\nCall depth log ({len(stack_depth_log)} entries, max shown 50):")
        for entry in stack_depth_log[:50]:
            print(f"  {entry}")
        if len(stack_depth_log) > 50:
            print(f"  ... and {len(stack_depth_log)-50} more")

    if cfg['compile_mode']:
        print(f"\nCompiler registers after run:")
        for rn in ['RA_EP_BUF_CNT','RA_EP_BUF_BASE','RA_EP_COUNT',
                   'RA_VS_BASE','RA_SSA_BASE','RA_RT_BASE','RA_BQ_BASE']:
            addr = R.get(rn)
            if addr:
                print(f"  {rn:20s} = {a[addr]} ({name(a[addr])})")




# ─────────────────────────────────────────────────────────────────────────────
# make_patched_run — reusable dispatch-table patcher
# ─────────────────────────────────────────────────────────────────────────────
#
# Eliminates the 40-line boilerplate that every debug script repeated:
#   fi tracking, dispatch patching, orig-save/restore, patched_run wrapper.
#
# Usage:
#   from trace import make_patched_run
#
#   events = []
#   results = make_patched_run(
#       on_write_dest={'LF': lambda ev: events.append(ev)},
#       on_voca_target={'LOAD_DISPATCH_CORE': lambda ev: events.append(ev)},
#       context=30,
#   )
#   # results['loader'] = Loader object after freeze()
#   # results['fi_events'] = list of fi transitions
#
# Event dict always contains:
#   fi, step, op ('Write'/'Move'/'Add'/'Voca'/...), pc (int), pc_name (str),
#   a1, a2, value (what was written / what target the Voca called), history (list of pc_names)
#
# Callbacks fire BEFORE the original handler executes, so aether still has old value.

def make_patched_run(
    on_write_dest=None,   # {sym_name: callback(ev)} — fires when Write/Move/Add writes to sym
    on_write_addr=None,   # {raw_addr: callback(ev)} — same, but by raw aether address (no
                           # Python symbol needed — for self-hosted-only addresses)
    on_voca_target=None,  # {sym_name: callback(ev)} — fires when Voca's target is sym
    on_equal_nxt=None,    # {sym_name: callback(ev)} — fires when Equal fires with this nxt (NOVA identity)
    context=30,           # PC history depth
    suppress_stderr=True, # hide freeze() stderr output
    sample_every=0,       # if >0, append (step, list-of-recent-pc-names) to the returned
                           # 'samples' list every this-many steps — for seeing what a long
                           # run is doing deep in, without needing to name a specific symbol
                           # to watch (e.g. suspected infinite loops far from any known hook)
):
    """
    Run freeze() with patches applied to the interpreter dispatch table.
    Returns dict with keys: loader, fi_events.
    All callback dicts map symbol NAME → fn(event_dict).
    See module docstring for event_dict fields.
    """
    import io as _io
    import loader as _ldr
    import interpreter as _imod

    # load_symbols() silently returns {} if reca.sym doesn't exist yet --
    # every on_*_target/on_write_dest dict would then resolve to nothing,
    # and every callback would silently never fire (looks exactly like
    # "this never happens" instead of "the tool never turned on"). Force
    # a real build first if needed, so symbol resolution below is never
    # silently empty. See BUGS.md Pattern Z's tracing-tool-reliability
    # notes -- this exact failure mode cost real debugging time this
    # session before being traced back to here.
    syms = load_symbols()
    if not syms:
        _buf = _io.StringIO(); _old = sys.stderr; sys.stderr = _buf
        try:
            _ldr.freeze()
        finally:
            sys.stderr = _old
        syms = load_symbols()
        if not syms:
            raise RuntimeError(
                "make_patched_run: symbol table is still empty after a "
                "fresh freeze() -- something is genuinely broken, not "
                "just a missing/stale reca.sym file.")
    rev  = {v: k for k, v in syms.items() if v}

    # Resolve symbol names → aether addresses
    def _resolve_names(d):
        if not d: return {}
        missing = [k for k in d if k not in syms]
        if missing:
            raise KeyError(
                f"make_patched_run: unknown symbol name(s) {missing!r} -- "
                f"check spelling; a silently-empty callback dict here "
                f"looks identical to 'this event never happens'.")
        return {syms[k]: v for k, v in d.items() if k in syms}

    wd_cbs  = _resolve_names(on_write_dest)    # addr → callback
    if on_write_addr:
        wd_cbs.update(on_write_addr)           # raw addr → callback, no name needed
    vt_cbs  = _resolve_names(on_voca_target)   # addr → callback (Voca target lux addr)
    eq_cbs  = _resolve_names(on_equal_nxt)     # addr → callback (nxt of the Equal NOVA)

    orig_run = _imod.Interpreter._run
    fi_cur   = [0]
    step_ctr = [0]
    history  = []          # rolling list of recent pc_names
    fi_evts  = []          # fi transitions: (step, old_fi, new_fi)
    samples  = []          # (step, list-of-recent-pc-names), every sample_every steps

    def _push_hist(name):
        history.append(name)
        if len(history) > context + 5:
            history.pop(0)
        if sample_every and step_ctr[0] % sample_every == 0:
            samples.append((step_ctr[0], list(history)))

    def _mk_ev(op, aether, pc, a1, a2, value):
        return dict(
            fi=fi_cur[0], step=step_ctr[0], op=op,
            pc=pc, pc_name=rev.get(pc, f'#{pc}'),
            a1=a1, a2=a2, value=value,
            history=list(history[-context:]),
        )

    def patched_run(self, start, progress_every=0, sym_arg=None,
                    use_cache=True, max_steps=0):
        a = self.aether.aether
        sk_fidx  = syms.get('SK_FIDX', 0)

        move_op  = syms.get('Move', 0)
        add_op   = syms.get('Add', 0)
        sub_op   = syms.get('Sub', 0)
        mul_op   = syms.get('Mul', 0)
        write_op = syms.get('Write', 0)
        read_op  = syms.get('Read', 0)
        voca_op  = syms.get('Voca', 0)
        equal_op = syms.get('Equal', 0)

        om = self._dispatch.get(move_op)
        oa = self._dispatch.get(add_op)
        os_ = self._dispatch.get(sub_op)
        omu = self._dispatch.get(mul_op)
        ow = self._dispatch.get(write_op)
        or_ = self._dispatch.get(read_op)
        ov = self._dispatch.get(voca_op)
        oe = self._dispatch.get(equal_op)

        # --- fi tracking (always active) ---
        _prev_fi = [fi_cur[0]]
        def _track_fi(new_fi):
            old = fi_cur[0]
            if new_fi != old:
                fi_evts.append((step_ctr[0], old, new_fi))
                fi_cur[0] = new_fi

        def tmove(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if ex == sk_fidx: _track_fi(int(ae[a1]))
            if wd_cbs and ex in wd_cbs:
                wd_cbs[ex](_mk_ev('Move', ae, nxt, a1, a2, int(ae[a1]) if a1 else 0))
            return om(a1, a2, ex, nxt, ae)

        def tadd(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if ex == sk_fidx:
                _track_fi(int((ae[a1] + ae[a2]) & 0xFFFFFFFFFFFFFFFF) if (a1 and a2) else 0)
            if wd_cbs and ex in wd_cbs:
                val = int((ae[a1] + ae[a2]) & 0xFFFFFFFFFFFFFFFF) if (a1 and a2) else 0
                wd_cbs[ex](_mk_ev('Add', ae, nxt, a1, a2, val))
            return oa(a1, a2, ex, nxt, ae)

        def tsub(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if wd_cbs and ex in wd_cbs:
                from aether import _MASK64
                from interpreter import _s64
                val = int((_s64(ae[a1]) - _s64(ae[a2])) & _MASK64) if (a1 and a2) else 0
                wd_cbs[ex](_mk_ev('Sub', ae, nxt, a1, a2, val))
            return os_(a1, a2, ex, nxt, ae)

        def tmul(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if wd_cbs and ex in wd_cbs:
                from interpreter import _s64
                val = int((_s64(ae[a1]) * _s64(ae[a2])) & 0xFFFFFFFFFFFFFFFF) if (a1 and a2) else 0
                wd_cbs[ex](_mk_ev('Mul', ae, nxt, a1, a2, val))
            return omu(a1, a2, ex, nxt, ae)

        def twrite(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if wd_cbs and a1:
                dest = int(ae[a1])
                if dest in wd_cbs:
                    val = int(ae[a2]) if a2 else 0
                    wd_cbs[dest](_mk_ev('Write', ae, nxt, a1, a2, val))
            return ow(a1, a2, ex, nxt, ae)

        def tread(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if wd_cbs and ex in wd_cbs:
                val = int(ae[ae[a1]]) if a1 else 0
                wd_cbs[ex](_mk_ev('Read', ae, nxt, a1, a2, val))
            return or_(a1, a2, ex, nxt, ae)

        def tvoca(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if vt_cbs and a1:
                target = int(ae[a1])
                if target in vt_cbs:
                    vt_cbs[target](_mk_ev('Voca', ae, nxt, a1, a2, target))
            if ex:
                if ex == self._ra_link and self._ra_sp:
                    sp = ae[self._ra_sp] - self._ra_frame_size
                    ae[self._ra_sp] = sp; ae[sp] = ae[ex]
                ae[ex] = nxt
            return int(ae[a1]) if a1 else 0

        def tequal(a1, a2, ex, nxt, ae):
            step_ctr[0] += 1; _push_hist(rev.get(nxt, f'#{nxt}'))
            if eq_cbs and nxt in eq_cbs:
                eq_cbs[nxt](_mk_ev('Equal', ae, nxt, a1, a2, int(ae[a1]) if a1 else 0))
            return oe(a1, a2, ex, nxt, ae)

        # Patch only ops we actually need
        needs_move  = sk_fidx or (wd_cbs and any(True for _ in wd_cbs))
        if om and needs_move:    self._dispatch[move_op]  = tmove
        if oa:                   self._dispatch[add_op]   = tadd
        if os_ and wd_cbs:      self._dispatch[sub_op]   = tsub
        if omu and wd_cbs:      self._dispatch[mul_op]   = tmul
        if ow and wd_cbs:       self._dispatch[write_op] = twrite
        if or_ and wd_cbs:      self._dispatch[read_op]  = tread
        if ov and vt_cbs:       self._dispatch[voca_op]  = tvoca
        if oe and eq_cbs:       self._dispatch[equal_op] = tequal

        return orig_run(self, start, progress_every, sym_arg, use_cache, max_steps)

    _imod.Interpreter._run = patched_run

    buf = _io.StringIO(); old_err = sys.stderr
    if suppress_stderr: sys.stderr = buf
    try:
        ldr = _ldr.freeze()
    finally:
        if suppress_stderr: sys.stderr = old_err
        _imod.Interpreter._run = orig_run

    return {'loader': ldr, 'fi_events': fi_evts, 'samples': samples}


# ─────────────────────────────────────────────────────────────────────────────
# run_find_writer — CLI implementation of --find-writer
# ─────────────────────────────────────────────────────────────────────────────

def run_watch_pc(pc_names, hit_limit=30, context=15):
    """
    Generic "watch these PC names during a real freeze() run" mode --
    consolidates the one-off `python3 - <<'PYEOF' ... PYEOF` scripts
    this session kept rebuilding from scratch (and re-introducing the
    same methodology bugs into) for simple "does this line get
    reached, how many times, in what order" questions.

    Reuses make_patched_run (already correct -- calls the real,
    unmodified `_run`/`orig_run`, only swaps individual dispatch-table
    handlers, so it automatically takes whichever branch the real
    `freeze()` call actually takes; see BUGS.md/TOOLING.md for why a
    from-scratch loop reimplementation is risky instead).

    Prints, for each watched name: total hit count, and the step number
    + a short PC history for the first `hit_limit` hits. For "does this
    ever get reached at all" and "in what rough order do these things
    happen relative to each other" questions -- most of what this
    session's own one-off scripts were built for -- this one call
    replaces them.

    For anything needing a *register's own value* at each hit (not just
    "was this PC reached"), use make_patched_run directly with its own
    on_write_dest/on_voca_target/on_equal_nxt callbacks instead -- this
    helper is deliberately PC-only, for the common, simpler case.
    """
    syms = load_symbols()
    rev = {v: k for k, v in syms.items() if v}
    missing = [n for n in pc_names if n not in syms]
    if missing:
        print(f'[watch-pc] Unknown symbols: {missing}')
        pc_names = [n for n in pc_names if n in syms]
    if not pc_names:
        print('[watch-pc] No targets.'); return

    targets = {n: syms[n] for n in pc_names}
    hits = {n: [] for n in pc_names}
    counts = {n: 0 for n in pc_names}
    addr_to_name = {v: k for k, v in targets.items()}

    def make_cb(name):
        def cb(ev):
            counts[name] += 1
            if len(hits[name]) < hit_limit:
                hits[name].append((ev['step'], list(ev['history'][-context:])))
        return cb

    # Watch via on_voca_target if the PC is itself a Voca's own target
    # (a real, RVOCA-called entry point); otherwise fall back to
    # on_equal_nxt (works for any JEQ/JZ-reached label) -- between the
    # two, most real labels are reachable one way or the other. For a
    # plain NOVA reached only via auto-chain, neither fires; use
    # make_patched_run directly with a custom hook in that case.
    result = make_patched_run(
        on_voca_target={n: make_cb(n) for n in pc_names},
        on_equal_nxt={n: make_cb(n) for n in pc_names},
        context=context,
    )

    print(f"[watch-pc] Watched: {', '.join(f'{n}@{a}' for n, a in targets.items())}")
    print()
    for n in pc_names:
        print(f"  {n}: {counts[n]} hit(s)")
        for step, hist in hits[n]:
            print(f"    step={step}")
            if hist:
                print(f"      history: {' -> '.join(hist)}")
    a = result['loader'].interp.aether.aether
    sk_fidx = syms.get('SK_FIDX')
    if sk_fidx:
        print()
        print(f"  SK_FIDX at end: {int(a[sk_fidx])}")
    return result



def run_find_writer(target_names, context=30):
    """
    Find every instruction that writes to each target symbol's aether address.
    Catches: Write (dest=aether[a1]), Move/Add/Sub/Mul/Read (dest=exit).
    Prints: fi, NOVA name, op, value written, and recent PC history.
    """
    syms = load_symbols()
    rev  = {v: k for k, v in syms.items() if v}
    missing = [n for n in target_names if n not in syms]
    if missing:
        print(f'[find-writer] Unknown symbols: {missing}')
        target_names = [n for n in target_names if n in syms]
    if not target_names:
        print('[find-writer] No targets.'); return

    targets = {n: syms[n] for n in target_names}
    print(f'[find-writer] Watching writes to: {", ".join(f"{n}@{a}" for n,a in targets.items())}')
    print()

    hit_counts = {n: 0 for n in target_names}

    def make_cb(sym_name, sym_addr):
        def cb(ev):
            hit_counts[sym_name] += 1
            fi    = ev['fi']
            step  = ev['step']
            op    = ev['op']
            pc_n  = ev['pc_name']
            val   = ev['value']
            old_v = '?'  # we fire before the write, so aether still has old value
            hist  = ev['history']
            val_sym = rev.get(val, str(val))
            print(f'  [{sym_name}] fi={fi} step={step} op={op} from={pc_n}')
            print(f'    value → {val} ({val_sym})')
            if hist:
                print(f'    history (last {min(len(hist), context)}):')
                for h in hist[-context:]:
                    print(f'      {h}')
            print()
        return cb

    on_write = {n: make_cb(n, a) for n, a in targets.items()}

    make_patched_run(on_write_dest=on_write, context=context, suppress_stderr=True)

    print('[find-writer] Summary:')
    for n, cnt in hit_counts.items():
        print(f'  {n}: {cnt} writes')


if __name__ == '__main__':
    if '--loadmain' in sys.argv:
        run_loadmain()
    elif '--find-writer' in sys.argv:
        _cfg = _parse_args(sys.argv[1:])
        run_find_writer(_cfg['find_writer'], context=_cfg['context'])
    elif '--watch-pc' in sys.argv:
        _idx = sys.argv.index('--watch-pc')
        _names = []
        for _a in sys.argv[_idx + 1:]:
            if _a.startswith('--'):
                break
            _names.append(_a)
        run_watch_pc(_names)
    else:
        run()
