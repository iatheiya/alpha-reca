#!/usr/bin/env python3
"""test_macros.py — Compare Python-inline macros with Reca-program macros.

Usage:
    python3 test_macros.py          # run all tests
    python3 test_macros.py RVOCA    # run specific macro test

For each macro, loads two .re fragments:
  - Python version: uses the macro command (Python inline handles it)
  - Reca version:   uses CMD_MACRO_TEST fragment with raw NOVA equivalents

Compares NOVA lux slots (word, op, e1, e2, exit) for equivalence.
Auto-links (SLOT_NEXT) may differ when context differs — we check only
the lux itself, not the chain.
"""
import sys
import os
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
# If this file is in a subdirectory (e.g. tooling/), root is one level up.
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
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
                  lux_sym: str = None, check_slots=(0,1,2,3,4),
                  addr_may_differ=()):
    """Compare NOVA lux slots for two loaders. Returns (ok, details).

    addr_may_differ lists slots holding an address that the two sides are
    legitimately allowed to allocate differently -- e.g. JEQ's flag lux,
    which the macro allocates itself on one side while the hand-written
    equivalent declares it explicitly on the other. Such a slot is only a
    real failure if one side has an address and the other does not; two
    differing non-zero addresses are expected and reported as such,
    rather than printed as a bare mismatch.
    """
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
        if vp == vr:
            match = '✓'
        elif i in addr_may_differ and vp and vr:
            match = '✓ (addr allocated differently on each side, as expected)'
        else:
            match = '✗'
            ok = False
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
    """RVOCA FOO BAR — compare Reca dispatch vs raw NOVA"""
    raw_frag = "NEW TEST_BAR\nNOLINK\nNOVA TEST_FOO Op=Voca El1=TEST_BAR Exit=RA_LINK\n"
    reca_frag = "NEW TEST_BAR\nNOLINK\nRVOCA TEST_FOO Body=TEST_BAR\n"

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
    """RREDI FOO → NOVA FOO Redi El1=RA_LINK + resets flow"""
    py_frag = "NOLINK\nRREDI TEST_RLUX\n"
    re_frag = "NOLINK\nNOVA TEST_RLUX Op=Redi El1=RA_LINK\n"

    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)
    return compare_luces('RREDI', ldr_py, ae_py, ldr_re, ae_re, 'TEST_RLUX', (0,1,2,4))


@test('CLEAR')
def test_clear(bin, sym_file, bump, water):
    """CLEAR FOO BAR → NOVA FOO Move El1=C_0 Exit=BAR"""
    py_frag = "NEW TEST_TGT\nNOLINK\nCLEAR TEST_CLR TEST_TGT\n"
    re_frag = "NEW TEST_TGT\nNOLINK\nNOVA TEST_CLR Move El1=C_0 Exit=TEST_TGT\n"

    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)
    return compare_luces('CLEAR', ldr_py, ae_py, ldr_re, ae_re, 'TEST_CLR', (0,1,2,4))


@test('NOP')
def test_nop(bin, sym_file, bump, water):
    """NOP FOO → NOVA FOO Move El1=C_0 Exit=C_0"""
    py_frag = "NOLINK\nNOP TEST_NOP\n"
    re_frag = "NOLINK\nNOVA TEST_NOP Op=Move El1=C_0 Exit=C_0\n"

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
JEQ TEST_JEQN El1=TEST_A El2=TEST_B Exit=TEST_DEST
"""
    re_frag = """
NEW TEST_A
NEW TEST_B
NEW TEST_DEST
NEW TEST_JEQFLAG
NOLINK
NOVA TEST_JEQN   Op=Equal   El1=TEST_A    El2=TEST_B    Exit=TEST_JEQFLAG
NOVA TEST_JEQN_J Op=JumpIf  El1=TEST_JEQFLAG            Exit=TEST_DEST
"""
    ldr_py, ae_py = load_fragment(py_frag, bin, sym_file, bump, water)
    ldr_re, ae_re = load_fragment(re_frag, bin, sym_file, bump, water)

    results = []
    all_ok = True
    # TEST_JEQN's exit slot (4) holds the flag lux: the JEQ macro allocates
    # it itself on the py side, while the hand-written re side declares
    # TEST_JEQFLAG explicitly -- so the two addresses are expected to
    # differ. Everything else must match exactly.
    for sym, slots, may_differ in [('TEST_JEQN', (1, 2, 4), (4,)),
                                   ('TEST_JEQN_J', (1, 4), ())]:
        ok, detail = compare_luces('JEQ', ldr_py, ae_py, ldr_re, ae_re, sym, slots,
                                   addr_may_differ=may_differ)
        results.append(detail)
        if not ok:
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
    """NOLINK breaks the autolink chain: NOVA after NOLINK should not be linked from the prior NOVA."""
    frag = "NOLINK\nNOVA A_FIRST End\nNOLINK\nNOVA B_SECOND End\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    a_addr = ldr.symbols.get('A_FIRST', 0)
    b_addr = ldr.symbols.get('B_SECOND', 0)
    if not a_addr or not b_addr:
        return False, f"  Symbol missing: A_FIRST={a_addr} B_SECOND={b_addr}"
    next_of_a = ae[a_addr + 5]   # slot 5 = NEXT
    lines = [f"  A_FIRST@{a_addr} next={next_of_a} (expected 0)"]
    ok = (next_of_a == 0)
    return ok, '\n'.join(lines)


@test('SET_VALUE_CROSSREF')
def test_set_value_crossref(bin, sym_file, bump, water):
    """SET target Value=source, target already declared -> word(target) = addr(source).

    This is SEAL's old whole job (SEAL X Y: word(X) = addr(Y)) before SEAL
    was retired as a pure subset of SET -- Value=Y is the same cross-ref
    write, just spelled through SET instead of a separate command. Exercises
    the case where the target already exists via a prior NEW (SET's own
    allocate-if-missing step is then a no-op and only the cross-ref wiring
    applies).
    """
    frag = "NEW SEAL_SRC\nNEW SEAL_DST\nSET SEAL_DST Value=SEAL_SRC\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    src = ldr.symbols.get('SEAL_SRC', 0)
    dst = ldr.symbols.get('SEAL_DST', 0)
    if not src or not dst:
        return False, f"  Symbol missing: SEAL_SRC={src} SEAL_DST={dst}"
    word_of_dst = ae[dst]
    lines = [f"  SEAL_DST.word={word_of_dst}, SEAL_SRC addr={src} (expected equal)"]
    ok = (word_of_dst == src)
    return ok, '\n'.join(lines)


@test('SET_BARE_SELFREF')
def test_set_bare_selfref(bin, sym_file, bump, water):
    """Bare SET X (no Value=/Of=) defaults to self-ref: word(X) = addr(X).

    SEAL's other old job (bare `SEAL X`) -- kept as SET's own default rather
    than a separate command, since `SET X` and `SET X Value=X` write
    identically and nothing else meaningful for a name's word content."""
    frag = "SET SELFREF_X\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    addr = ldr.symbols.get('SELFREF_X', 0)
    if not addr:
        return False, "  Symbol missing: SELFREF_X"
    word = ae[addr]
    lines = [f"  SELFREF_X@{addr}, word={word} (expected equal)"]
    ok = (word == addr)
    return ok, '\n'.join(lines)


@test('IN_REPLACES_OWN_FREE')
def test_in_replaces_own_free(bin, sym_file, bump, water):
    """NEW Name=X In=OWN places X in the OWN region; no In= means the
    ordinary pool.

    This replaces STANDALONE_OWN_FREE, which asserted that `OWN X` then
    `FREE X` left X not-owned. Both commands are gone: they were two words
    (plus three negations) answering one question -- where does this name
    live -- and neither had a single use left in the project. In= answers
    it once, by naming the region, so a placement is stated rather than
    toggled, and the kernel no longer has to know which placements exist.
    """
    frag = "NEW SA_IN_A In=OWN\nNEW SA_IN_B\n"
    ldr, ae = load_fragment(frag, bin, sym_file, bump, water)
    if not ldr.symbols.get('SA_IN_A') or not ldr.symbols.get('SA_IN_B'):
        return False, "  Symbol missing: SA_IN_A / SA_IN_B"
    a_owned = 'own' in ldr._name_regions.get('SA_IN_A', set())
    b_owned = 'own' in ldr._name_regions.get('SA_IN_B', set())
    ok = a_owned and not b_owned
    return ok, (f"  SA_IN_A owned={a_owned} (expected True), "
                f"SA_IN_B owned={b_owned} (expected False)")


@test('OWN_BLOCK_CONTIGUOUS')
def test_own_block_contiguous(bin, sym_file, bump, water):
    """Every name placed In=OWN lands in one contiguous reserved range,
    physically together, regardless of which file declared it -- Pattern W,
    BUGS.md. A name never marked OWN must NOT fall inside that range.

    (The old cross-form cases -- the `| OWN` modifier tail, and the
    standalone `OWN`/`FREE` commands with their last-declaration-wins rule
    -- are gone: those forms were retired in favour of the single In=REGION
    placement, so there is only one form left to test.)
    """
    import tempfile, os as _os
    ldr, ae = make_fresh_loader(bin, sym_file, bump, water)
    ldr._lumen_prepass = {}
    with tempfile.TemporaryDirectory() as d:
        p1 = _os.path.join(d, 'a_first.re')
        p2 = _os.path.join(d, 'b_second.re')
        with open(p1, 'w') as f:
            f.write("NEW OB_A In=OWN\nNEW OB_X In=OWN\n")
        with open(p2, 'w') as f:
            f.write("NEW OB_B In=OWN\nNEW OB_C\n")
        ldr._load_group([p1, p2])

    a = ldr.symbols.get('OB_A', 0)
    b = ldr.symbols.get('OB_B', 0)
    c = ldr.symbols.get('OB_C', 0)
    x = ldr.symbols.get('OB_X', 0)
    blk = ldr._combo_blocks.get(frozenset({'own'}), [0, 0, 0])
    base, end = blk[0], blk[2]
    in_block = lambda addr: base <= addr < end

    checks = [
        ('OB_A (In=OWN, file a) in block', in_block(a), True),
        ('OB_B (In=OWN, file b) in block', in_block(b), True),
        ('OB_X (In=OWN, file a) in block', in_block(x), True),
        ('OB_C (never marked) in block', in_block(c), False),
    ]
    ok = all(actual == expected for _, actual, expected in checks)
    lines = [f"  OWN block: base={base} end={end}"] + [
        f"  {label}: {actual} (expected {expected})" for label, actual, expected in checks
    ]
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


# ── iris hub contract ─────────────────────────────────────────────────────
# test_macros stays a standalone file (a full macro-equivalence harness with
# its own per-test loaders) but plugs into the iris hub. NEEDS="trace": each
# test builds its own loader from reca.bin/reca.sym, so it shares nothing
# with the hub's pooled build.
from iris import Command


def _cmd_macro_tests(ctx, args) -> int:
    """Run the macro equivalence tests (optionally filtered by name)."""
    filter_name = next((x for x in args if not x.startswith('-')), None)
    return 0 if run_tests(filter_name) else 1


COMMANDS = [
    Command("macro-tests", "trace", _cmd_macro_tests, is_check=True,
            takes_args=True,
            help="macro equivalence tests (Python vs self-hosted)"),
]


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
