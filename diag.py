#!/usr/bin/env python3
"""
diag.py — Reca diagnostics.

Usage:
    python3 diag.py                        # full health + graph + macros
    python3 diag.py --health               # freeze health only
    python3 diag.py --graph                # Aether graph integrity
    python3 diag.py --macros               # registered vs used macro check
    python3 diag.py --strings              # string encoding layout
    python3 diag.py --parity file.re       # macro parity: Python loader vs runtime
    python3 diag.py --lost                 # symbols swallowed by comment blocks
    python3 diag.py --deps                 # SCC load order and dependency tree
    python3 diag.py --indent               # what the indent-synthesis mechanism produced
    python3 diag.py --trace [...]          # delegate to trace.py (els forwarded)
    python3 diag.py --lint                 # static lint: phantom ITOs, dead refs, self-ref violations
"""
import sys, os, glob, io, re as _re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from symphony import SLOT_WORD, SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, ITO_SIZE, LUMEN_PAIR
from loader import (freeze, Loader, _read_re_file, _parse_line,
                    _INDENT_LEADERS, _strip_comments)
from interpreter import Interpreter

_HERE = os.path.dirname(os.path.abspath(__file__))


_load_cache = None

def _load():
    global _load_cache
    if _load_cache is not None:
        return _load_cache
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    try:
        ldr = freeze()
    finally:
        sys.stderr = old
    errs = [l for l in buf.getvalue().splitlines() if 'Pass2' in l or 'Error' in l]
    a = ldr.interp.aether.aether; R = ldr.symbols
    sym = {v: k for k, v in R.items()}
    _load_cache = (ldr, a, R, sym, errs)
    return _load_cache


def _name(addr, sym):
    if addr == 0: return 'NULL'
    return sym.get(addr, f'#{addr}')


def _read_packed_string(a, first_chunk):
    """Decode a packed byte-chain in DATA LUX format: [word, 0] pairs, stride=2.
    Used for htable string values and bootstrap intern names.
    NOTE: different from loader._decode_string_word which uses dense stride=1 format."""
    cur = first_chunk; raw = b''
    for _ in range(4096):
        if cur == 0: break
        w = a[cur]
        if w == 0: break
        raw += w.to_bytes(8, 'little')
        cur += 2  # advance to next 2-lux chunk
    nul = raw.find(b'\x00')
    return (raw[:nul] if nul >= 0 else raw).decode('ascii', errors='replace')


def _all_re_files():
    return sorted(glob.glob(os.path.join(_HERE, '**', '*.re'), recursive=True))


# ── health ────────────────────────────────────────────────────────────────────

def mode_health():
    print("=== Reca Freeze Health ===\n")
    ldr, a, R, sym, errs = _load()

    print(f"Symbols:    {len(R)}")
    print(f"Bump:       {ldr._bump}")
    print(f"Watermark:  {ldr._water}")
    print(f"ITO luces:  {len(ldr._ito_addrs)}")
    print(f"Data luces: {len(ldr._data_addrs) - len(ldr._ito_addrs)}")
    print()

    if errs:
        print(f"PASS2 ERRORS ({len(errs)}):")
        for e in errs[:25]: print(f"  {e}")
        if len(errs) > 25: print(f"  ... and {len(errs)-25} more")
    else:
        print("Pass2: clean \u2713")
    print()

    critical = [
        'Move','Jump','JumpIf','Voca','Redi','Read','Write',
        'Add','Sub','Equal','Less','End','Exire','Not','Greater',
        'P0_NID','RA_LINK','K_CURSOR','SC_NR','SC_A0',
        'EMIT_STR_ENTRY','PUT_BYTE','PB_FLUSH',
        'ALLOC_LUX','ALLOC_LUCES','HT_INSERT','HT_LOOKUP',
        'SCAN_ALL_LUX','SR_FIRST_LM','SR_NEXT_LM',
    ]
    missing = [s for s in critical if s not in R]
    if missing:
        print(f"MISSING CRITICAL ({len(missing)}): {missing}")
    else:
        print(f"Critical symbols ({len(critical)}): all present \u2713")
    print()

    ito_sym = a[R['ITO_SIZE']] if 'ITO_SIZE' in R else None
    if ito_sym is not None:
        tag = "\u2713" if ito_sym == ITO_SIZE else "\u2717 MISMATCH"
        print(f"ITO_SIZE: symphony.py={ITO_SIZE}, constants.re={ito_sym}  {tag}")
    print("Canon: aether[0]=0  " + ("\u2713" if a[0] == 0 else "\u2717 VIOLATION"))
    print()

    aspects = [
        'Read','Write','Add','Sub','Mul','Div','Rem','UDiv','URem',
        'And','Or','Xor','Left','Right','ARight','Equal','Less','ULess',
        'JumpIf','JumpReg','End','Exire','Voca','Redi',
        'Move','Jump','Load','Store','Not','Greater',
    ]
    bad = [f'{asp}@{R[asp]}:word={a[R[asp]+SLOT_WORD]}'
           for asp in aspects if asp in R and a[R[asp]+SLOT_WORD] != R[asp]]
    if bad:
        print(f"BAD SELF-REF ({len(bad)}):"); [print(f"  {b}") for b in bad]
    else:
        present = sum(1 for x in aspects if x in R)
        print(f"Self-ref for {present} aspects \u2713")
    print()

    key_regs = ['RA_LINK','RA_OB_POS','RA_INSTR','RA_SSA_CTR',
                'RA_RULE','RA_NXT_N','RA_SR_OUT']
    print("Key registers:")
    for rn in key_regs:
        if rn in R:
            addr = R[rn]; val = a[addr]
            print(f"  {rn:15s} addr={addr}  word={val} ({sym.get(val, str(val))})")
    print()
    return ldr, a, R, sym, errs


# ── graph ─────────────────────────────────────────────────────────────────────

def mode_graph():
    print("=== Aether Graph Integrity ===\n")
    ldr, a, R, sym, errs = _load()
    bump = ldr._bump
    known_addrs = set(R.values())  # R = {name: addr}; known_addrs = set of all symbol addresses
    issues = []

    for addr in ldr._ito_addrs:
        word = a[addr + SLOT_WORD]
        if word != addr and word not in known_addrs:
            # word is neither self-ref nor a NEWREF target pointing to a known symbol → broken
            issues.append(f"ITO@{addr}({sym.get(addr,'?')}): word\u2260self (word={word})")
        op = a[addr + SLOT_OP]
        if op == 0:    issues.append(f"ITO@{addr}({sym.get(addr,'?')}): op=0 unwired")
        elif op>=bump: issues.append(f"ITO@{addr}({sym.get(addr,'?')}): op={op} OOB")
        for slot, sn in [(SLOT_E1,'e1'),(SLOT_E2,'e2'),(SLOT_EXIT,'exit'),(SLOT_NEXT,'next')]:
            v = a[addr+slot]
            if v and v >= bump: issues.append(f"ITO@{addr}({sym.get(addr,'?')}): {sn}={v} OOB")

    from symphony import AETHER_SIZE as _AETHER_SIZE
    for addr in ldr._data_addrs:
        if addr in ldr._ito_addrs_set: continue
        i = 1
        while addr+i < bump:
            rel = a[addr+i]
            if rel == 0: break
            tgt = a[addr+i+1]
            # Use full aether size for data lumen bounds — data nodes may store
            # valid runtime pointers (e.g. K_CURSOR stores the bump address itself)
            if rel >= _AETHER_SIZE: issues.append(f"Data@{addr}({sym.get(addr,'?')})+{i}: rel OOB")
            if tgt >= _AETHER_SIZE: issues.append(f"Data@{addr}({sym.get(addr,'?')})+{i}: tgt OOB")
            i += LUMEN_PAIR

    if issues:
        # Group by type for readability
        from collections import Counter
        type_counts = Counter()
        for iss in issues:
            if 'op=0' in iss:            type_counts['op=0 unwired'] += 1
            elif 'word\u2260self' in iss: type_counts['word≠self'] += 1
            elif 'OOB' in iss:           type_counts['OOB'] += 1
            else:                        type_counts['other'] += 1
        summary = ', '.join(f"{v} {k}" for k, v in sorted(type_counts.items()))
        print(f"Issues: {len(issues)}  ({summary})")
        unwired_count = type_counts.get('op=0 unwired', 0)
        if unwired_count:
            print(f"  NOTE: {unwired_count} op=0 'unwired' issues are a known pre-existing baseline.")
            print(f"        Root cause: LOAD_CU_BUILDER dispatch path is dead code (see BUGS.md).")
            print(f"        These do not affect runtime for the currently-exercised code paths.")
        for iss in issues:
            print(f"  {iss}")
    else:
        n_ito = len(ldr._ito_addrs); n_dat = len(ldr._data_addrs)-n_ito
        print(f"ITO checked: {n_ito}  \u2713")
        print(f"Data checked: {n_dat}  \u2713")
        print("No graph integrity issues.")


# ── strings ───────────────────────────────────────────────────────────────────

def mode_strings():
    print("=== String Encoding Layout ===\n")
    ldr, a, R, sym, errs = _load()
    print("Model: 8 bytes packed/lux (little-endian u64), linked list.")
    print("OB: 1 byte/lux BLOCK, SYS_WRITE_PACKED reads 8 bytes/lux.\n")
    for name in [k for k in R if k.startswith('SF_')][:12]:
        addr = R[name]; first = a[addr]
        if not first: print(f"  {name}: (empty)"); continue
        decoded = _read_packed_string(a, first)
        chunks = 0; cur = first
        while cur: chunks += 1; cur = a[cur+1]
        print(f"  {name}: {len(decoded)}ch {chunks}chunk  {decoded[:60]!r}")
    print()
    if 'OB_START' in R and 'OB_SIZE' in R:
        print(f"OB: BLOCK@{R['OB_START']}, {a[R['OB_SIZE']]} luces")


# ── macros ────────────────────────────────────────────────────────────────────

def mode_macros():
    """Check which Reca macro commands are defined vs actually invoked in .re files."""
    print("=== Macro Registry vs .re Usage ===\n")
    primitives = {
        'NEW', 'NEWREF', 'NEWSET', 'SETREF', 'ITO', 'RVOCA', 'RREDI', 'LINK',
        'NOLINK', 'BLOCK', 'ALLOC_TO', 'JEQ', 'JZ', 'SWITCH', 'FOR',
        'CLEAR', 'NOP', 'SAVE', 'NOITO', 'CHAIN', 'SET', 'FOR_ITER_ELEM',
        'NEXO', 'YAKU_NEXO', 'YAKU_NEXO_TERM', 'YAKU_NEXO_CMP',
        'YAKU_NEXO_ARITH', 'YAKU_NEXO_ALIAS', 'RCALL_AT',
        'LH', 'LR', 'LT', 'WALK_ITO', 'EMIT', 'EMITI', 'PUTBYTE',
    }
    ldr, a, R, sym, errs = _load()
    known_addrs = set(R.values())
    reca_macros = {name for name, addr in R.items()
                   if a[addr] != addr and a[addr] in known_addrs}
    all_commands = primitives | reca_macros
    used = set()
    for fp in _all_re_files():
        in_block = False
        for _, line in _read_re_file(fp):
            s = line.strip()
            if not s: continue
            if s.startswith('//'): in_block = not in_block; continue
            if in_block or s.startswith('/'): continue
            p = _parse_line(line)
            if p and p[0] in all_commands:
                used.add(p[0])
    unused = all_commands - used - {'FOR_ITER_ELEM'}
    if unused:
        print(f"Defined but never invoked ({len(unused)}): {sorted(unused)}")
    else:
        print("All defined commands are invoked  \u2713")
    print(f"Commands: {len(primitives)} native + {len(reca_macros)} Reca-defined = {len(all_commands)} total")

# ── parity ────────────────────────────────────────────────────────────────────

def mode_parity(target_path):
    print(f"=== Macro Parity: {os.path.basename(target_path)} ===\n")
    if not os.path.exists(target_path): print(f"File not found: {target_path}"); return
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    interp_a = Interpreter(); ldr_a = Loader(interp_a)
    ldr_a.load_file(os.path.join(_HERE, 'core', 'aspects.re'))
    for f in sorted(glob.glob(os.path.join(_HERE, '*.re'))):
        if os.path.abspath(f) != os.path.abspath(target_path) and 'aspects.re' not in f:
            ldr_a.load_file(f)
    sym_before = set(ldr_a.symbols.keys()); ldr_a.load_file(target_path)
    sys.stderr = old
    a_a = ldr_a.interp.aether.aether; R_a = ldr_a.symbols
    sym_a = {v: k for k, v in R_a.items()}
    target_instrs = {
        name: [a_a[addr+i] for i in range(ITO_SIZE)]
        for name, addr in R_a.items()
        if name not in sym_before and addr in ldr_a._ito_addrs_set
    }
    print(f"Python loader: {len(target_instrs)} ITO luces from target")
    slot_labels = ['word','op','e1','e2','exit','next','pad']
    printed = 0
    for name, slots in sorted(target_instrs.items()):
        slotstr = '  '.join(
            f"{slot_labels[i]}={sym_a.get(slots[i],slots[i])}"
            for i in range(ITO_SIZE) if slots[i]
        )
        print(f"  {name}: {slotstr}"); printed += 1
        if printed >= 40: print(f"  ... ({len(target_instrs)-printed} more)"); break


# ── lost symbols ──────────────────────────────────────────────────────────────

def mode_lost():
    """Find symbols defined in .re files but swallowed by comment blocks."""
    print("=== Lost Symbols (defined but swallowed by comment blocks) ===\n")
    cmd_re = _re.compile(
        r'^\s*(NEW|NEWREF|NEWSET|ITO|BLOCK|YAKU_NEXO|YAKU_NEXO_TERM|YAKU_NEXO_ALIAS)\s+(\w+)'
    )
    total_lost = 0
    for fp in _all_re_files():
        raw_names = set()
        for line in open(fp, encoding='utf-8', errors='replace'):
            m = cmd_re.match(line)
            if m: raw_names.add(m.group(2))
        loaded_names = set()
        for _, text in _read_re_file(fp):
            m = cmd_re.match(text)
            if m: loaded_names.add(m.group(2))
        lost = raw_names - loaded_names
        if lost:
            total_lost += len(lost)
            print(f"{os.path.basename(fp)}: {len(lost)} lost — {sorted(lost)}")
    print("\nTotal lost: " + (str(total_lost) if total_lost else "0  \u2713"))


# ── dependency order ──────────────────────────────────────────────────────────

def mode_deps():
    """Show SCC load order and per-file symbol counts."""
    print("=== .re File Dependency / Load Order ===\n")
    import interpreter as _interp_mod
    interp = _interp_mod.Interpreter(); ldr = Loader(interp)
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    ldr.load_file(os.path.join(_HERE, 'core', 'aspects.re'))
    sys.stderr = old

    seed_path = os.path.abspath(os.path.join(_HERE, 'core', 'aspects.re'))
    all_files = [
        os.path.abspath(p)
        for p in sorted(glob.glob(os.path.join(_HERE, '**', '*.re'), recursive=True))
        if os.path.abspath(p) != seed_path
    ]

    file_info = {}
    for fp in all_files:
        defs, refs, links = ldr._scan_file(fp)
        file_info[fp] = {'defs': defs, 'refs': refs - defs}

    name_to_file = {}
    for fp, info in file_info.items():
        for name in info['defs']:
            if name not in name_to_file: name_to_file[name] = fp

    dep_graph = {fp: set() for fp in all_files}
    for fp, info in file_info.items():
        for name in info['refs']:
            if name not in ldr.symbols:
                dep_fp = name_to_file.get(name)
                if dep_fp and dep_fp != fp: dep_graph[fp].add(dep_fp)

    sccs = ldr._tarjan_sccs(dep_graph)
    print(f"aspects.re  [seed]  {len(ldr.symbols)} symbols\n")
    for i, ring in enumerate(sccs, 1):
        names = [os.path.basename(f) for f in ring]
        defs  = sum(len(file_info[f]['defs']) for f in ring)
        tag   = "  \u2190 CYCLE" if len(ring) > 1 else ""
        print(f"SCC {i:3d}: {', '.join(names)}  ({defs} defs){tag}")
    print(f"\nTotal SCCs: {len(sccs)}, files: {len(all_files)}")


# ── indent synthesis log ──────────────────────────────────────────────────────

def mode_indent():
    """Show every line synthesised by the indent mechanism."""
    print("=== Indent Synthesis Log ===\n")
    print("Leaders:")
    for leader, (mode, idx) in sorted(_INDENT_LEADERS.items()):
        print(f"  {leader:22s} mode={mode}  name_idx={idx}")
    print()

    total = 0
    for fp in _all_re_files():
        fname = os.path.basename(fp)
        raw_lines = open(fp, encoding='utf-8', errors='replace').readlines()
        in_block = False; _indent_ctx = None; synth = []
        for lineno, raw in enumerate(raw_lines, 1):
            line = raw.rstrip('\n')
            is_indented = bool(line) and line[0] in (' ', '\t')
            out = []; in_str = False; i = 0
            while i < len(line):
                ch = line[i]
                if in_str:
                    out.append(ch)
                    if ch == '"': in_str = False
                    i += 1
                elif in_block:
                    if ch=='/' and i+1<len(line) and line[i+1]=='/':
                        in_block = False; break
                    else: i += 1
                else:
                    if ch == '"': in_str = True; out.append(ch); i += 1
                    elif ch=='/' and i+1<len(line) and line[i+1]=='/':
                        in_block = True; i += 2
                    else: out.append(ch); i += 1
            stripped = ''.join(out).strip()
            if not stripped: _indent_ctx = None; continue
            if is_indented and _indent_ctx is not None:
                mode, name = _indent_ctx
                if mode == 'nexo':
                    result = f'NEXO {name} {stripped}'
                else:
                    parts = stripped.split(None, 1); cmd = parts[0]
                    rest = parts[1] if len(parts)>1 else ''
                    result = f'{cmd} {name} {rest}'.rstrip()
                synth.append((lineno, stripped, result)); continue
            parts = stripped.split(); first = parts[0].upper() if parts else ''
            entry = _INDENT_LEADERS.get(first)
            if entry is not None:
                mode2, name_idx = entry
                _indent_ctx = (mode2, parts[name_idx]) if len(parts)>name_idx else None
            else:
                _indent_ctx = None

        if synth:
            total += len(synth)
            print(f"{fname} ({len(synth)}):")
            for ln, orig, result in synth[:8]:
                print(f"  {ln:4d}  {orig[:32]:32s} \u2192 {result[:50]}")
            if len(synth)>8: print(f"  ... and {len(synth)-8} more")
            print()
    print(f"Total synthesised: {total}")


# ── entry point ───────────────────────────────────────────────────────────────


# ─── LOAD_MAIN mode ──────────────────────────────────────────────────────────

def _setup_load_main(ldr, cursor_start=700_000, zero_alloc=True):
    from symphony import AETHER_SIZE
    a = ldr.interp.aether.aether; R = ldr.symbols
    for fn in ('RA_BS_FLAG','LD_FLAG','RA_JEQ_FLAG','RA_FLAG','RA_MC_FLAG'):
        fa = R.get(fn, 0)
        if fa: a[fa] = fa
    for bn in ('LD_LCOUNT_BUF_000','LD_BACKFILL_BUF_000','LD_BODY_BUF_000'):
        ba = R.get(bn, 0)
        if ba: a[ba] = ba
    K_C = R['K_CURSOR']; K_WM = R.get('K_WATERMARK', 0)
    a[K_C] = K_C + 1; a[K_C + 1] = cursor_start
    if K_WM: a[K_WM] = K_WM + 1; a[K_WM + 1] = 0
    if zero_alloc:
        for i in range(cursor_start, min(AETHER_SIZE, cursor_start + 200_000)):
            a[i] = 0


def mode_load_main(max_steps=0, watch_lux=0):
    """Run LOAD_MAIN and report stats."""
    from symphony import SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT
    import time
    ldr, a, R, sym, errs = _load()
    _setup_load_main(ldr)
    # Register __LT_ALLOC_ITO as native op (allocates ITO at K_CURSOR)
    lt_addr = R.get("__LT_ALLOC_ITO", 0)
    ra_ma_ret = R.get("RA_MA_RET", 0)
    K_C_ref = R["K_CURSOR"]
    if lt_addr:
        def _lt_alloc(a1, a2, ex, nxt, aether):
            from loader import _ITO_SIZE, _WORD
            ptr = aether[K_C_ref + 1]
            aether[K_C_ref + 1] = ptr + _ITO_SIZE
            aether[ptr + _WORD] = ptr  # self-ref
            if ra_ma_ret: aether[ra_ma_ret] = ptr
            return nxt
        ldr.interp._dispatch[lt_addr] = _lt_alloc
    track = {
        'LOAD_MAIN_INC': 'files', 'LOAD_MAIN_BF2': 'done',
        'LOAD_CMD_ITO': 'ito', 'LOAD_CMD_LINK_IMPL': 'link',
        'LOAD_CU_BUILDER': 'builder', 'LOAD_CU_CALL': 'native', 'LOAD_CU_SKIP': 'skip',
    }
    counts  = {v: 0 for v in track.values()}
    tracked = {R.get(k, 0): v for k, v in track.items()}; tracked.pop(0, None)
    Write_op = R.get('Write', 0); K_C = R['K_CURSOR']
    interp = ldr.interp; dispatch = interp._dispatch
    pc = R['LOAD_MAIN']; history = []; crash = None; t0 = time.time(); steps = 0
    while pc:
        if pc in tracked: counts[tracked[pc]] += 1
        steps += 1; history.append(pc); history = history[-8:]
        if max_steps and steps > max_steps: crash = f'step limit {max_steps:,}'; break
        if watch_lux:
            op = a[pc + SLOT_OP]; e1 = a[pc + SLOT_E1]; e2 = a[pc + SLOT_E2]
            if op == Write_op and e1 and a[e1] == watch_lux:
                val = a[e2] if e2 else 0
                print(f'  [WATCH] {sym.get(pc, pc)} → lux {watch_lux} = {val} ({sym.get(val, val)})')
        op = a[pc+SLOT_OP]; e1 = a[pc+SLOT_E1]; e2 = a[pc+SLOT_E2]
        ex = a[pc+SLOT_EXIT]; nx = a[pc+SLOT_NEXT]
        h = dispatch.get(op)
        if not h: crash = f'unknown op={op} at {sym.get(pc, pc)}'; break
        pc = h(e1, e2, ex, nx, a)
    alloc = a[K_C + 1] - 700_000
    ht_base = a[R.get('BS_HT_BASE', 0)] if R.get('BS_HT_BASE') else 0
    ht_sz   = int(a[R.get('BS_HT_SIZE', 0)]) if R.get('BS_HT_SIZE') else 0
    ht_fill = sum(1 for i in range(ht_sz) if ht_base and a[ht_base + i] != 0)
    print(f"LOAD_MAIN  {'✗ ' + crash if crash else '✓ DONE'}")
    print(f"  Steps  : {steps:,}  ({time.time()-t0:.1f}s)")
    total = int(a[R.get('BS_FILE_COUNT', 0)]) if R.get('BS_FILE_COUNT') else '?'
    print(f"  Files  : {counts['files']} / {total}  (sweep-done={counts['done']})")
    print(f"  Cmds   : ITO={counts['ito']}  LINK={counts['link']}  builder={counts['builder']}  native={counts['native']}  skip={counts['skip']}")
    print(f"  Alloc  : {alloc} luces  |  HT: {ht_fill} entries")
    print(f"  Last   : {' → '.join(sym.get(p, str(p)) for p in history[-5:])}")

def mode_lint():
    """Static lint: find phantom ITOs, missing labels, op=0 ITOs reachable from code.

    Checks:
    1. All ITO addrs with op=0 — phantom labels (EXIT/NEXT points to unresolved name).
    2. All EXIT / NEXT references in live ITOs that point to op=0 targets.
    3. Self-ref violations: aether[addr] != addr for known ITO addrs.
    """
    print("=== Reca Static Lint ===\n")
    ldr, a, R, sym, errs = _load()

    ito_addrs  = ldr._ito_addrs   # list of ITO base addresses
    ito_set    = set(ito_addrs)
    bump       = ldr._bump

    # ── 1. Phantom ITOs: op=0 ────────────────────────────────────────────────
    phantom = [addr for addr in ito_addrs if a[addr + SLOT_OP] == 0]
    if phantom:
        print(f"PHANTOM ITOs (op=0) — {len(phantom)} found:")
        for addr in sorted(phantom)[:40]:
            name = sym.get(addr, f'#{addr}')
            # Try to show what might reference this:
            print(f"  {name} @ {addr}")
        if len(phantom) > 40:
            print(f"  ... and {len(phantom)-40} more")
    else:
        print("Phantom ITOs: none ✓")
    print()

    # ── 2. Dead EXIT/NEXT references ─────────────────────────────────────────
    dead_refs = []
    for addr in ito_addrs:
        if a[addr + SLOT_OP] == 0:
            continue  # already phantom itself
        for slot, slot_name in [(SLOT_EXIT, 'EXIT'), (SLOT_NEXT, 'NEXT')]:
            target = a[addr + slot]
            if target == 0:
                continue  # 0 = fall-through or no exit, valid
            if target < bump and target not in ito_set and target not in {v for v in R.values()}:
                continue  # not an ITO addr, could be a data lux — skip
            if target in ito_set and a[target + SLOT_OP] == 0:
                src_name = sym.get(addr, f'#{addr}')
                tgt_name = sym.get(target, f'#{target}')
                dead_refs.append((src_name, slot_name, tgt_name, addr, target))

    if dead_refs:
        print(f"DEAD REFERENCES (→ phantom ITO) — {len(dead_refs)} found:")
        for src, slot, tgt, src_addr, tgt_addr in sorted(dead_refs, key=lambda x: x[0])[:40]:
            print(f"  {src}.{slot} → {tgt} (op=0)")
        if len(dead_refs) > 40:
            print(f"  ... and {len(dead_refs)-40} more")
    else:
        print("Dead references: none ✓")
    print()

    # ── 3. Self-ref violations ───────────────────────────────────────────────
    # NEWREF aliases intentionally have word != addr (word = addr of the target).
    # Only flag luces whose word doesn't point to any known symbol address.
    known_addrs = set(R.values())  # R = {name: addr}
    bad_selfref = [
        addr for addr in ito_addrs
        if a[addr] != addr            # not self-ref
        and a[addr] not in known_addrs  # word doesn't point to a known symbol
        and a[addr] != 0
    ]
    if bad_selfref:
        print(f"SELF-REF VIOLATIONS — {len(bad_selfref)} found:")
        for addr in sorted(bad_selfref)[:20]:
            name = sym.get(addr, f'#{addr}')
            print(f"  {name} @ {addr}: word={a[addr]} (expected {addr})")
        if len(bad_selfref) > 20:
            print(f"  ... and {len(bad_selfref)-20} more")
    else:
        print("Self-ref: all ITO addrs ✓")
    print()

    # ── Summary ───────────────────────────────────────────────────────────────
    total_issues = len(phantom) + len(dead_refs) + len(bad_selfref)
    if total_issues == 0:
        print("✓ Lint clean — no issues found.")
    else:
        print(f"✗ {total_issues} total issues ({len(phantom)} phantom, "
              f"{len(dead_refs)} dead refs, {len(bad_selfref)} self-ref violations)")
    return ldr, a, R, sym, errs, phantom, dead_refs


if __name__ == '__main__':
    els = sys.argv[1:]

    if '--lint' in els:
        mode_lint()
    elif '--load-main' in els:
        steps = int(els[els.index('--steps')+1]) if '--steps' in els else 0
        watch = int(els[els.index('--watch')+1]) if '--watch' in els else 0
        mode_load_main(max_steps=steps, watch_lux=watch)
    elif '--trace' in els:
        import trace as _trace
        _trace.run([a for a in els if a != '--trace'])
    elif '--lost' in els:
        mode_lost()
    elif '--deps' in els:
        mode_deps()
    elif '--indent' in els:
        mode_indent()
    elif '--strings' in els:
        mode_strings()
    elif '--graph' in els:
        mode_graph()
    elif '--macros' in els:
        mode_macros()
    elif '--parity' in els:
        idx = els.index('--parity')
        mode_parity(els[idx+1] if idx+1<len(els) else '')
    elif '--health' in els:
        mode_health()
    else:
        ldr, a, R, sym, errs = mode_health()
        print(); mode_graph()
