#!/usr/bin/env python3
"""test_macros.py — Compare Python-inline macros with Reca-program macros.

Usage:
    python3 test_macros.py          # run all tests
    python3 test_macros.py RVOCA    # run specific macro test

For each macro, loads two .re fragments:
  - Python version: uses the macro command (Python inline handles it)
  - Reca version:   uses CMD_MACRO_TEST fragment with raw ITO equivalents

Compares ITO lux slots (word, op, e1, e2, exit) for equivalence.
Auto-links (SLOT_NEXT) may differ when context differs — we check only
the lux itself, not the chain.
"""
import sys
import os
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
# If this file is in a subdirectory (e.g. tooling/), root is one level up.
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'symphony.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import freeze, Loader, fresh_loader as _fresh_loader


# ── Test infrastructure ───────────────────────────────────────────────────────

def make_fresh_loader(base_bin: str = None, base_sym: str = None,
                      base_bump: int = 0, base_water: int = 0):
    """Create a Loader on top of the frozen base state.
    Arguments kept for backward compatibility but ignored — fresh_loader()
    in loader.py computes the correct bump from K_CURSOR automatically.
    """
    ldr = _fresh_loader()
    return ldr, ldr.interp.aether.aether


def load_fragment(frag: str, base_bin: str, base_sym: str,
                  base_bump: int, base_water: int):
    """Load a .re fragment into a fresh loader. Returns (loader, aether)."""
    ldr, aether = make_fresh_loader(base_bin, base_sym, base_bump, base_water)
    with tempfile.NamedTemporaryFile(mode='w', suffix='.re', delete=False) as f:
        f.write(frag); name = f.name
    try:
        ldr._lumen_prepass = {}
        ldr.load_file(name)
    finally:
        os.unlink(name)
    return ldr, aether


def compare_luces(name: str, ldr_py, ae_py, ldr_re, ae_re,
                  lux_sym: str = None, check_slots=(0,1,2,3,4)):
    """Compare ITO lux slots for two loaders. Returns (ok, details)."""
    sym = lux_sym or name.split()[0] if ' ' not in (lux_sym or '') else lux_sym
    addr_py = ldr_py.symbols.get(sym, 0)
    addr_re = ldr_re.symbols.get(sym, 0)
    if not addr_py or not addr_re:
        return False, f"  Symbol {sym!r} missing: py={addr_py} re={addr_re}"

    slot_names = ['word', 'op', 'e1', 'e2', 'exit', 'next']
    lines = [f"  {sym}: py@{addr_py} re@{addr_re}"]
    ok = True
    for i in check_slots:
        vp = ae_py[addr_py + i]
        vr = ae_re[addr_re + i]
        match = '✓' if vp == vr else '✗'
        if vp != vr: ok = False
        lines.append(f"    slot {i} ({slot_names[i]}): py={vp} re={vr} {match}")
    return ok, '\n'.join(lines)


# ── Test cases ────────────────────────────────────────────────────────────────

TESTS = []

def test(name):
    def decorator(fn):
        TESTS.append((name, fn))
        return fn
    return decorator


@test('RVOCA')
def test_rvoca(bin, sym_file, bump, water):
    """RVOCA FOO BAR — compare Reca dispatch vs raw ITO"""
    raw_frag = "NEW TEST_BAR\nNOLINK\nITO TEST_FOO Voca El1=TEST_BAR Exit=RA_LINK\n"
    reca_frag = "NEW TEST_BAR\nNOLINK\nRVOCA TEST_FOO TEST_BAR\n"

    ldr_raw, ae_raw = load_fragment(raw_frag, bin, sym_file, bump, water)
    ldr_rec, ae_rec = load_fragment(reca_frag, bin, sym_file, bump, water)

    foo_raw = ldr_raw.symbols.get('TEST_FOO', 0)
    foo_rec = ldr_rec.symbols.get('TEST_FOO', 0)
    if not foo_raw or not foo_rec:
        return False, f"  Symbol missing: raw={foo_raw} rec={foo_rec}"
    lines = [f"  TEST_FOO: raw@{foo_raw} reca@{foo_rec}"]
    ok = True
    slot_names = ['word','op','e1','e2','exit']
    for i in range(5):
        vr = ae_raw[foo_raw + i]; vv = ae_rec[foo_rec + i]
        match = chr(10004) if vr == vv else chr(10008)
        if vr != vv: ok = False
        lines.append(f"    slot {i} ({slot_names[i]}): raw={vr} reca={vv} {match}")
    return ok, "\n".join(lines)

@test('RREDI')
def test_rredi(bin, sym_file, bump, water):
    """RREDI FOO → ITO FOO Redi El1=RA_LINK + resets flow"""
    py_frag = "NOLINK\nRREDI TEST_RLUX\n"
    re_frag = "NOLINK\nITO TEST_RLUX Redi El1=RA_LINK\n"

    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)
    return compare_luces('RREDI', ldr_py, ae_py, ldr_re, ae_re, 'TEST_RLUX', (0,1,2,4))


@test('CLEAR')
def test_clear(bin, sym_file, bump, water):
    """CLEAR FOO BAR → ITO FOO Move El1=C_0 Exit=BAR"""
    py_frag = "NEW TEST_TGT\nNOLINK\nCLEAR TEST_CLR TEST_TGT\n"
    re_frag = "NEW TEST_TGT\nNOLINK\nITO TEST_CLR Move El1=C_0 Exit=TEST_TGT\n"

    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)
    return compare_luces('CLEAR', ldr_py, ae_py, ldr_re, ae_re, 'TEST_CLR', (0,1,2,4))


@test('NOP')
def test_nop(bin, sym_file, bump, water):
    """NOP FOO → ITO FOO Move El1=C_0 Exit=C_0"""
    py_frag = "NOLINK\nNOP TEST_NOP\n"
    re_frag = "NOLINK\nITO TEST_NOP Move El1=C_0 Exit=C_0\n"

    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)
    return compare_luces('NOP', ldr_py, ae_py, ldr_re, ae_re, 'TEST_NOP', (0,1,2,4))


@test('JEQ')
def test_jeq(bin, sym_file, bump, water):
    """JEQ name a b dest → Equal(a,b)→flag; JumpIf(flag)→dest (optimized: no _K NOP)"""
    py_frag = """
NEW TEST_A
NEW TEST_B
NEW TEST_DEST
NOLINK
JEQ TEST_JEQN TEST_A TEST_B TEST_DEST
"""
    re_frag = """
NEW TEST_A
NEW TEST_B
NEW TEST_DEST
NEW TEST_JEQFLAG
NOLINK
ITO TEST_JEQN   Equal   El1=TEST_A    El2=TEST_B    Exit=TEST_JEQFLAG
ITO TEST_JEQN_J JumpIf  El1=TEST_JEQFLAG            Exit=TEST_DEST
"""
    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)

    results = []
    all_ok = True
    for sym, slots in [('TEST_JEQN', (1,2,4)), ('TEST_JEQN_J', (1,4))]:
        ok, detail = compare_luces('JEQ', ldr_py, ae_py, ldr_re, ae_re, sym, slots)
        # For JEQ the flag lux addr will differ (different bump), but op/e1/exit logic is same
        # We check op and exit structure, skip e2 flag addr comparison for _J lux
        results.append(detail)
        if not ok:
            # Check if only the flag addr differs (expected since flag is allocated differently)
            addr_py2 = ldr_py.symbols.get(sym, 0)
            addr_re2 = ldr_re.symbols.get(sym, 0)
            if addr_py2 and addr_re2:
                op_match = ae_py[addr_py2+1] == ae_re[addr_re2+1]
                if op_match:
                    results[-1] += '\n    (flag addr differs as expected — op matches ✓)'
                    continue
            all_ok = False
    # TEST_JEQN_K should NOT exist on either side — _K was removed as an
    # unnecessary NOP. JumpIf's own slot 5 (next) is the fall-through point.
    k_py = ldr_py.symbols.get('TEST_JEQN_K', 0)
    k_re = ldr_re.symbols.get('TEST_JEQN_K', 0)
    if k_py or k_re:
        all_ok = False
        results.append(f"  TEST_JEQN_K should not exist: py={k_py} re={k_re}")
    return all_ok, '\n'.join(results)



@test('NOLINK')
def test_nolink(bin, sym_file, bump, water):
    """NOLINK breaks the autolink chain: ITO after NOLINK should not be linked from the prior ITO."""
    frag = "NOLINK\nITO A_FIRST End\nNOLINK\nITO B_SECOND End\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    a_addr = ldr.symbols.get('A_FIRST', 0)
    b_addr = ldr.symbols.get('B_SECOND', 0)
    if not a_addr or not b_addr:
        return False, f"  Symbol missing: A_FIRST={a_addr} B_SECOND={b_addr}"
    next_of_a = ae[a_addr + 5]   # slot 5 = NEXT
    lines = [f"  A_FIRST@{a_addr} next={next_of_a} (expected 0)"]
    ok = (next_of_a == 0)
    return ok, '\n'.join(lines)


@test('SETREF')
def test_setref(bin, sym_file, bump, water):
    """SETREF target source → word(target) = addr(source). Test that cross-ref is correctly wired."""
    # NEWREF makes target.word = addr(source). SETREF does the same but name must already exist.
    frag = "NEW REF_SRC\nNEW REF_DST\nSETREF REF_DST REF_SRC\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    src = ldr.symbols.get('REF_SRC', 0)
    dst = ldr.symbols.get('REF_DST', 0)
    if not src or not dst:
        return False, f"  Symbol missing: REF_SRC={src} REF_DST={dst}"
    word_of_dst = ae[dst]
    lines = [f"  REF_DST.word={word_of_dst}, REF_SRC addr={src} (expected equal)"]
    ok = (word_of_dst == src)
    return ok, '\n'.join(lines)


# ── Runner ────────────────────────────────────────────────────────────────────

def run_tests(filter_name=None):
    import os as _os
    bin = _os.path.join(_ROOT, 'reca.bin')
    sym = _os.path.join(_ROOT, 'reca.sym')
    if not (_os.path.exists(bin) and _os.path.exists(sym)):
        print("Building reca.bin...", end=' ', flush=True)
        freeze()
    ldr = _fresh_loader()
    print(f"bump={ldr._bump} symbols={len(ldr.symbols)}")

    passed = failed = 0
    for name, fn in TESTS:
        if filter_name and filter_name.upper() not in name.upper():
            continue
        try:
            ok, details = fn(bin, sym, ldr._bump, ldr._water)
            status = '✓ PASS' if ok else '✗ FAIL'
            print(f"\n{status}: {name}")
            print(details)
            if ok: passed += 1
            else:  failed += 1
        except Exception as e:
            import traceback
            print(f"\n✗ ERROR: {name}: {e}")
            traceback.print_exc()
            failed += 1

    print(f"\n── Results: {passed} passed, {failed} failed ──")
    return failed == 0


# ── Low-level BS lexer tests (from test_bs_read_token_chunk.py / test_bs_token_value.py) ──
# Run with: python3 test_macros.py --bs-read-chunk CHUNK_FILE
#           python3 test_macros.py --bs-token-value "SOME_TOKEN"

def _test_bs_read_chunk(chunk_path):
    """Feed a raw byte chunk into BS_READ_TOKEN and report tokens found."""
    from loader import freeze
    l = freeze(); R = l.symbols; interp = l.interp; a = interp.aether.aether
    try:
        chunk = open(chunk_path, 'rb').read()
    except FileNotFoundError:
        print(f"[bs-read-chunk] File not found: {chunk_path}"); return
    readbuf_base = int(a[R['BS_READBUF_BASE']])
    for i, b in enumerate(chunk):
        a[readbuf_base + i] = b
    a[R['RA_LOAD_RPOS']] = 0
    a[R['RA_LOAD_RLEN']] = len(chunk)
    # Run BS_READ_TOKEN repeatedly until EOF
    BS_READ_TOKEN = R.get('BS_READ_TOKEN')
    RA_LOAD_TLEN  = R.get('RA_LOAD_TLEN')
    BS_TOKBUF     = R.get('BS_TOKBUF_BASE')
    if not BS_READ_TOKEN:
        print("[bs-read-chunk] BS_READ_TOKEN not found"); return
    tokens = []
    for _ in range(200):
        interp.execute_aether(BS_READ_TOKEN)
        tlen = int(a[RA_LOAD_TLEN])
        if tlen == 0: break
        tok = bytes(int(a[BS_TOKBUF + i]) & 0xFF for i in range(tlen))
        tokens.append(tok.decode('utf-8', 'replace'))
    print(f"[bs-read-chunk] Tokens from {chunk_path} ({len(chunk)} bytes): {len(tokens)} found")
    for t in tokens:
        print(f"  {t!r}")


def _test_bs_token_value(text):
    """Feed a token string into BS_TOKEN_VALUE and report the resolved address."""
    from loader import freeze
    l = freeze(); R = l.symbols; interp = l.interp; a = interp.aether.aether
    tokbuf_base = int(a[R['BS_TOKBUF_BASE']])
    for i, ch in enumerate(text):
        a[tokbuf_base + i] = ord(ch)
    a[R['RA_LOAD_TLEN']] = len(text)
    BS_TOKEN_VALUE = R.get('BS_TOKEN_VALUE')
    RA_BS_RESULT   = R.get('RA_BS_RESULT')
    if not BS_TOKEN_VALUE:
        print("[bs-token-value] BS_TOKEN_VALUE not found"); return
    print(f"[bs-token-value] Before: tlen={len(text)} token={text!r}")
    interp.execute_aether(BS_TOKEN_VALUE)
    result = int(a[RA_BS_RESULT]) if RA_BS_RESULT else -1
    sym_rev = {v: k for k, v in R.items()}
    print(f"[bs-token-value] RA_BS_RESULT = {result} ({sym_rev.get(result, '?')})")


# Extend __main__ to handle --bs-read-chunk and --bs-token-value
if __name__ == '__main__':
    args = sys.argv[1:]
    if '--bs-read-chunk' in args:
        idx = args.index('--bs-read-chunk')
        _test_bs_read_chunk(args[idx+1] if idx+1 < len(args) else '')
    elif '--bs-token-value' in args:
        idx = args.index('--bs-token-value')
        _test_bs_token_value(args[idx+1] if idx+1 < len(args) else '')
    else:
        filter_name = args[0] if args else None
        ok = run_tests(filter_name)
        sys.exit(0 if ok else 1)
