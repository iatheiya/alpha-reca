#!/usr/bin/env python3
"""test_key_table.py -- Phase-2 KEY_HTAB regression test.

The data-driven key recognizer replaces the AUTOGEN MATCH byte-tree with a real
hash table (KEY_HTAB) of the key vocabulary, pre-computed and written as static
data by loader.py's AUTOGEN_KEY_TABLE. The table is verified here directly:
HT_LOOKUP is a self-contained, already-tested routine, so we drive it on the
built table and confirm every key resolves to its code and non-keys miss --
the behavioural guard for the recognizer self-hosted arg-reading depends on.

Run: python test_key_table.py   (exit 0 = pass)
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import freeze, _reca_djb2, _discover_key_vocabulary


def main():
    ld = freeze()
    a = ld.interp.aether.aether
    s = ld.symbols
    keys = sorted(_discover_key_vocabulary())
    tokbuf = s['BS_TOKBUF_000']

    # --- djb2 must stay byte-identical to the interpreter's runtime hash ---
    h33 = s['BS_RT_H33']
    for w in ('Op', 'Body', 'Exit', 'Count'):
        a[s['RA_LOAD_HASH']] = 5381
        for ch in w:
            a[s['RA_LOAD_BYTE']] = ord(ch)
            ld.interp._run(h33, 0, None, False, 2)
        assert (a[s['RA_LOAD_HASH']] & ((1 << 64) - 1)) == _reca_djb2(w), \
            f"djb2 drift on {w!r} -- precomputed KEY_HTAB entries would be wrong"

    # --- structural: every key present with correct word0/word1/dest ---
    mask = a[s['KEY_HT_MASK']]
    base = s['KEY_HTAB']
    assert a[s['KEY_HT_BASE']] == base
    used = {0: None}   # slot 0 reserved (the HTAB pointer cell) -- mirrors loader.py
    for code, k in enumerate(keys, start=1):
        h32 = _reca_djb2(k) & 0xFFFFFFFF
        slot = h32 & mask
        while slot in used:
            slot = (slot + 1) & mask
        used[slot] = k
        # bucket i at base + i*2 (word0) / +1 (word1). No +2 header -- the
        # "eliminate HT +2-header convention" cleanup removed it from
        # HT_LOOKUP and every writer; this test was the last stale +2.
        w0 = a[base + 2 * slot]
        w1 = a[base + 1 + 2 * slot]
        nm = s['KEY_NAME_' + k]
        assert (w0 >> 32) == h32 and (w0 & 0xFFFFFFFF) == code, f"word0 wrong for {k}"
        assert w1 == nm and a[nm] == len(k), f"name entry wrong for {k}"
        assert a[s['KEY_DEST'] + code] == s['SK_ARG_' + k], f"dest wrong for {k}"

    # --- live: drive the real HT_LOOKUP over the built table ---
    def lookup(name):
        for i, ch in enumerate(name):
            a[tokbuf + i] = ord(ch)
        a[s['RA_HT_NAME']] = tokbuf
        a[s['RA_HT_NLEN']] = len(name)
        a[s['RA_HT_HASH']] = _reca_djb2(name) & 0xFFFFFFFF
        a[s['RA_HT_BASE']] = a[s['KEY_HT_BASE']]
        a[s['RA_HT_SIZE']] = a[s['KEY_HT_SIZE']]
        a[s['RA_HT_MASK']] = a[s['KEY_HT_MASK']]
        a[s['RA_HT_RESULT']] = 0xDEADBEEF
        a[s['RA_SP']] = 16000000
        try:
            # HT_LK_FOUND sets RA_HT_RESULT before the final Redi; the post-return
            # jump (we call it detached, with no real return frame) may fault
            # afterwards -- harmless, the result is already written.
            ld.interp._run(s['HT_LOOKUP'], 0, None, False, 5000)
        except Exception:
            pass
        return a[s['RA_HT_RESULT']]

    for code, k in enumerate(keys, start=1):
        got = lookup(k)
        assert got == code, f"HT_LOOKUP({k}) = {got}, expected code {code}"
    for miss in ('Zzz', 'RA_X', 'Nope'):
        got = lookup(miss)
        assert got == 0, f"HT_LOOKUP({miss}) = {got}, expected miss (0)"

    # --- live: drive the rewritten LOAD_ELEM recognizer on real tokens ---
    code = {k: i for i, k in enumerate(keys, start=1)}

    def classify(tok):
        for i, ch in enumerate(tok):
            a[tokbuf + i] = ord(ch)
        a[s['RA_LOAD_TLEN']] = len(tok)
        a[s['SK_ELEM_KEY']] = 0xEEEE
        a[s['RA_SP']] = 16000000
        a[s['RA_LINK']] = s['AC_G_HALT']
        try:
            ld.interp._run(s['LOAD_ELEM'], 0, None, False, 20000)
        except Exception:
            pass
        return a[s['SK_ELEM_KEY']]

    for k in keys:
        got = classify(k + '=X')
        assert got == code[k], f"LOAD_ELEM({k}=X) key={got}, expected {code[k]}"
    for pos in ('RA_FOO', 'Zzz=1', 'NOTKEY=9'):   # positional / non-key-with-= -> 0
        got = classify(pos)
        assert got == 0, f"LOAD_ELEM({pos}) key={got}, expected 0 (positional)"

    # --- live: drive the generic keyed-arg STORE on an integer value ---
    # Post-classify state (tokbuf = value bytes, SK_ELEM_KEY = code) drives the
    # KEY_DEST-routed store: LOAD_ELEM_RESOLVE mints/finds a constant cell
    # holding the value (the int-interning path, mirroring _lit_const_cache),
    # then the store writes that cell's address into SK_ARG_<key> and kind=1
    # into the adjacent slot. Guards both the routing (correct slot, no
    # cross-write) and the resolved value (was the broken addr(C_0)+value*2).
    def store_int(key, numtok):
        for i, ch in enumerate(numtok):
            a[tokbuf + i] = ord(ch)
        a[s['RA_LOAD_TLEN']] = len(numtok)
        a[s['RA_LOAD_HASH']] = _reca_djb2(numtok) & ((1 << 64) - 1)
        a[s['SK_ELEM_KEY']] = code[key]
        dest = s['SK_ARG_' + key]
        a[dest] = 0xDEAD
        a[dest + 1] = 0xDEAD
        a[s['RA_LOAD_BYTE']] = 10   # LF -> the arg loop exits after this one arg
        a[s['RA_SP']] = 16000000
        a[s['RA_LINK']] = s['AC_G_HALT']
        try:
            ld.interp._run(s['LOAD_RKVA_KBASE'], 0, None, False, 8000)
        except Exception:
            pass
        return a[dest], a[dest + 1]

    for key, numtok in (('Op', '7'), ('El1', '3'), ('Count', '256'), ('El2', '0')):
        addr, kind = store_int(key, numtok)
        assert a[addr] == int(numtok), \
            f"store {key}={numtok}: cell holds {a[addr]}, expected {numtok}"
        assert kind == 1, f"store {key}={numtok}: kind={kind}, expected 1 (int)"

    print(f"KEY_HTAB: {len(keys)} keys -- djb2, structural, live HT_LOOKUP, "
          f"live LOAD_ELEM recognition, keyed int store all pass")


if __name__ == '__main__':
    main()
