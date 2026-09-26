#!/usr/bin/env python3
"""Runtime test for the new, unwired self-hosted indent-classify functions
(LOAD_INDENT_STACK_INIT / LOAD_INDENT_CLASSIFY in wax.re).

These are NOT yet wired to any caller (LOAD_READ_LINE/FOR/SWITCH/etc still
use the old SK_IND_DEPTH 0/1/2+ scheme). This test loads the frozen base
program (which already includes the new functions, since they compile
cleanly and are simply unreferenced dead code so far), then directly
drives LOAD_INDENT_STACK_INIT and LOAD_INDENT_CLASSIFY via the real
interpreter, feeding SK_IND_COUNT by hand for each simulated line
(bypassing LOAD_READ_LINE_V2's byte-reading, which needs a real source
buffer -- that's exercised separately). This isolates and verifies the
classify() port itself against the same worked examples already
confirmed correct in loader.IndentStack.
"""
import sys
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) else os.path.dirname(_HERE)
sys.path.insert(0, _ROOT)

from loader import fresh_loader


def get_addr(ldr, name):
    addr = ldr.symbols.get(name, 0)
    if not addr:
        raise RuntimeError(f"symbol not found: {name}")
    return addr


def get_entry_addr(ldr, aether, seal_name):
    """SEAL Name=X Y stores an indirection: aether[addr(X)] == addr(Y), not
    addr(X) itself (confirmed directly -- op=0 at addr(X), it is not
    executable code). Normal calls go through RVOCA/Voca, which follow
    this indirection automatically (Voca returns aether[a1], not a1).
    execute_aether() needs the real, self-referencing entry address
    directly, so resolve through the stored value here."""
    seal_addr = get_addr(ldr, seal_name)
    return aether[seal_addr]


import tempfile


def _load_fragment(ldr, frag: str):
    """Load a .re fragment into an already-created loader (compile-time
    only, does not execute anything)."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.re', delete=False) as f:
        f.write(frag)
        name = f.name
    try:
        ldr._lumen_prepass = {}
        ldr.load_file(name)
    finally:
        os.unlink(name)


def run_via_rvoca(ldr, aether, callee_name: str):
    """Calling execute_aether directly on a function's own body skips
    Voca's own call-stack setup (pushing the old RA_LINK onto RA_SP
    before overwriting it), so the function's own RREDI -- which always
    assumes a real Voca call happened -- then reads garbage from an
    effectively-empty stack (confirmed directly: IndexError reading
    aether[sp] on the very first Redi). Fix: load a tiny wrapper fragment
    that does a real RVOCA call, then execute from the WRAPPER's entry
    point instead of the callee's body directly, so Voca sets up
    RA_SP/RA_LINK exactly as it would for any normal call.

    The wrapper must NOT end with its own RREDI: by the time the callee
    returns (via its own Redi El1=RA_LINK), the stack is already back to
    exactly where it was before this call -- there is nothing left to pop
    at this level. An extra RREDI here was popping an already-empty
    frame, corrupting RA_SP by _ra_frame_size on every single call
    (confirmed directly by single-stepping the interpreter: RA_SP ended
    at 16777223 instead of back at 16777215 after one call). Not
    emitting anything after the RVOCA line means execution lands on an
    unallocated (op=0) address when the callee returns, which has no
    dispatch handler -- exactly what's needed to stop cleanly here.

    Separately: the frozen base state's fault_vector is non-zero
    (confirmed directly: addr=1426003, value=1426005) -- it points at
    LOAD_MAIN's own error-recovery handler, built assuming a real,
    already-open .re file read context exists. When execute_aether hits
    the op=0 stop point above, it does NOT stop cleanly in that case --
    _run's own fallback is `elif fv and aether[fv]: pc = aether[fv]`, so
    it jumps into that handler instead, which itself loops forever
    trying to read from a read buffer this isolated test never opened
    (confirmed directly: this was the actual cause of the earlier
    apparent "infinite loop", not a bug in the new Reca functions being
    tested). Zero fault_vector before running any isolated fragment here
    so an unhandled op stops execution the same way manual
    single-stepping (which never consults fault_vector) already showed
    was correct.
    """
    fv = ldr.interp._fault_vector
    if fv:
        aether[fv] = 0

    frag = f"NOLINK\nRVOCA Name=TEST_WRAP_ENTRY {callee_name}\n"
    _load_fragment(ldr, frag)
    entry = get_addr(ldr, 'TEST_WRAP_ENTRY')
    ldr.interp.execute_aether(entry, no_cache=True)


def run_classify(ldr, aether, indent_count):
    """Set SK_IND_COUNT, call LOAD_INDENT_CLASSIFY, return (action, level, top)."""
    sk_ind_count = get_addr(ldr, 'SK_IND_COUNT')
    sk_ind_action = get_addr(ldr, 'SK_IND_ACTION')
    sk_ind_level = get_addr(ldr, 'SK_IND_LEVEL')
    sk_ind_top = get_addr(ldr, 'SK_IND_TOP')

    aether[sk_ind_count] = indent_count
    run_via_rvoca(ldr, aether, 'LOAD_INDENT_CLASSIFY')

    action = aether[sk_ind_action]
    level = aether[sk_ind_level]
    top = aether[sk_ind_top]
    return action, level, top


def run_init(ldr, aether, opener_indent):
    sk_ind_count = get_addr(ldr, 'SK_IND_COUNT')
    aether[sk_ind_count] = opener_indent
    run_via_rvoca(ldr, aether, 'LOAD_INDENT_STACK_INIT')


ACTION_NAMES = {0: 'skip', 1: 'same', 2: 'deeper', 3: 'return'}


def check(name, opener_indent, lines, expected):
    """lines: [(text, indent)]. expected: [(action_str, level_or_None)]."""
    ldr = fresh_loader()
    aether = ldr.interp.aether.aether

    # fresh_loader() loads the frozen static state (reca.bin), not a
    # runtime-executed LOAD_MAIN -- so SK_IND_STACK_BASE (set by a Move
    # inside LOAD_MAIN's own boot sequence) is still unset here. Set it
    # by hand to the buffer's real address, mirroring what that boot-time
    # Move would do. This isolates LOAD_INDENT_STACK_INIT/
    # LOAD_INDENT_CLASSIFY themselves; the full boot sequence (including
    # this Move actually running) is exercised separately by the full
    # self-hosting build via iris.py.
    sk_ind_stack_base = get_addr(ldr, 'SK_IND_STACK_BASE')
    sk_ind_stack_buf = get_addr(ldr, 'SK_IND_STACK')
    aether[sk_ind_stack_base] = sk_ind_stack_buf

    run_init(ldr, aether, opener_indent)

    got = []
    for _text, indent in lines:
        action, level, _top = run_classify(ldr, aether, indent)
        action_str = ACTION_NAMES.get(action, f'UNKNOWN({action})')
        level_val = level if action_str != 'skip' else None
        got.append((action_str, level_val))

    ok = got == expected
    status = 'PASS' if ok else 'FAIL'
    print(f'[{status}] {name}')
    if not ok:
        for (text, indent), g, e in zip(lines, got, expected):
            marker = '  ' if g == e else '  <-- MISMATCH'
            print(f'    {text!r:10} indent={indent:3} got={g!r:20} expected={e!r:20}{marker}')
    return ok


def check_count_indent(name, text, expected, redirect=False):
    """Drive LOAD_COUNT_INDENT against a hand-built read buffer."""
    ldr = fresh_loader()
    aether = ldr.interp.aether.aether

    base = get_addr(ldr, 'BS_READBUF_000')
    aether[get_addr(ldr, 'BS_READBUF_BASE')] = base
    for i, ch in enumerate(text):
        aether[base + i] = ord(ch)
    aether[get_addr(ldr, 'RA_LOAD_RPOS')] = 0
    aether[get_addr(ldr, 'RA_LOAD_RLEN')] = len(text)
    aether[get_addr(ldr, 'RA_REDIRECT_BASE')] = 12345 if redirect else 0

    run_via_rvoca(ldr, aether, 'LOAD_COUNT_INDENT')

    got = aether[get_addr(ldr, 'SK_IND_COUNT')]
    rpos = aether[get_addr(ldr, 'RA_LOAD_RPOS')]
    ok = (got == expected) and (rpos == 0)
    print(f'[{"PASS" if ok else "FAIL"}] {name}')
    if not ok:
        print(f'    text={text!r} got={got} expected={expected} '
              f'RA_LOAD_RPOS={rpos} (must stay 0 -- it must consume nothing)')
    return ok


def check_mark(name, opener_indent, indents, expected):
    """Drive LOAD_INDENT_MARK; expected is [(level, marker), ...]."""
    ldr = fresh_loader()
    aether = ldr.interp.aether.aether
    aether[get_addr(ldr, 'SK_IND_STACK_BASE')] = get_addr(ldr, 'SK_IND_STACK')

    aether[get_addr(ldr, 'SK_IND_COUNT')] = opener_indent
    run_via_rvoca(ldr, aether, 'LOAD_INDENT_STACK_INIT')

    got = []
    for ind in indents:
        aether[get_addr(ldr, 'SK_IND_COUNT')] = ind
        run_via_rvoca(ldr, aether, 'LOAD_INDENT_MARK')
        got.append((aether[get_addr(ldr, 'RA_LOAD_INDENT')],
                    aether[get_addr(ldr, 'SK_IND_DEPTH')]))

    ok = got == expected
    print(f'[{"PASS" if ok else "FAIL"}] {name}')
    if not ok:
        for ind, g, e in zip(indents, got, expected):
            mark = '  ' if g == e else '  <-- MISMATCH'
            print(f'    indent={ind:3} got=(level={g[0]}, marker={g[1]}) '
                  f'expected=(level={e[0]}, marker={e[1]}){mark}')
    return ok


def main():
    results = []

    # ── LOAD_COUNT_INDENT ────────────────────────────────────────────
    results.append(check_count_indent("count: unindented line", "FOR X", 0))
    results.append(check_count_indent("count: 4-space indent", "    NOVA Y", 4))
    results.append(check_count_indent("count: 8-space indent", "        Z", 8))
    results.append(check_count_indent("count: tab counts as 4", "\tQ", 4))
    results.append(check_count_indent("count: mixed spaces then tab", "  \tQ", 6))
    results.append(check_count_indent(
        "count: RA_LOAD_RLEN bound respected (empty buffer terminates)", "", 0))
    results.append(check_count_indent(
        "count: redirect mode short-circuits to 0 (BS_READBUF is not the active source)",
        "        Z", 0, redirect=True))

    # ── LOAD_INDENT_CLASSIFY ─────────────────────────────────────────
    results.append(check(
        "user's full worked example",
        0,
        [('TEXT1', 4), ('TEXT2', 8), ('TEXT3', 12), ('TEXT4', 8),
         ('TEXT5', 12), ('TEXT6', 4), ('TEXT7', 8)],
        [('deeper', 1), ('deeper', 2), ('deeper', 3), ('return', 2),
         ('deeper', 3), ('return', 1), ('deeper', 2)],
    ))

    results.append(check(
        "flat body: every item at the same single level",
        0,
        [('A', 4), ('B', 4), ('C', 4)],
        [('deeper', 1), ('same', 1), ('same', 1)],
    ))

    results.append(check(
        "stray/garbage indent is skipped, doesn't disturb the stack",
        0,
        [('A', 4), ('B', 8), ('STRAY', 6), ('C', 8), ('D', 4)],
        [('deeper', 1), ('deeper', 2), ('skip', None), ('same', 2), ('return', 1)],
    ))

    results.append(check(
        "return exactly to opener's own level is inert (normalized to skip)",
        0,
        [('A', 4), ('B', 8), ('BACK_TO_OPENER', 0)],
        [('deeper', 1), ('deeper', 2), ('skip', None)],
    ))

    results.append(check(
        "nested opener (opener_indent itself nonzero)",
        4,
        [('A', 8), ('B', 12), ('C', 8), ('D', 4)],
        [('deeper', 1), ('deeper', 2), ('return', 1), ('skip', None)],
    ))

    # ── LOAD_INDENT_MARK ─────────────────────────────────────────────
    results.append(check_mark(
        "mark: exact level in RA_LOAD_INDENT, marker capped at 2 for the expander",
        0,
        [4, 8, 12, 8, 4, 0],
        [(1, 1), (2, 2), (3, 2), (2, 2), (1, 1), (0, 0)],
    ))

    n_pass = sum(results)
    n_total = len(results)
    print(f'\n{n_pass}/{n_total} passed')
    return n_pass == n_total


if __name__ == '__main__':
    ok = main()
    sys.exit(0 if ok else 1)
