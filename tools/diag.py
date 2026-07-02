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
    python3 diag.py --block-lint           # indent/style outliers in CHAIN/SWITCH/FOR/WAVE bodies
    python3 diag.py --unwrapped            # uncommented decorative lines, flagged by risk (inside vs outside a block body)
    python3 diag.py --invariants           # comprehensive post-freeze invariant check (ASCII, C_N, ITO wiring, macros, self-hosting)
"""
import sys, os, glob, io, re as _re

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'symphony.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from symphony import SLOT_WORD, SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, ITO_SIZE, LUMEN_PAIR
from loader import (freeze, Loader, _read_re_file, _parse_line,
                    _INDENT_LEADERS, _strip_comments, discover_newref_macro_names)
from interpreter import Interpreter

_load_cache = None
_load_fast_cache = None

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


class _LightLoader:
    """Duck-typed stand-in for Loader exposing only .interp/.symbols.

    Several diag.py modes (strings, macros, load-main, wiring, htable,
    invariants) never touch Loader's build-time bookkeeping (_bump,
    _ito_addrs, _data_addrs, ...) -- only the frozen interp+symbols. Those
    modes used to call _load(), paying freeze()'s full Python-Wave-A/B-plus-
    LOAD_MAIN cost on every single CLI invocation even when nothing on disk
    had changed since the last run. _load_fast() below reuses load_or_freeze()
    (now staleness-aware, see loader._bin_is_stale) so repeated invocations
    in the same session -- e.g. running --invariants after every edit, as
    TOOLING.md recommends -- thaw the cached reca.bin instead of rebuilding
    it, while still rebuilding correctly the moment a .re file changes.
    """
    __slots__ = ('interp', 'symbols')
    def __init__(self, interp, symbols):
        self.interp = interp
        self.symbols = symbols


def _load_fast():
    """Like _load(), but for modes that only need interp+symbols.

    Uses load_or_freeze() (thaw reca.bin when fresh, freeze() when stale)
    instead of unconditionally calling freeze(). Returns the same 5-tuple
    shape as _load() so existing `ldr, a, R, sym, errs = _load_fast()`
    call sites don't need to change beyond the function name -- ldr here
    is a _LightLoader, not a full Loader, so it only supports .interp/.symbols.
    """
    global _load_fast_cache
    if _load_fast_cache is not None:
        return _load_fast_cache
    from loader import load_or_freeze
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    try:
        interp, symbols = load_or_freeze()
    finally:
        sys.stderr = old
    errs = [l for l in buf.getvalue().splitlines() if 'Pass2' in l or 'Error' in l]
    a = interp.aether.aether; R = symbols
    sym = {v: k for k, v in R.items()}
    ldr = _LightLoader(interp, symbols)
    _load_fast_cache = (ldr, a, R, sym, errs)
    return _load_fast_cache


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
    return sorted(glob.glob(os.path.join(_ROOT, '**', '*.re'), recursive=True))


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
    ldr, a, R, sym, errs = _load_fast()
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
    # Commands expected to be used directly as a line-starting keyword
    # somewhere (an intentional, curated distinction -- not an attempt to
    # enumerate every macro, which is what discover_newref_macro_names()
    # below is for).
    primitives = {
        'NEW', 'NEWREF', 'NEWSET', 'SETREF', 'ITO', 'RVOCA', 'RREDI', 'LINK',
        'NOLINK', 'BLOCK', 'ALLOC_TO', 'JEQ', 'JZ', 'SWITCH', 'FOR',
        'CLEAR', 'NOP', 'SET',
        # CHAIN/NOITO/SAVE: genuinely used directly elsewhere, but they're
        # Python-preprocessor constructs (see loader.py's _read_re_file)
        # fully expanded/consumed before a literal "CHAIN"/"NOITO"/"SAVE"
        # token would ever reach this scan -- so these three will *always*
        # show as "never seen as p[0]" below, regardless of real usage.
        # Not the same as a genuine helper-only macro; kept in primitives
        # since they ARE meant to be used directly, just undetectable by
        # this particular method.
        'CHAIN', 'NOITO', 'SAVE',
        'YAKU_NEXO', 'YAKU_NEXO_TERM', 'YAKU_NEXO_CMP',
        'YAKU_NEXO_ARITH', 'YAKU_NEXO_ALIAS', 'RCALL_AT',
        'LH', 'LR', 'LT', 'WALK_ONE', 'EMIT', 'EMITI',
        'PUTBYTE', 'LINK_OP', 'UNLINK_OP',
    }
    ldr, a, R, sym, errs = _load_fast()
    # Every NEWREF-declared macro -- accurate by construction (scans
    # macros.re's own declarations), unlike the old "a[addr] points at
    # another known symbol" heuristic, which mostly matched ordinary data
    # buffers (BS_READBUF_*, SK_PLINK_BUF_*, and hundreds more) that happen
    # to alias another address for unrelated reasons, not real macros.
    all_macros = discover_newref_macro_names()
    all_commands = primitives | all_macros
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
    # Most NEWREF macros are legitimately helper-only (only ever called as
    # an RVOCA "sub" target, never as a direct line-starting command) -- so
    # "never invoked as p[0]" is informational, not inherently a problem.
    # Only commands in `primitives` (expected to be used directly) being
    # unused is worth flagging as suspicious.
    never_as_command = all_commands - used
    suspicious = never_as_command & primitives
    helper_only = never_as_command - primitives
    if suspicious:
        print(f"In `primitives` (expected as a direct command) but never seen as one "
              f"({len(suspicious)}): {sorted(suspicious)}")
    else:
        print("Every primitive expected as a direct command is used as one  \u2713")
    print(f"Helper-only macros (never used as p[0], only as an RVOCA/etc. "
          f"target -- expected, not a problem) ({len(helper_only)})")
    print(f"Commands: {len(primitives)} primitives + {len(all_macros)} "
          f"NEWREF-defined = {len(all_commands)} total")

# ── parity ────────────────────────────────────────────────────────────────────

def mode_parity(target_path):
    print(f"=== Macro Parity: {os.path.basename(target_path)} ===\n")
    if not os.path.exists(target_path): print(f"File not found: {target_path}"); return
    buf = io.StringIO(); old = sys.stderr; sys.stderr = buf
    interp_a = Interpreter(); ldr_a = Loader(interp_a)
    ldr_a.load_file(os.path.join(_ROOT, 'core', 'aspects.re'))
    for f in sorted(glob.glob(os.path.join(_ROOT, '*.re'))):
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
    ldr.load_file(os.path.join(_ROOT, 'core', 'aspects.re'))
    sys.stderr = old

    seed_path = os.path.abspath(os.path.join(_ROOT, 'core', 'aspects.re'))
    all_files = [
        os.path.abspath(p)
        for p in sorted(glob.glob(os.path.join(_ROOT, '**', '*.re'), recursive=True))
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
    ldr, a, R, sym, errs = _load_fast()
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

    Pattern-R phantoms (op=0 because their op-slot is filled only by runtime
    Pattern-R ops like LR/LT/EMIT/LH/WALK_ONE -- never by Python Wave-B) are
    counted but not listed as errors. They are architecturally expected.
    Dead refs sourced from auto-generated JumpIf labels (_J/_K suffix) pointing
    to Pattern-R phantom targets are also expected and suppressed.
    """
    print("=== Reca Static Lint ===\n")
    ldr, a, R, sym, errs = _load()

    ito_addrs  = ldr._ito_addrs   # list of ITO base addresses
    ito_set    = set(ito_addrs)
    bump       = ldr._bump

    # ── 1. Phantom ITOs: op=0 ────────────────────────────────────────────────
    phantom = [addr for addr in ito_addrs if a[addr + SLOT_OP] == 0]
    # Partition into Pattern-R (expected) vs real (unexpected).
    # Pattern-R: an ITO whose op-slot is only ever filled by runtime Pattern-R
    # ops (LR, LT, EMIT, LH, WALK_ONE, ...). We identify them heuristically:
    # a phantom is Pattern-R if NO live ITO directly references it in an
    # EXIT/NEXT slot -- they're only reached via Pattern-R-built jump tables.
    # Phantoms that ARE referenced by live ITOs (dead refs) but whose only
    # referencing sources are auto-generated _J/_K JumpIf labels (the
    # fall-through targets of JEQ/SWITCH expansions) pointing to a Pattern-R
    # target are also suppressed -- these are architecturally expected.
    phantom_set = set(phantom)
    # For each phantom, collect all live-code sources that reference it
    phantom_sources: dict = {addr: [] for addr in phantom}
    for addr in ito_addrs:
        if a[addr + SLOT_OP] == 0: continue
        src_name = sym.get(addr, '')
        for slot in [SLOT_EXIT, SLOT_NEXT]:
            tgt = int(a[addr + slot])
            if tgt in phantom_set:
                phantom_sources[tgt].append((addr, src_name))

    def _is_autogenerated_jump_label(name: str) -> bool:
        """True for auto-generated JumpIf/SWITCH labels (_J/_K suffix, __sw_N_J)."""
        return (name.endswith('_J') or name.endswith('_K') or
                name.startswith('__sw_'))

    # Known Pattern-R subsystem name prefixes: live ITOs from these subsystems
    # reference Pattern-R phantoms as part of normal, expected control flow.
    # These subsystems are built at runtime by Pattern-R ops (LR/LT/EMIT/LH/etc.)
    # rather than being wired by Python Wave-B.
    _PATTERN_R_SRC_PREFIXES = (
        'EB_', 'EJR_', 'EF_', 'EM_', 'ETH_', 'EXH_',
        'PA_', 'PGB_', 'RTB_', 'ND_', 'BFS_', 'FAL_',
        'CSP_', 'INIT_', 'RING_', 'RP_', 'RI_',
        'P0_', 'PS_', 'PFX_', 'LIM_', 'EMIT_',
    )

    def _is_pattern_r_source(name: str) -> bool:
        return (_is_autogenerated_jump_label(name) or
                any(name.startswith(p) for p in _PATTERN_R_SRC_PREFIXES))

    # Iterative propagation: a phantom is Pattern-R if ALL its live sources are
    # either (a) auto-generated jump labels / Pattern-R subsystem sources, or
    # (b) already-identified Pattern-R phantoms. Repeat until stable.
    pattern_r_phantom: set = set()
    while True:
        prev_size = len(pattern_r_phantom)
        for addr in phantom:
            if addr in pattern_r_phantom:
                continue
            srcs = phantom_sources[addr]
            if not srcs:
                pattern_r_phantom.add(addr)  # unreachable = Pattern-R
                continue
            if all(
                _is_pattern_r_source(name) or src_addr in pattern_r_phantom
                for src_addr, name in srcs
            ):
                pattern_r_phantom.add(addr)
        if len(pattern_r_phantom) == prev_size:
            break  # stable
    real_phantom = phantom_set - pattern_r_phantom

    if phantom:
        n_r = len(pattern_r_phantom)
        n_real = len(real_phantom)
        if n_real:
            print(f"PHANTOM ITOs (op=0) — {n_real} real + {n_r} Pattern-R (expected, not shown):")
            for addr in sorted(real_phantom)[:40]:
                name = sym.get(addr, f'#{addr}')
                print(f"  {name} @ {addr}")
            if n_real > 40:
                print(f"  ... and {n_real-40} more")
        else:
            print(f"Phantom ITOs: none unexpected ✓  ({n_r} Pattern-R expected, suppressed)")
    else:
        print("Phantom ITOs: none ✓")
    print()

    # ── 2. Dead EXIT/NEXT references ─────────────────────────────────────────
    dead_refs = []
    pattern_r_dead = []
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
                # Pattern-R dead ref: the target is a Pattern-R phantom
                # (will be built at runtime by Pattern-R ops like LR/LT/EMIT).
                # The source label's naming doesn't matter -- what matters is
                # that the target's op=0 is architecturally expected.
                is_pattern_r = target in pattern_r_phantom
                if is_pattern_r:
                    pattern_r_dead.append((src_name, slot_name, tgt_name, addr, target))
                else:
                    dead_refs.append((src_name, slot_name, tgt_name, addr, target))

    if dead_refs:
        n_pr = len(pattern_r_dead)
        print(f"DEAD REFERENCES (→ phantom ITO) — {len(dead_refs)} real"
              + (f" + {n_pr} Pattern-R (suppressed)" if n_pr else "") + ":")
        for src, slot, tgt, src_addr, tgt_addr in sorted(dead_refs, key=lambda x: x[0])[:40]:
            print(f"  {src}.{slot} → {tgt} (op=0)")
        if len(dead_refs) > 40:
            print(f"  ... and {len(dead_refs)-40} more")
    elif pattern_r_dead:
        print(f"Dead references: none unexpected ✓  ({len(pattern_r_dead)} Pattern-R expected, suppressed)")
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
    total_real = len(real_phantom) + len(dead_refs) + len(bad_selfref)
    total_pr   = len(pattern_r_phantom) + len(pattern_r_dead)
    if total_real == 0:
        if total_pr:
            print(f"✓ Lint clean  ({total_pr} Pattern-R phantoms/dead-refs suppressed as expected)")
        else:
            print("✓ Lint clean — no issues found.")
    else:
        msg = (f"✗ {total_real} issues ({len(real_phantom)} phantom, "
               f"{len(dead_refs)} dead refs, {len(bad_selfref)} self-ref violations)")
        if total_pr:
            msg += f"  +{total_pr} Pattern-R expected (suppressed)"
        print(msg)
    return ldr, a, R, sym, errs, phantom, dead_refs


def _mode_wiring(sym_names):
    """Print lux wiring for named symbols. Replaces dump_wiring.py."""
    from symphony import SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT
    ldr, a, R, sym, errs = _load_fast()
    if not sym_names:
        print("Usage: python3 diag.py --wiring SYM1 [SYM2 ...]")
        return
    for sname in sym_names:
        addr = R.get(sname, 0)
        if not addr:
            print(f"  {sname}: NOT FOUND in symbols"); continue
        word = int(a[addr]); op = int(a[addr+SLOT_OP])
        e1v = int(a[addr+SLOT_E1]); e2v = int(a[addr+SLOT_E2])
        exv = int(a[addr+SLOT_EXIT]); nxv = int(a[addr+SLOT_NEXT])
        self_ok = "✓" if word == addr else "✗ BAD(word≠addr)"
        opn = sym.get(op, f'#{op}'); e1n = sym.get(e1v, e1v)
        e2n = sym.get(e2v, e2v); exn = sym.get(exv, exv)
        nxn = '0(implicit)' if nxv == 0 else sym.get(nxv, nxv)
        print(f"  {sname:30s}  @{addr}  word={word} {self_ok}")
        print(f"    op={opn}  e1={e1n}  e2={e2n}")
        print(f"    exit={exn}  next={nxn}")


def _mode_broken():
    """Check for broken (next=0) luces in key infrastructure. From diag_loadmain.py."""
    from symphony import SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT
    ldr, a, R, sym, errs = _load()
    prims = ['Equal','Read','Write','Add','Sub','Mul','Move','JumpIf','Jump',
             'Voca','Redi','Less','ULess','End','Exire']
    print("Primitive self-ref check:")
    for n in prims:
        addr = R.get(n, 0)
        if not addr: continue
        word = int(a[addr])
        ok = "✓" if word == addr else f"✗ word={word} expected {addr}"
        print(f"  {n:12s} @{addr}: {ok}")
    print("\nNext=0 check on key LOAD_MAIN symbols:")
    suspects = [k for k in R if k.startswith('LOAD_') or k.startswith('BS_') or k.startswith('SK_')]
    broken = []
    for sname in suspects:
        addr = R[sname]
        nxt = int(a[addr + SLOT_NEXT])
        op  = int(a[addr + SLOT_OP])
        # A lux that dispatches (op!=0) but has next=0 could be broken IF it's a
        # sequential instruction (not a jump). Heuristic: flag op=0 (unset) + ITO in ito_addrs
        if addr in ldr._ito_addrs_set and op == 0:
            broken.append((sname, addr))
    if broken:
        print(f"  Found {len(broken)} ITO luces with op=0 (unset):")
        for sname, addr in broken[:20]:
            print(f"    {sname} @{addr}")
    else:
        print("  None found ✓")


def _mode_htable(name_to_find):
    """Look up a name in the runtime hash table. From diag_loadmain.py --htable."""
    if not name_to_find:
        print("Usage: python3 diag.py --htable NAME"); return
    ldr, a, R, sym, errs = _load_fast()
    # Use the same BS_LOOKUP machinery: hash the name, find in HT
    BS_HT_BASE = int(a[R['BS_HT_BASE']]) if R.get('BS_HT_BASE') else 0
    BS_HT_SIZE = int(a[R['BS_HT_SIZE']]) if R.get('BS_HT_SIZE') else 0
    if not BS_HT_BASE or not BS_HT_SIZE:
        print("HT registers not found"); return
    # DJB2 hash
    h = 5381
    for ch in name_to_find.encode('utf-8'):
        h = ((h * 33) ^ ch) & 0xFFFFFFFFFFFFFFFF
    slot = h % BS_HT_SIZE
    print(f"  Name: {name_to_find!r}")
    print(f"  DJB2 hash: {h}  slot: {slot}")
    # Walk the chain
    entry = int(a[BS_HT_BASE + slot])
    if not entry:
        print(f"  HT[{slot}] = 0 (empty) — name not in runtime htable")
        return
    print(f"  HT[{slot}] = {entry} ({sym.get(entry, '?')})")
    # Try to read the packed name at that lux
    name_word = int(a[entry])
    print(f"  word = {name_word} ({sym.get(name_word, '?')})")
    if R.get(name_to_find):
        addr = R[name_to_find]
        found = int(a[entry]) == addr or name_word == addr
        print(f"  Expected addr: {addr}  Match: {'✓' if found else '✗'}")


# ─────────────────────────────────────────────────────────────────────────────
# mode_block_lint — indentation-block (CHAIN/SWITCH/FOR/WAVE/...) consistency
# ─────────────────────────────────────────────────────────────────────────────
#
# Generalizes a real bug found+fixed in comments.re: an indentation-aware
# block (CHAIN, SWITCH, FOR, WAVE, ...) had 4 sibling body lines at one
# indent depth and a 5th line at a *deeper* indent with a stray "ITO "
# prefix the other siblings didn't have. The deeper indent silently changed
# how the line is parsed/dispatched (LOAD_READ_BODY tags each body line with
# its raw indent depth as a marker byte; the self-hosted CHAIN builder
# treats different marker depths differently), and the resulting lux was
# never correctly wired -- a freshly-allocated, never-dispatched lux,
# exactly the bug class documented repeatedly in BUGS.md.
#
# This does not look for that exact text pattern. It looks for the
# *general* shape: within one block, an indent depth that's a singleton
# (appears on only one line) while siblings cluster around 1-2 other
# depths, or a line whose first token is a primitive/builtin keyword
# (ITO, JZ, JEQ, RVOCA, CLEAR, NOP, RREDI, WAVE, EMIT, EMITI, WRITE_OUT,
# ALLOC_TO) while its same-indent siblings are bare "NAME op args" lines
# (CHAIN/SWITCH/FOR's own implicit-ITO sugar). Either signal alone can be
# a false positive (genuine multi-line continuations exist, e.g. RVOCA/EMIT
# chains in yaku.re's ETH_* handlers are legitimate deeper-indent
# continuations) -- this is a *finder*, not an authority; every hit needs a
# human read of the surrounding block before deciding it's a real bug.
#
# Usage:
#   python3 diag.py --block-lint                # scan every .re file
#   python3 diag.py --block-lint foo.re bar.re   # scan specific files

from loader import _SPECIAL_OPENERS as _BLOCK_OPENERS

_BLOCK_PRIMITIVE_FIRST_TOKENS = frozenset({
    'ITO', 'JZ', 'JEQ', 'RVOCA', 'RREDI', 'CLEAR', 'NOP', 'WAVE',
    'EMIT', 'EMITI', 'WRITE_OUT', 'ALLOC_TO', 'LINK_OP', 'UNLINK_OP',
    'PUTBYTE', 'RCALL_AT', 'WALK_ITO', 'WALK_ONE', 'LINK', 'LH', 'LT',
    'LX', 'LR', 'NOLINK',
})


def _indent_width(raw_line: str) -> int:
    n = 0
    for ch in raw_line:
        if ch == ' ':
            n += 1
        elif ch == '\t':
            n += 4  # treat a tab as 4 columns for comparison purposes
        else:
            break
    return n


def _scan_block_lint_file(filepath: str):
    """Return a list of (block_start_lineno, opener, finding_str) for one file."""
    findings = []
    try:
        with open(filepath, encoding='utf-8', errors='replace') as f:
            raw_lines = f.readlines()
    except OSError:
        return findings
    stripped = _strip_comments(raw_lines)  # [(lineno, raw_line, is_indented, text), ...]

    i = 0
    n = len(stripped)
    while i < n:
        lineno, raw, is_indented, text = stripped[i]
        first_tok = text.split()[0] if text.strip() else ''
        if first_tok in _BLOCK_OPENERS:
            opener_lineno = lineno
            opener_tok = first_tok
            body = []  # (lineno, indent_width, first_token, text)
            j = i + 1
            while j < n:
                bln, braw, bind, btext = stripped[j]
                if not btext.strip():
                    break  # blank/comment-only line ends the body (LOAD_RB stops at non-indented/EOF)
                if not bind:
                    break  # first non-indented line ends the body
                btok = btext.split()[0] if btext.strip() else ''
                body.append((bln, _indent_width(braw), btok, btext.rstrip()))
                j += 1
            if len(body) >= 2:
                depths = [d for (_, d, _, _) in body]
                depth_counts: dict = {}
                for d in depths:
                    depth_counts[d] = depth_counts.get(d, 0) + 1
                # Signal A: singleton indent depth among >=3 body lines
                if len(body) >= 3:
                    for (bln, d, btok, btext) in body:
                        if depth_counts[d] == 1 and len(depth_counts) >= 2:
                            findings.append((
                                opener_lineno, opener_tok,
                                f"line {bln}: indent={d} is a singleton in this "
                                f"block (siblings use {sorted(set(depths) - {d})}) "
                                f"-- {btext.strip()[:70]!r}"
                            ))
                # Signal B: mixed primitive-keyword style at the modal (most common) depth
                modal_depth = max(depth_counts, key=lambda k: depth_counts[k])
                modal_lines = [(bln, btok, btext) for (bln, d, btok, btext) in body if d == modal_depth]
                if len(modal_lines) >= 2:
                    primitive_count = sum(1 for (_, t, _) in modal_lines if t in _BLOCK_PRIMITIVE_FIRST_TOKENS)
                    if 0 < primitive_count < len(modal_lines):
                        for (bln, btok, btext) in modal_lines:
                            if btok in _BLOCK_PRIMITIVE_FIRST_TOKENS:
                                findings.append((
                                    opener_lineno, opener_tok,
                                    f"line {bln}: starts with primitive keyword {btok!r} "
                                    f"but {len(modal_lines) - primitive_count} same-indent "
                                    f"sibling(s) use bare 'NAME op args' style "
                                    f"-- {btext.strip()[:70]!r}"
                                ))
            i = j
            continue
        i += 1
    return findings


def mode_block_lint(target_files=None):
    """Scan CHAIN/SWITCH/FOR/WAVE/... body blocks for indent/style outliers.

    Heuristic finder, not a hard pass/fail check -- every hit needs a human
    read of the block before treating it as a bug. See module comment above
    for the bug pattern this generalizes (comments.re stray-ITO, found and
    fixed this session).
    """
    if target_files:
        files = [f if os.path.isabs(f) else os.path.join(_ROOT, f) for f in target_files]
    else:
        files = sorted(glob.glob(os.path.join(_ROOT, '**', '*.re'), recursive=True))

    print("=== Block-lint: indentation/style outliers in CHAIN/SWITCH/FOR/WAVE bodies ===\n")
    total = 0
    for fp in files:
        findings = _scan_block_lint_file(fp)
        if not findings:
            continue
        rel = os.path.relpath(fp, _ROOT)
        print(f"--- {rel} ---")
        for opener_lineno, opener_tok, msg in findings:
            print(f"  [{opener_tok} @ line {opener_lineno}]  {msg}")
            total += 1
        print()
    if total == 0:
        print("No outliers found.")
    else:
        print(f"{total} potential outlier(s) found across {len(files)} files. "
              f"False positives are expected (legitimate multi-line "
              f"continuations, e.g. RVOCA/EMIT chains, look similar) -- "
              f"read each block before treating a hit as a bug.")


# ─────────────────────────────────────────────────────────────────────────────
# mode_unwrapped — uncommented decorative lines, classified by risk
# ─────────────────────────────────────────────────────────────────────────────
#
# Finds every line that isn't a comment and isn't a recognized command (its
# first token doesn't match any known primitive or NEWREF-defined macro
# name -- the SAME "is this a real command" test saku.re's own
# LOAD_CMD_UNKNOWN does at runtime). Classifies each as:
#
#   OUTSIDE a block -> safe. The top-level self-hosted dispatcher already
#   silently skips any line whose first word doesn't resolve to a known
#   command (LOAD_CMD_UNKNOWN: "word==0: skip rest of line" in saku.re) --
#   this mirrors loader.py's Python-side fallthrough (no matching `if
#   first == ...` branch -> line produces nothing). No fix needed; this is
#   working as designed, same as a "── Section ──" header between top-level
#   ITO/NEW lines.
#
#   INSIDE an indentation-aware block (SWITCH/CHAIN/FOR/NEXO/SAVE) -> risky.
#   Block bodies are read by a SEPARATE mechanism that doesn't always route
#   through the top-level dispatcher's unknown-word fallback (this was
#   exactly the fmt.re/SWITCH bug fixed this session: a "── divider ──"
#   line landing right after a SWITCH case list, misread as another case
#   instead of ending the block). Every hit here needs a human read --
#   it's flagging "this line sits where a block-body reader will see it",
#   not asserting every hit is still broken (SWITCH/NEXO/FOR/SAVE/CHAIN are
#   all now verified to correctly stop at the first dedented line).
#
# Usage:
#   python3 diag.py --unwrapped                # scan every .re file
#   python3 diag.py --unwrapped foo.re bar.re   # scan specific files

def _scan_unwrapped_file(filepath: str):
    """Return [(lineno, in_block, opener_tok, text), ...] for one file.

    Flags lines containing non-ASCII bytes (the actual failure mode: the
    self-hosted lexer reads files byte-by-byte and has no Unicode decoding,
    so a multi-byte UTF-8 character like '─' becomes 2-4 separate bytes
    that don't match any expected token). This is deliberately narrower
    than "first token isn't a known command" -- that check produces
    hundreds of false positives because block bodies (CHAIN/SWITCH/FOR
    items) legitimately use "NAME op args" syntax where NAME is never a
    top-level command word. Non-ASCII content is what actually breaks the
    byte-level reader, regardless of which block-body grammar applies.
    """
    findings = []
    try:
        with open(filepath, encoding='utf-8', errors='replace') as f:
            raw_lines = f.readlines()
    except OSError:
        return findings
    stripped = _strip_comments(raw_lines)

    block_stack = []  # [(opener_tok, opener_indent), ...]
    for lineno, raw, is_indented, text in stripped:
        if not text.strip():
            block_stack.clear()  # blank line: every reader's body-loop stops here too
            continue
        indent = len(raw) - len(raw.lstrip(' \t'))
        while block_stack and indent <= block_stack[-1][1]:
            block_stack.pop()

        has_non_ascii = any(ord(ch) > 127 for ch in text)
        if has_non_ascii:
            in_block = bool(block_stack)
            opener = block_stack[-1][0] if block_stack else None
            findings.append((lineno, in_block, opener, raw.rstrip()[:70]))

        first_tok = text.split()[0] if text.strip() else ''
        if first_tok in _BLOCK_OPENERS:
            block_stack.append((first_tok, indent))

    return findings


def mode_unwrapped(target_files=None):
    """Find uncommented decorative/unknown lines; flag the ones sitting
    inside an indentation-aware block body as needing a human read.
    """
    if target_files:
        files = [f if os.path.isabs(f) else os.path.join(_ROOT, f) for f in target_files]
    else:
        files = sorted(glob.glob(os.path.join(_ROOT, '**', '*.re'), recursive=True))

    print("=== Unwrapped lines: non-ASCII content outside any comment, classified by risk ===\n")
    total_risky = total_safe = 0
    for fp in files:
        findings = _scan_unwrapped_file(fp)
        if not findings:
            continue
        risky = [f for f in findings if f[1]]
        if not risky:
            total_safe += len(findings)
            continue
        rel = os.path.relpath(fp, _ROOT)
        print(f"--- {rel} ---")
        for lineno, in_block, opener, text in risky:
            print(f"  line {lineno}: INSIDE {opener} block -- {text!r}")
            total_risky += 1
        total_safe += len(findings) - len(risky)
        print()
    print(f"{total_risky} risky (inside a block body), {total_safe} safe "
          f"(outside any block -- top-level dispatcher already skips these).")
    if total_risky == 0:
        print("No lines found sitting inside a tracked block body. "
              "(Doesn't guarantee every block-reading mechanism is bug-free "
              "-- only that none currently land on an unwrapped line.)")



# ─────────────────────────────────────────────────────────────────────────────
# mode_invariants — comprehensive post-freeze invariant checker
# ─────────────────────────────────────────────────────────────────────────────
#
# Run after every significant code change.  Catches:
#   - Wrong constant values (ASCII, C_N, etc.)
#   - Bad ITO wiring (NEWSET_START El2, LOAD_CNS_WR El2, etc.)
#   - C_N address-formula gaps (C_7_REF breaking the table)
#   - Missing critical symbols
#   - Macro registration mistakes
#   - Self-hosting file processing progress
#
# Usage:
#   python3 diag.py --invariants

def mode_invariants():
    import re as _re
    import glob as _glob

    ldr, a, R, sym, errs = _load_fast()

    fails  = []   # definite bugs
    warns  = []   # suspicious but may be intentional
    oks    = []   # confirmed correct

    def ok(msg):   oks.append(msg)
    def warn(msg): warns.append(f'WARN  {msg}')
    def fail(msg): fails.append(f'FAIL  {msg}')

    def chk(label, expected, actual):
        if actual == expected:
            ok(f'{label} = {actual}')
        else:
            fail(f'{label}: expected {expected}, got {actual}')

    def sym_val(name):
        addr = R.get(name, 0)
        return int(a[addr]) if addr else None

    def ito_slot(name, slot):
        addr = R.get(name, 0)
        return int(a[addr + slot]) if addr else None

    print('=== Reca Invariants ===\n')

    # ── 1. ASCII character constants ──────────────────────────────────────────
    # Everything the lexer uses for comparisons.  Bugs here break tokenisation.
    ascii_expected = {
        # Control characters (from ascii.re NEWSET commands)
        'NUL': 0, 'TAB': 9, 'LF': 10, 'CR': 13, 'ESC': 27, 'SP': 32, 'DEL': 127,
        # Punctuation (from ascii.re)
        'MINUS': 45, 'PERCENT': 37, 'SLASH': 47, 'PERIOD': 46,
        'COLON': 58, 'SEMICOLON': 59, 'EQUALS': 61, 'QUESTION': 63,
        'LBRACE': 123, 'RBRACE': 125, 'LBRACKET': 91, 'RBRACKET': 93,
        'PIPE': 124, 'BACKSLASH': 92, 'TILDE': 126, 'UNDERSCORE': 95,
        'BACKTICK': 96, 'HASH': 35, 'AMP': 38, 'BANG': 33,
        # Digit range sentinels (used by lexer and BS_PARSE_INT)
        'ASCII_0': 48, 'ASCII_9': 57,
        # Letter range sentinels
        'ASCII_A': 65, 'ASCII_Z': 90, 'ASCII_a': 97, 'ASCII_z': 122,
    }
    # Note: LT/GT/PLUS/STAR etc. are comparison ASPECTS, not ASCII constants

    print('1. ASCII character constants:')
    for name, expected in sorted(ascii_expected.items()):
        if name not in R:
            continue  # optional
        v = sym_val(name)
        if v == expected:
            ok(f'{name}={v}')
        else:
            fail(f'{name}: expected {expected}, got {v}')

    # ── 2. C_N numeric constants  (C_N.word == N) ────────────────────────────
    # Critical for: ITO El fields, arithmetic, macro argument resolution.
    print('\n2. C_N numeric constants (word == N):')
    c_constants = {}
    for sym_name, addr in sorted(R.items()):
        m = _re.match(r'^C_(\d+)$', sym_name)
        if m:
            n = int(m.group(1))
            v = int(a[addr])
            c_constants[n] = (sym_name, addr, v)
            if v == n:
                ok(f'{sym_name}@{addr}={v}')
            else:
                fail(f'{sym_name}@{addr}: expected {n}, got {v}')

    # C_NEG1 special case
    cneg = R.get('C_NEG1', 0)
    if cneg:
        v = int(a[cneg])
        expected_neg1 = (1 << 64) - 1  # 0xFFFFFFFFFFFFFFFF
        if v == expected_neg1:
            ok(f'C_NEG1@{cneg}=0xFFFF...FFFF')
        else:
            fail(f'C_NEG1@{cneg}: expected -1 (u64), got {v}')

    # ── 3. C_N address-formula integrity ─────────────────────────────────────
    # Formula: addr(C_N) should equal addr(C_0) + 2*N for all N.
    # Gaps indicate extra symbols (like C_7_REF) inserted between consecutive C_N.
    print('\n3. C_N address-formula (addr(C_N) == C_0 + 2*N):')
    c0_addr = R.get('C_0', 0)
    if c0_addr and c_constants:
        formula_gaps = []
        for n, (sym_name, addr, val) in sorted(c_constants.items()):
            expected_addr = c0_addr + 2 * n
            if addr == expected_addr:
                ok(f'{sym_name} formula ✓ (@{addr})')
            else:
                diff = addr - expected_addr
                formula_gaps.append((n, sym_name, addr, expected_addr, diff))
                warn(f'{sym_name}@{addr} formula expects @{expected_addr} (off by {diff:+d})')
        if formula_gaps:
            # Find what's at the gap position
            for n, sym_name, addr, exp_addr, diff in formula_gaps:
                intruder = sym.get(exp_addr, '?')
                warn(f'  Gap for C_{n}: @{exp_addr} occupied by {intruder!r}')
    else:
        warn('C_0 not found, cannot check formula')

    # C_7_REF specific check
    c7ref = R.get('C_7_REF', 0)
    c7    = R.get('C_7', 0)
    c8    = R.get('C_8', 0)
    if c7ref and c7 and c8:
        # C_7_REF should NOT be between C_7 and C_8 (it breaks the formula)
        if c7 < c7ref < c8:
            fail(f'C_7_REF@{c7ref} sits BETWEEN C_7@{c7} and C_8@{c8} — breaks C_N formula')
        else:
            ok(f'C_7_REF@{c7ref} not between C_7 and C_8')
        # C_7_REF.word should == addr(C_7)
        v7ref = int(a[c7ref])
        if v7ref == c7:
            ok(f'C_7_REF.word = {c7} (addr of C_7) ✓')
        else:
            fail(f'C_7_REF.word = {v7ref}, expected addr(C_7)={c7}')

    # RA_C0_REF value
    ra_c0_ref = R.get('RA_C0_REF', 0)
    if ra_c0_ref:
        v = int(a[ra_c0_ref])
        if v == c0_addr:
            ok(f'RA_C0_REF.word = {v} = addr(C_0) ✓')
        else:
            fail(f'RA_C0_REF.word = {v}, expected addr(C_0)={c0_addr}')

    # ── 4. Critical ITO wiring ────────────────────────────────────────────────
    # These are the ITOs whose El1/El2 fields must point to specific registers.
    # Getting them wrong is the class of bug that caused the NEWSET issue.
    print('\n4. Critical ITO El field wiring:')

    ra_bs_pival = R.get('RA_BS_PIVAL', 0)
    ra_bs_el0   = R.get('RA_BS_EL0', 0)
    ra_ma0      = R.get('RA_MA0', 0)
    ra_ma1      = R.get('RA_MA1', 0)

    ito_checks = [
        # (ITO_name, slot, expected_sym_name, reason)
        ('NEWSET_START', SLOT_E1, 'RA_MA0',      'dest reg: name address'),
        ('NEWSET_START', SLOT_E2, 'RA_BS_PIVAL', 'value: parsed integer (NOT RA_MA1)'),
        ('SETREF_START', SLOT_E1, 'RA_MA0',      'dest reg'),
        ('SETREF_START', SLOT_E2, 'RA_MA1',      'value: symbol address (addr, not int)'),
        ('LOAD_CNS_WR',  SLOT_E1, 'RA_BS_EL0',   'dest reg'),
        ('LOAD_CNS_WR',  SLOT_E2, 'RA_BS_PIVAL', 'value: parsed integer'),
    ]
    for ito_name, slot, expected_sym, reason in ito_checks:
        if ito_name not in R:
            warn(f'{ito_name}: not found')
            continue
        actual_addr = ito_slot(ito_name, slot)
        expected_addr = R.get(expected_sym, 0)
        actual_sym = sym.get(actual_addr, f'#{actual_addr}')
        slot_name = {SLOT_E1: 'El1', SLOT_E2: 'El2', SLOT_EXIT: 'Exit'}.get(slot, f's{slot}')
        if actual_addr == expected_addr:
            ok(f'{ito_name}.{slot_name} = {expected_sym} ({reason})')
        else:
            fail(f'{ito_name}.{slot_name}: expected {expected_sym}@{expected_addr}, '
                 f'got {actual_sym}@{actual_addr}  [{reason}]')

    # ── 5. Macro NEWREF wiring ────────────────────────────────────────────────
    # Each macro NEWREF's word must point to a valid ITO (non-zero, non-self).
    print('\n5. Macro NEWREF wiring (word → entry ITO):')
    macro_newrefs = {
        'NEWSET':  'NEWSET_START',
        'SETREF':  'SETREF_START',
        'NEWREF':  None,           # word points to itself by convention
        'ITO':     None,           # built-in
        'CHAIN':   'CHAIN_START',
        'FOR':     'FOR_START',
        'SWITCH':  'SWITCH_START',
        'WAVE':    'WAVE_START',
        'JEQ':     'JEQ_START',
        'JZ':      'JZ_START',
        'ALLOC_RAW': None,
        'RVOCA':   None,
        'RCALL_AT': None,
    }
    for macro_name, entry_name in macro_newrefs.items():
        if macro_name not in R:
            continue
        macro_addr = R[macro_name]
        word = int(a[macro_addr])
        if entry_name is None:
            # Just check it's not 0 and not self (or IS self for self-refs)
            if word == 0:
                fail(f'{macro_name}.word = 0 (unset)')
            else:
                ok(f'{macro_name}.word = {sym.get(word, f"#{word}")}')
        else:
            entry_addr = R.get(entry_name, 0)
            if not entry_addr:
                warn(f'{macro_name}: entry {entry_name} not found')
            elif word == entry_addr:
                ok(f'{macro_name}.word → {entry_name}@{entry_addr} ✓')
            else:
                fail(f'{macro_name}.word = {sym.get(word, f"#{word}")}@{word}, '
                     f'expected {entry_name}@{entry_addr}')

    # ── 6. Key register sanity ────────────────────────────────────────────────
    # Registers should have word=0 (data luces start zeroed).
    # Non-zero after freeze is OK only for known exceptions.
    print('\n6. Key register values after freeze:')
    regs_expected_zero = [
        'RA_LINK', 'RA_LOAD_BYTE', 'RA_LOAD_TLEN', 'RA_LOAD_RPOS', 'RA_LOAD_RLEN',
        'RA_REDIRECT_BASE', 'RA_REDIRECT_POS', 'RA_REDIRECT_LEN',
        'RA_BS_PIVAL', 'RA_BS_EL0', 'RA_BS_RESULT', 'RA_BS_FLAG',
        'RA_MA0', 'RA_MA1', 'RA_MA2', 'RA_MA3',
        'SK_FIDX', 'SK_BLOCK_CMT', 'SK_IND_DEPTH',
    ]
    for rn in regs_expected_zero:
        if rn not in R: continue
        v = int(a[R[rn]])
        if v == 0:
            ok(f'{rn}=0')
        else:
            # After LOAD_MAIN some registers are non-zero — report but don't fail
            warn(f'{rn} = {v} (non-zero after freeze; may be OK)')

    # Known non-zero after freeze
    regs_known_nonzero = {
        'RA_SP': None,        # stack pointer
        'RA_C0_REF': c0_addr, # must == addr(C_0)
        'LF': 10, 'TAB': 9, 'CR': 13, 'SP': 32, 'NUL': 0,
    }
    for rn, expected in regs_known_nonzero.items():
        if rn not in R: continue
        v = int(a[R[rn]])
        if expected is None:
            ok(f'{rn}={v} (non-zero expected ✓)')
        elif v == expected:
            ok(f'{rn}={v} ✓')
        else:
            fail(f'{rn}: expected {expected}, got {v}')

    # ── 7. Buffer and lexer constants ─────────────────────────────────────────
    print('\n7. Lexer buffer constants:')
    buf_checks = {
        'BS_READBUF_SIZE': (lambda v: 64 <= v <= 65536, '64..65536'),
        'BS_READBUF_BASE': (lambda v: v > 0, '>0'),
        'BS_TOKBUF_BASE':  (lambda v: v > 0, '>0'),
    }
    for bname, (pred, desc) in buf_checks.items():
        v = sym_val(bname)
        if v is None:
            warn(f'{bname}: not found')
        elif pred(v):
            ok(f'{bname}={v} ({desc} ✓)')
        else:
            fail(f'{bname}={v}: expected {desc}')

    # ── 8. File-list integrity ────────────────────────────────────────────────
    print('\n8. File list integrity:')
    bs_file_count = sym_val('BS_FILE_COUNT')
    bs_file_list  = sym_val('BS_FILE_LIST')

    # Count actual .re source files
    here = os.path.dirname(os.path.abspath(__file__))
    re_files = sorted(glob.glob(os.path.join(here, '*.re')))
    re_files += sorted(glob.glob(os.path.join(here, 'generated', '*.re')))
    actual_count = len(re_files)

    if bs_file_count == actual_count:
        ok(f'BS_FILE_COUNT={bs_file_count} matches .re file count ✓')
    elif bs_file_count is None:
        warn('BS_FILE_COUNT not set')
    else:
        warn(f'BS_FILE_COUNT={bs_file_count} vs {actual_count} .re files on disk')

    if bs_file_list and bs_file_list > 0:
        ok(f'BS_FILE_LIST={bs_file_list} (non-zero ✓)')
    else:
        fail(f'BS_FILE_LIST={bs_file_list} (must be non-zero)')

    # ── 9. Self-ref for key NEWREFs ───────────────────────────────────────────
    print('\n9. Self-ref check (word == addr for data luces):')
    # All aspect ops must self-ref
    aspects = ['Read','Write','Add','Sub','Mul','Div','Rem',
                'And','Or','Xor','Left','Right','Equal','Less','ULess',
                'JumpIf','JumpReg','End','Exire','Voca','Redi',
                'Move','Jump','Not','Greater']
    asp_fails = []
    for asp in aspects:
        if asp not in R: continue
        addr = R[asp]; word = int(a[addr])
        if word != addr:
            asp_fails.append(f'{asp}@{addr}:word={word}')
    if asp_fails:
        for f in asp_fails:
            warn(f'Aspect self-ref modified (LOAD_MAIN side effect?): {f}')
    else:
        ok(f'All {len([x for x in aspects if x in R])} aspects self-ref ✓')

    # ── 10. LOAD_MAIN dispatch integrity ──────────────────────────────────────
    # Check that key dispatching is correctly wired.
    print('\n10. LOAD_MAIN dispatch wiring:')
    dispatch_checks = [
        ('LOAD_CMD_N_GROUP', SLOT_E1, 'BS_TOKBUF_BASE', 'reads tokbuf[1] for second byte'),
        ('LOAD_CNG_LU',      None,    'LOAD_CMD_UNKNOWN', 'tlen>3 N-group fallthrough'),
    ]
    for ito_name, slot, expected_ref, reason in dispatch_checks:
        if ito_name not in R:
            warn(f'{ito_name}: not found')
            continue
        if slot is not None:
            actual = ito_slot(ito_name, slot)
            expected = R.get(expected_ref, 0)
            slot_n = {SLOT_E1:'El1',SLOT_E2:'El2'}.get(slot,str(slot))
            if actual == expected:
                ok(f'{ito_name}.{slot_n} = {expected_ref} ✓  [{reason}]')
            else:
                fail(f'{ito_name}.{slot_n}: expected {expected_ref}@{expected}, '
                     f'got {sym.get(actual,f"#{actual}")}@{actual}  [{reason}]')

    # LOAD_CMD_UNKNOWN target for NEWSET (must call something other than NEWSET_START directly)
    # We just verify NEWSET macro's word
    newset_addr = R.get('NEWSET', 0)
    if newset_addr:
        word = int(a[newset_addr])
        word_sym = sym.get(word, f'#{word}')
        if word_sym == 'NEWSET_START':
            ok(f'NEWSET macro → NEWSET_START (macro dispatch)')
        else:
            warn(f'NEWSET macro.word = {word_sym} (expected NEWSET_START)')

    # ── 11. Self-hosting progress (live run) ──────────────────────────────────
    # Count how many files get ≥2 dispatches (real processing, not just "====..." + exit)
    print('\n11. Self-hosting processing depth:')
    try:
        from trace import make_patched_run as _mpr

        sk_fidx_addr = R.get('SK_FIDX', 0)
        ldc_addr     = R.get('LOAD_DISPATCH_CORE', 0)
        ldc_target   = int(a[ldc_addr]) if ldc_addr else 0  # = LOAD_DC_TLENCK addr
        ldc_sym      = sym.get(ldc_target, 'LOAD_DC_TLENCK')  # entry ITO name

        dispatch_counts = {}  # fi → count
        fi_cur2 = [0]

        def _on_voca(ev):
            # ev['value'] == ldc_target because we keyed on ldc_sym (the entry ITO)
            fi = ev['fi']
            dispatch_counts[fi] = dispatch_counts.get(fi, 0) + 1

        # Key on the ENTRY ITO (LOAD_DC_TLENCK), not the NEWREF (LOAD_DISPATCH_CORE).
        # Voca target = aether[LOAD_DISPATCH_CORE] = LOAD_DC_TLENCK addr = ldc_target.
        _mpr(on_voca_target={ldc_sym: _on_voca}, suppress_stderr=True)

        fully_processed = sum(1 for fi, cnt in dispatch_counts.items() if cnt >= 2)
        single_dispatch = sum(1 for fi, cnt in dispatch_counts.items() if cnt == 1)
        total_files = bs_file_count or actual_count

        ok(f'Files with ≥2 dispatches (real processing): {fully_processed}/{total_files}')
        if single_dispatch:
            warn(f'Files with exactly 1 dispatch (separator-only?): {single_dispatch}')
        if fully_processed == 0:
            fail('No files fully processed — self-hosting is broken')
        elif fully_processed < total_files * 0.5:
            warn(f'Only {fully_processed}/{total_files} files processed (< 50%)')
    except Exception as exc:
        warn(f'Self-hosting progress check failed: {exc}')

    # ── Summary ───────────────────────────────────────────────────────────────
    print(f'\n{"="*60}')
    print(f'FAILS: {len(fails)}  WARNS: {len(warns)}  OKs: {len(oks)}')
    print()

    if fails:
        print('FAILURES (must fix):')
        for f in fails:
            print(f'  {f}')
        print()
    if warns:
        print('WARNINGS (investigate):')
        for w in warns:
            print(f'  {w}')
        print()
    if not fails and not warns:
        print('All invariants ✓')
    elif not fails:
        print('No failures. Check warnings above.')

    return len(fails)


if __name__ == '__main__':
    els = sys.argv[1:]

    if '--lint' in els:
        mode_lint()
    elif '--block-lint' in els:
        targets = [a for a in els if not a.startswith('--')]
        mode_block_lint(targets if targets else None)
    elif '--unwrapped' in els:
        targets = [a for a in els if not a.startswith('--')]
        mode_unwrapped(targets if targets else None)
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
    elif '--htable' in els:
        # Replaces diag_loadmain.py --htable: look up a name in the runtime htable
        name_to_find = els[els.index('--htable') + 1] if els.index('--htable') + 1 < len(els) else ''
        _mode_htable(name_to_find)
    elif '--broken' in els:
        # Replaces diag_loadmain.py --broken: find next=0 luces in key infrastructure
        _mode_broken()
    elif '--wiring' in els:
        # Replaces dump_wiring.py: print lux wiring for one or more symbols
        syms = [s for s in els if not s.startswith('--')]
        _mode_wiring(syms)
    elif '--invariants' in els:
        mode_invariants()
    else:
        ldr, a, R, sym, errs = mode_health()
        print(); mode_graph()
