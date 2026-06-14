#!/usr/bin/env python3
"""
LOAD_MAIN Wave-B diagnostic tool.
Usage: python3 diag_loadmain.py [--file N] [--broken] [--htable NAME]

Runs LOAD_MAIN and reports:
 - How many files completed
 - Where the stall is (last vocas, byte, RA_LINK)
 - Broken next=0 luces in key infrastructure
 - Corrupt writes (to primitive area)
 - Htable lookup for a specific name
"""
import sys, glob, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import interpreter as interp_mod
from loader import freeze


def run_with_trace(target_file=None, check_htable=None):
    # First freeze — get symbol table and aether baseline
    l = freeze()
    R   = l.symbols
    a   = l.interp.aether.aether
    sym = {v: k for k, v in R.items()}

    # ── Primitive integrity ──────────────────────────────────────────────
    prims = ['Equal','Read','Write','Add','Move','JumpIf','Voca','Redi','Less','Mul']
    broken = [(n, R[n], a[R[n]]) for n in prims if R.get(n) and a[R[n]] != R[n]]
    print(f"Primitives: {'NONE BROKEN ✓' if not broken else broken}")

    # ── Register addresses ───────────────────────────────────────────────
    ld_fidx = R.get('LD_FIDX', 0)
    ra_link = R.get('RA_LINK', 0)
    ra_lb   = R.get('RA_LOAD_BYTE', 0)
    bs_tb   = R.get('BS_TOKBUF_BASE', 0)
    ra_tlen = R.get('RA_LOAD_TLEN', 0)

    wave2 = [False]; in_tgt = [False]
    file_entries = {}; corrupt = [0]
    last_v = []; last_dispatch = []

    # ── Monkey-patch interpreter methods to trace LOAD_MAIN ──────────────
    orig_move  = interp_mod.Interpreter._move
    orig_add   = interp_mod.Interpreter._add
    orig_voca  = interp_mod.Interpreter._voca
    orig_write = interp_mod.Interpreter._write

    def wm(self, a1, a2, exit, nxt, ae):
        if exit == ld_fidx and not wave2[0] and ae[ld_fidx] > 5:
            wave2[0] = True
        return orig_move(self, a1, a2, exit, nxt, ae)

    def wa(self, a1, a2, exit, nxt, ae):
        result = orig_add(self, a1, a2, exit, nxt, ae)
        if wave2[0] and exit == ld_fidx:
            fidx = ae[ld_fidx]
            if target_file is None or fidx == target_file:
                in_tgt[0] = True
            elif in_tgt[0]:
                in_tgt[0] = False
        return result

    def wv(self, a1, a2, e, n, ae):
        nm = sym.get(a1, '?')
        if nm == 'LOAD_FILE':
            file_entries[ae[ld_fidx]] = 1
        if wave2[0] and (target_file is None or in_tgt[0]):
            rl = sym.get(ae[ra_link], ae[ra_link])
            b  = ae[ra_lb] if ra_lb else 0
            last_v.append((nm, rl, b))
            if len(last_v) > 30: last_v.pop(0)
            if nm == 'LOAD_DISPATCH_LINE':
                tlen = ae[ra_tlen] if ra_tlen else 0
                tb   = ae[bs_tb]   if bs_tb   else 0
                if tb and 0 < tlen < 40:
                    try:
                        tok = bytes(min(255, int(ae[tb+i])) for i in range(min(tlen, 12))).decode('ascii', 'replace')
                    except: tok = f'?tlen={tlen}'
                else:
                    tok = '' if tlen == 0 else f'tlen={tlen}'
                last_dispatch.append(tok)
                if len(last_dispatch) > 20: last_dispatch.pop(0)
        return orig_voca(self, a1, a2, e, n, ae)

    def ww(self, a1, a2, e, n, ae):
        if a1 and 0 < ae[a1] < 10: corrupt[0] += 1
        return orig_write(self, a1, a2, e, n, ae)

    interp_mod.Interpreter._move  = wm
    interp_mod.Interpreter._add   = wa
    interp_mod.Interpreter._voca  = wv
    interp_mod.Interpreter._write = ww

    try:
        freeze()
    finally:
        interp_mod.Interpreter._move  = orig_move
        interp_mod.Interpreter._add   = orig_add
        interp_mod.Interpreter._voca  = orig_voca
        interp_mod.Interpreter._write = orig_write

    # ── Results ──────────────────────────────────────────────────────────
    _HERE = os.path.dirname(os.path.abspath(__file__))
    all_re = sorted(glob.glob(os.path.join(_HERE, '**', '*.re'), recursive=True))
    n_files   = len(file_entries)
    last_fidx = max(file_entries, default=-1)
    last_name = os.path.basename(all_re[last_fidx]) if 0 <= last_fidx < len(all_re) else '?'

    print(f"Files completed: {n_files}/{len(all_re)}  (stalled at [{last_fidx}]={last_name})")
    print(f"Corrupt writes: {corrupt[0]}")

    print(f"\nLast 12 vocas (voca, rl, byte):")
    for nm, rl, b in last_v[-12:]:
        bc = chr(b) if 32 <= b < 127 else '.'
        print(f"  {nm:30s} rl={str(rl):25s} byte={b}('{bc}')")

    print(f"\nLast dispatched tokens: {last_dispatch[-10:]}")

    # ── Broken next=0 in infrastructure ─────────────────────────────────
    safe_ops = {R.get(n, 0) for n in ('Redi','Jump','JumpIf','JumpReg','Voca','End')}
    broken_next = []
    for nm, addr in R.items():
        if not addr: continue
        if not any(nm.startswith(p) for p in ('LOAD_','BS_','HT_','AC_')): continue
        op  = a[addr + 1]
        nxt = a[addr + 5]
        if op and op not in safe_ops and nxt == 0:
            broken_next.append((addr, nm, sym.get(op, op)))
    print(f"\nBroken next=0 in LOAD_/BS_/HT_/AC_: {len(broken_next)}")
    for addr, nm, op in sorted(broken_next)[:10]:
        print(f"  {nm}({addr}): op={op}")

    # ── Htable lookup ────────────────────────────────────────────────────
    if check_htable:
        ht_base = a[R.get('BS_HT_BASE', 0)] if R.get('BS_HT_BASE') else 0
        ht_mask = a[R.get('BS_HT_MASK', 0)] if R.get('BS_HT_MASK') else 262143
        def djb2m(s):
            h = 5381
            for c in s.encode():
                h = ((h * 33 + c) & 0xFFFFFFFFFFFFFFFF) & ht_mask
            return h
        h = djb2m(check_htable)
        for i in range(min(ht_mask + 1, 100)):
            slot  = (h + i) & ht_mask
            entry = a[ht_base + slot]
            if entry == 0:
                print(f"  '{check_htable}': NOT IN HTABLE"); break
            if (entry >> 32) == h:
                found = entry & 0xFFFFFFFF
                sym_addr = R.get(check_htable, 0)
                match = '✓' if found == sym_addr else f'MISMATCH! htable={found} sym={sym_addr}'
                print(f"  '{check_htable}': addr={found} ({sym.get(found, found)}) {match}"); break

    return n_files, last_fidx, last_name


if __name__ == '__main__':
    els = sys.argv[1:]
    tgt_file   = None
    htable_name = None
    for i, arg in enumerate(els):
        if arg == '--file'   and i + 1 < len(els): tgt_file    = int(els[i + 1])
        if arg == '--htable' and i + 1 < len(els): htable_name = els[i + 1]
    run_with_trace(target_file=tgt_file, check_htable=htable_name)
