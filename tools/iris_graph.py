#!/usr/bin/env python3
"""iris_graph.py — built-graph diagnostics: everything that inspects the
allocated/wired graph, not just source text.

Commands here declare NEEDS="fresh" (thawed reca.bin is enough — symbol
table + aether) or NEEDS="freeze" (a real rebuild, for the few that read
Wave-1 build-time bookkeeping like _name_regions / _combo_blocks / _nova_addrs_set,
which fresh_loader never repopulates).

Each command's detection logic is the original check_*.py / diag mode
algorithm, moved verbatim; only its harness is replaced by the iris
Command contract.
"""
import re
import os
from collections import Counter

from aether import (SLOT_WORD, SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT,
                      SLOT_NEXT, NOVA_SIZE, LUMEN_PAIR, AETHER_SIZE)

from iris import Command
from iris_common import (wax_cmd_registrations, read_packed_string,
                         parse_source_line, iter_source_lines)


# ── op identity (was check_op_identity.py) ────────────────────────────────
def _op_identity(ctx) -> int:
    """Verify every named instruction's compiled op matches what its own
    source declaration says. Catches a name collision silently skipping the
    intended instruction's allocation (BUGS.md Pattern Z)."""
    a = ctx.aether
    sym = ctx.symbols
    rev = ctx.rev

    mismatches = []
    checked = 0

    for fn in ctx.re_files:
        for lineno, line in iter_source_lines(fn):
                rec = parse_source_line(line)
                if rec is None:
                    continue
                cmd, args, _slots = rec
                if cmd != 'NOVA' or len(args) < 2:
                    continue
                label, opword = args[0], args[1]
                if '{' in label or opword not in sym or label not in sym:
                    continue
                addr = sym[label]
                real_op = int(a[addr + 1])
                expected_op = sym[opword]
                checked += 1
                if real_op != expected_op:
                    mismatches.append(
                        (fn, lineno, label,
                         f"declared {opword!r}, compiled op resolves to "
                         f"{rev.get(real_op, f'#{real_op}')!r}"))

    equal_addr = sym['Equal']
    for fn in ctx.re_files:
        for lineno, line in iter_source_lines(fn):
                rec = parse_source_line(line)
                if rec is None:
                    continue
                cmd, args, _slots = rec
                if cmd not in ('JEQ', 'JZ') or not args:
                    continue
                label = args[0]
                if '{' in label or label not in sym:
                    continue
                addr = sym[label]
                real_op = int(a[addr + 1])
                checked += 1
                if real_op != equal_addr:
                    mismatches.append(
                        (fn, lineno, label,
                         f"JEQ/JZ own name should compile to 'Equal', "
                         f"actually resolves to "
                         f"{rev.get(real_op, f'#{real_op}')!r}"))

    print(f"Checked {checked} named declarations (NOVA/JEQ/JZ) project-wide.")
    if not mismatches:
        print("✓ Clean -- every name's compiled op matches its own source declaration.")
        return 0

    print(f"\n✗ {len(mismatches)} mismatch(es) -- likely a name collision silently")
    print("  skipped allocating the intended instruction:\n")
    for fn, lineno, label, detail in mismatches:
        print(f"  {fn}:{lineno}  {label}: {detail}")
    return 1


# ── dereference mismatch (was check_deref_mismatch.py) ────────────────────
def _deref_mismatch(ctx) -> int:
    """Find operands that need a symbol's own address but were given a
    dereferenced word instead (the _REF/Idem bug class)."""
    a = ctx.aether
    sym = ctx.symbols
    macro_names = set(wax_cmd_registrations(ldr=ctx.loader).keys())

    suspects = []

    for fn in ctx.re_files:
        for lineno, line in iter_source_lines(fn):
                rec = parse_source_line(line)
                if rec is None:
                    continue
                cmd, args, _slots = rec
                if cmd != 'JEQ' or len(args) < 4:
                    continue
                name, op2, op3 = args[0], args[1], args[2]
                # LOAD_CU_JZCK/LOAD_CU_JEQCK: known, already-verified
                # exception (compare an already-dereferenced register
                # against a target's word -- correct, see BUGS.md Pattern Z).
                if name in ('LOAD_CU_JZCK', 'LOAD_CU_JEQCK'):
                    continue
                for op in (op2, op3):
                    if op not in macro_names or op not in sym:
                        continue
                    addr = sym[op]
                    word = int(a[addr])
                    if word != addr:
                        other = op3 if op == op2 else op2
                        suspects.append(
                            (fn, lineno, line.strip(),
                             f"{op} is a macro name, not self-ref "
                             f"(compared against {other})"))

    for fn in ctx.re_files:
        for lineno, line in iter_source_lines(fn):
                stripped = line.strip()
                rec = parse_source_line(line)
                if rec is None:
                    continue
                cmd, args, slots = rec
                # Idem (Aspect #25) uses a non-dereferenced value by design.
                # The op can sit either in args (explicit `NOVA NAME Idem ...`)
                # or as the command word itself (block-body sugar, where the
                # implicit NOVA's op leads the line) -- check both.
                if cmd == 'IDEM' or 'Idem' in args:
                    continue
                for key in ('El1', 'El2'):
                    op = slots.get(key)
                    if op is None or op not in macro_names or op not in sym:
                        continue
                    if op.startswith('Idem:'):
                        continue
                    addr = sym[op]
                    word = int(a[addr])
                    if word != addr:
                        # Voca legitimately wants the dereferenced word --
                        # same two positions as Idem above.
                        if cmd == 'VOCA' or 'Voca' in args:
                            continue
                        suspects.append(
                            (fn, lineno, stripped,
                             f"{op} used as El1/El2, not self-ref"))

    print(f"Checked against {len(macro_names)} known WAX_CMD-registered names.")
    if not suspects:
        print("✓ Clean -- no dereference mismatch found.")
        return 0

    print(f"\n✗ {len(suspects)} suspect(s):\n")
    for fn, lineno, text, detail in suspects:
        print(f"  {fn}:{lineno}  {detail}\n    {text}")
    return 1


# ── NOVA sizing (was check_nova_sizing.py) ──────────────────────────────────
def _nova_sizing(ctx) -> int:
    """For every name that is the target of a NOVA line or any NOVA-naming
    macro (needs a full NOVA_SIZE=7 lux), verify its address ended up in
    _nova_addrs_set. Violations are the under-allocation bug class.

    NEEDS freeze: _nova_addrs_set / _discover_nova_naming_commands are Wave-1
    bookkeeping, absent from a fresh thaw."""
    l = ctx.loader
    R = ctx.symbols
    nova_set = l._nova_addrs_set
    naming_cmds = l._discover_nova_naming_commands()

    needs_nova_names = set()
    for fp in ctx.re_files:
        for _lineno, line in iter_source_lines(fp):
            rec = parse_source_line(line)
            if rec is None:
                continue
            cmd, args, _slots = rec
            if not args:
                continue
            if cmd == 'NOVA' or cmd in naming_cmds:
                needs_nova_names.add(args[0])

    print(f"Names that appear as NOVA/{'/'.join(sorted(naming_cmds))} targets "
          f"across all files: {len(needs_nova_names)}")

    violations = []
    for name in sorted(needs_nova_names):
        addr = R.get(name)
        if addr is None:
            continue
        if addr not in nova_set:
            violations.append((name, addr))

    print(f"\nVIOLATIONS (need full NOVA, but address NOT in _nova_addrs_set): "
          f"{len(violations)}")
    for name, addr in violations:
        print(f"  {name:30s} addr={addr}")
    return 1 if violations else 0


# ── save/own/lock integrity (was check_save_integrity.py) ─────────────────
def _save_integrity(ctx) -> int:
    """Verify SAVE's (and OWN/LOCK's) structural guarantee: every combo-
    blocked name sits inside its own combo's reserved range, and no two
    names collide on an address.

    NEEDS freeze: _name_regions and _combo_blocks are Wave-1 bookkeeping,
    never repopulated by a fresh thaw."""
    ldr = ctx.loader
    sym = ctx.symbols

    problems = []
    total_checked = 0
    addr_owner: dict = {}
    # Names carrying any placement region that reserves a combo block: own,
    # save or lock (the region model is _name_regions -> a set of region
    # strings per name; there is no separate _own_names/_save_names set).
    all_flagged = {n for n, regs in ldr._name_regions.items()
                   if regs & {'own', 'save', 'lock'}}
    for name in all_flagged:
        addr = sym.get(name)
        if addr is None:
            continue
        total_checked += 1
        key = ldr._combo_key(name)
        blk = ldr._combo_blocks.get(key)
        if blk is None:
            problems.append(f"{name}: combo {sorted(key)} has no reserved block at all")
        else:
            base, _next, end = blk
            if not (base <= addr < end):
                problems.append(
                    f"{name}: address {addr} is outside its {sorted(key)} block "
                    f"[{base}, {end})")
        if addr in addr_owner:
            problems.append(
                f"{name} and {addr_owner[addr]} share address {addr} "
                f"-- a combo block's whole guarantee is that this cannot happen")
        addr_owner[addr] = name

    print(f"Checked {total_checked} combo-blocked names "
          f"({len(ldr._combo_blocks)} distinct combo(s): "
          f"{[sorted(k) for k in ldr._combo_blocks]}) against a real build.")
    if not problems:
        print("✓ Clean -- every combo block's structural guarantee held.")
        return 0

    print(f"\n✗ {len(problems)} problem(s):\n")
    for p in problems:
        print(f"  {p}")
    return 1


# ── graph equivalence (was check_graph_equivalence.py) ────────────────────
_MASK64 = (1 << 64) - 1
_MASK32 = 0xFFFFFFFF
_SLOT_OP, _SLOT_E1, _SLOT_E2, _SLOT_EXIT = 1, 2, 3, 4


def _djb2(name: str) -> int:
    h = 5381
    for b in name.encode("utf-8"):
        h = ((h * 33) + b) & _MASK64
    return h


def _sk_lookup(mem, ht_base, ht_size, ht_mask, name):
    """Replicates htable.re's HT_LOOKUP exactly: hash finds the bucket, the
    stored name-pool text confirms the real match (not just the 32-bit
    hash -- see htable.re's own Pattern Z note)."""
    h = _djb2(name)
    h32 = h & _MASK32
    name_bytes = name.encode("utf-8")
    nlen = len(name_bytes)
    slot = h & ht_mask
    for _ in range(ht_size):
        addr = ht_base + slot * 2
        word0 = int(mem[addr])
        if word0 == 0:
            return None
        stored_hash = (word0 >> 32) & _MASK32
        if stored_hash == h32:
            word1 = int(mem[addr + 1])
            if word1:
                stored_len = int(mem[word1])
                if stored_len == nlen:
                    stored = bytes(int(mem[word1 + 1 + i]) & 0xFF for i in range(nlen))
                    if stored == name_bytes:
                        return word0 & _MASK32
        slot = (slot + 1) & ht_mask
    return None


def _graph_equivalence(ctx, args) -> int:
    """Compare the Python-built graph against the self-hosted LOAD_MAIN's
    own graph, by name: does the self-hosted SK_HTAB agree with Python's
    real one on every NOVA slot? Answers what 'N/42 files' cannot."""
    verbose = "--verbose" in args
    a = ctx.aether
    syms = ctx.symbols

    sk_ht_base_lux = syms.get("SK_HT_BASE")
    sk_ht_size_lux = syms.get("BS_HT_SIZE")
    sk_ht_mask_lux = syms.get("BS_HT_MASK")
    if not (sk_ht_base_lux and sk_ht_size_lux and sk_ht_mask_lux):
        print("graph-equivalence: cannot locate SK_HT_BASE/BS_HT_SIZE/"
              "BS_HT_MASK symbols — build too old or incompatible.")
        return 1
    sk_ht_base = int(a[sk_ht_base_lux])
    sk_ht_size = int(a[sk_ht_size_lux])
    sk_ht_mask = int(a[sk_ht_mask_lux])
    if not sk_ht_base:
        print("graph-equivalence: SK_HT_BASE is 0 — LOAD_MAIN's own htable "
              "was never initialized (self-hosting did not run).")
        return 1

    # SK_HT_BASE alone does not prove self-hosting actually ran: the base
    # address is assigned during init, while the table is populated later.
    # On a fresh frozen state the base is therefore non-zero over an
    # essentially empty table, and this check would sail past the guard
    # above and report every one of thousands of names as "not found" --
    # a 0-of-N result that looks like a catastrophic mismatch but in fact
    # verifies nothing at all. Judge by real occupancy instead.
    occupied = 0
    for _slot in range(sk_ht_size):
        if int(a[sk_ht_base + _slot * 2]):
            occupied += 1
            if occupied > 64:
                break
    if occupied <= 64:
        print(f"graph-equivalence: self-hosted SK_HTAB holds only {occupied} "
              f"entries — LOAD_MAIN's own symbol table was never populated "
              f"in this state, so there is nothing to compare against.")
        print("  This check needs a state in which the self-hosted build has "
              "actually run and interned its names; against a fresh frozen "
              "state it can only report that it had no data.")
        return 1

    py_rev: dict = {}
    for name, addr in syms.items():
        if isinstance(name, str) and addr and addr not in py_rev:
            py_rev[addr] = name

    matched: dict = {}
    unmatched: list = []
    for name in syms:
        if not isinstance(name, str):
            continue
        sh_addr = _sk_lookup(a, sk_ht_base, sk_ht_size, sk_ht_mask, name)
        if sh_addr:
            matched[name] = sh_addr
        else:
            unmatched.append(name)

    sh_rev = {addr: name for name, addr in matched.items()}

    print(f"Python-declared names:                {len(syms)}")
    print(f"Found in self-hosted SK_HTAB (by name): {len(matched)}")
    print(f"Not found in self-hosted SK_HTAB:       {len(unmatched)}")
    print()

    nova_compared = 0
    slot_mismatches = []
    unverifiable_slots = 0

    py_nova_names: set = set()
    for _fn in ctx.re_files:
        for _lineno, _line in iter_source_lines(_fn):
                _rec = parse_source_line(_line)
                if _rec is None:
                    continue
                _cmd, _args, _ = _rec
                # A named NOVA is `NOVA <name> Op=.. El1=..`: exactly one
                # positional arg (the name); operands are keyword slots. (The
                # old `>= 2` guard predates that syntax -- it expected
                # positional operands and so matched no modern NOVA at all,
                # silently making this whole check compare zero names.)
                if _cmd == 'NOVA' and len(_args) >= 1 and '{' not in _args[0]:
                    py_nova_names.add(_args[0])

    # Robust, honest comparison. For each slot, resolve Python's operand to a
    # NAME, then ask where THAT name lives in the self-hosted build
    # (matched[name]) and require the self-hosted slot to hold exactly that
    # address. This is layout-independent (the two builds use different
    # absolute addresses) yet immune to the reverse-map alias ambiguity of
    # comparing py_rev[pv] against an independent sh_rev[sv]: two names sharing
    # one address made the old form both miss real divergences (it skipped
    # every slot where the self-hosted value was 0 -- i.e. every unwired husk)
    # and, in principle, invent false ones. Per NOVA node we classify:
    #   correct  -- every verifiable slot holds the expected self-hosted addr
    #   wrong    -- some verifiable slot holds the wrong addr (but node is wired)
    #   husk     -- op/e1/e2/exit ALL zero while Python wired at least one:
    #               the self-hosted build interned the name but never wired it
    #   unverif  -- no slot was name-verifiable (all operands anonymous / not
    #               interned self-hosted): nothing to compare
    # plus, separately, `missing`: a Python NOVA name absent from SK_HTAB.
    SLOTS = (("Op", _SLOT_OP), ("El1", _SLOT_E1), ("El2", _SLOT_E2),
             ("Exit", _SLOT_EXIT), ("Next", SLOT_NEXT))
    _WIRE_SLOTS = (_SLOT_OP, _SLOT_E1, _SLOT_E2, _SLOT_EXIT)  # Next excluded: 0 == fall-through

    n_correct = n_wrong = n_husk = n_unverif = 0
    husk_names: list = []
    for name, sh_addr in matched.items():
        if name not in py_nova_names:
            continue
        py_addr = syms[name]
        nova_compared += 1

        # husk: Python wired something in op/e1/e2/exit, self-hosted wired none
        py_has = any(int(a[py_addr + o]) for o in _WIRE_SLOTS)
        sh_has = any(int(a[sh_addr + o]) for o in _WIRE_SLOTS)
        if py_has and not sh_has:
            n_husk += 1
            if len(husk_names) < 40:
                husk_names.append(name)
            continue

        node_verifiable = 0
        node_mism = []
        for slot_label, slot_off in SLOTS:
            py_val = int(a[py_addr + slot_off])
            sh_val = int(a[sh_addr + slot_off])
            if py_val == 0:
                # Python has no operand here. For Next, a self-hosted explicit
                # link vs Python's fall-through is not name-comparable; for the
                # real operand slots, py-empty/sh-set is a genuine divergence.
                if sh_val != 0 and slot_off != SLOT_NEXT:
                    node_verifiable += 1
                    node_mism.append(dict(name=name, slot=slot_label,
                                          py="(empty)", sh=sh_rev.get(sh_val) or sh_val))
                continue
            py_name = py_rev.get(py_val)
            if py_name is None:
                unverifiable_slots += 1
                continue
            exp = matched.get(py_name)
            if exp is None:
                unverifiable_slots += 1
                continue
            node_verifiable += 1
            if sh_val != exp:
                node_mism.append(dict(name=name, slot=slot_label,
                                      py=py_name, sh=sh_rev.get(sh_val) or sh_val))
        if node_mism:
            n_wrong += 1
            slot_mismatches.extend(node_mism)
        elif node_verifiable:
            n_correct += 1
        else:
            n_unverif += 1

    n_missing = sum(1 for n in py_nova_names if n not in matched)

    print(f"NOVA-kind names (Python ground truth): "
          f"{nova_compared} compared, {n_missing} missing from SK_HTAB")
    print(f"  correct={n_correct}  wrong={n_wrong}  husk(unwired)={n_husk}  "
          f"unverifiable={n_unverif}")
    print(f"  (skipped {unverifiable_slots} individual slots: operand anonymous "
          f"or not interned self-hosted)")
    print()

    print(f"[1] Slot mismatches on wired nodes (expected self-hosted addr not found): "
          f"{len(slot_mismatches)}")
    _by_slot: dict = {}
    for v in slot_mismatches:
        _by_slot[v['slot']] = _by_slot.get(v['slot'], 0) + 1
    print(f"    by slot: {_by_slot}")
    for v in slot_mismatches[:30]:
        print(f"    {v['name']}.{v['slot']}: python={v['py']} self-hosted={v['sh']}")
    if len(slot_mismatches) > 30:
        print(f"    ... and {len(slot_mismatches) - 30} more")
    print()

    if n_husk:
        print(f"[2] Husk nodes (interned self-hosted, never wired): {n_husk}")
        print(f"    e.g. {', '.join(husk_names[:20])}")
        print()

    if verbose and unmatched:
        print(f"Names not found in self-hosted SK_HTAB (first 40 of {len(unmatched)}):")
        for n in unmatched[:40]:
            print(f"    {n}")
        print()

    if n_wrong or n_husk or n_missing:
        print(f"✗ graph-equivalence: FAILED — {n_correct}/{nova_compared} named NOVA "
              f"nodes reproduced correctly by the self-hosted build "
              f"({n_husk} husk, {n_wrong} wrong, {n_missing} missing).")
        return 1
    print(f"✓ graph-equivalence: clean — all {n_correct} verifiable named NOVA nodes "
          "(Op/El1/El2/Exit/Next) reproduced identically by the self-hosted build. "
          "(Slots that are 0 / anonymous / not-yet-interned self-hosted are skipped "
          "as not name-comparable across the two layouts.)")
    return 0


# ── static lint (was diag.py mode_lint) ───────────────────────────────────
# Known Pattern-R subsystem name prefixes: live NOVAs from these subsystems
# reference Pattern-R phantoms as normal, expected control flow (built at
# runtime by Pattern-R ops LR/LT/EMIT/LH/..., not wired by Python Wave-B).
_PATTERN_R_SRC_PREFIXES = (
    'EB_', 'EJR_', 'EF_', 'EM_', 'ETH_', 'EXH_',
    'PA_', 'PGB_', 'RTB_', 'ND_', 'BFS_', 'FAL_',
    'CSP_', 'INIT_', 'RING_', 'RP_', 'RI_',
    'P0_', 'PS_', 'PFX_', 'LIM_', 'EMIT_',
)


def _is_autogen_jump_label(name: str) -> bool:
    """True for auto-generated JumpIf/SWITCH labels (_J/_K suffix, __sw_N_J)."""
    return (name.endswith('_J') or name.endswith('_K') or
            name.startswith('__sw_'))


def _is_pattern_r_source(name: str) -> bool:
    return (_is_autogen_jump_label(name) or
            any(name.startswith(p) for p in _PATTERN_R_SRC_PREFIXES))


def _lint(ctx, args) -> int:
    """Static lint: phantom NOVAs (op=0), dead EXIT/NEXT refs to phantoms,
    self-ref violations, and out-of-bounds operands.

    Findings are classified, not merely listed: Pattern-R phantoms (whose
    op-slot is filled only at runtime by LR/LT/EMIT/LH/WALK_ONE, never by
    Wave 2) are architecturally expected, counted, and suppressed by
    default. `--all` shows every suppressed entry as well, so "expected"
    is inspectable rather than taken on faith.

    Absorbed the former `graph` command: it re-detected op=0 and word≠self
    without this classification -- reporting 106 expected Pattern-R
    phantoms as issues -- and its only unique contribution was the bounds
    families below, now sections 4 and 5 here.
    """
    show_all = '--all' in args
    print("=== Reca Static Lint ===\n")
    ldr = ctx.loader
    a = ctx.aether
    R = ctx.symbols
    sym = ctx.rev

    nova_addrs = ldr._nova_addrs
    nova_set = set(nova_addrs)
    bump = ldr._bump

    # 1. Phantom NOVAs: op=0
    phantom = [addr for addr in nova_addrs if a[addr + SLOT_OP] == 0]
    phantom_set = set(phantom)
    phantom_sources = {addr: [] for addr in phantom}
    for addr in nova_addrs:
        if a[addr + SLOT_OP] == 0:
            continue
        src_name = sym.get(addr, '')
        for slot in [SLOT_EXIT, SLOT_NEXT]:
            tgt = int(a[addr + slot])
            if tgt in phantom_set:
                phantom_sources[tgt].append((addr, src_name))

    pattern_r_phantom = set()
    while True:
        prev_size = len(pattern_r_phantom)
        for addr in phantom:
            if addr in pattern_r_phantom:
                continue
            srcs = phantom_sources[addr]
            if not srcs:
                pattern_r_phantom.add(addr)  # unreachable = Pattern-R
                continue
            if all(_is_pattern_r_source(name) or src_addr in pattern_r_phantom
                   for src_addr, name in srcs):
                pattern_r_phantom.add(addr)
        if len(pattern_r_phantom) == prev_size:
            break
    real_phantom = phantom_set - pattern_r_phantom

    if phantom:
        n_r = len(pattern_r_phantom)
        n_real = len(real_phantom)
        if n_real:
            print(f"PHANTOM NOVAs (op=0) — {n_real} real + {n_r} Pattern-R (expected):")
            for addr in sorted(real_phantom)[:40]:
                print(f"  {sym.get(addr, f'#{addr}')} @ {addr}")
            if n_real > 40:
                print(f"  ... and {n_real-40} more")
        else:
            print(f"Phantom NOVAs: none unexpected ✓  ({n_r} Pattern-R expected, suppressed)")
        if show_all and pattern_r_phantom:
            print(f"  Pattern-R phantoms ({n_r}), op-slot filled at runtime:")
            for addr in sorted(pattern_r_phantom):
                print(f"    {sym.get(addr, f'#{addr}')} @ {addr}")
    else:
        print("Phantom NOVAs: none ✓")
    print()

    # 2. Dead EXIT/NEXT references
    dead_refs = []
    pattern_r_dead = []
    known_vals = {v for v in R.values()}
    for addr in nova_addrs:
        if a[addr + SLOT_OP] == 0:
            continue
        for slot, slot_name in [(SLOT_EXIT, 'EXIT'), (SLOT_NEXT, 'NEXT')]:
            target = a[addr + slot]
            if target == 0:
                continue
            if target < bump and target not in nova_set and target not in known_vals:
                continue
            if target in nova_set and a[target + SLOT_OP] == 0:
                src_name = sym.get(addr, f'#{addr}')
                tgt_name = sym.get(target, f'#{target}')
                if target in pattern_r_phantom:
                    pattern_r_dead.append((src_name, slot_name, tgt_name, addr, target))
                else:
                    dead_refs.append((src_name, slot_name, tgt_name, addr, target))

    if dead_refs:
        n_pr = len(pattern_r_dead)
        print(f"DEAD REFERENCES (→ phantom NOVA) — {len(dead_refs)} real"
              + (f" + {n_pr} Pattern-R (suppressed)" if n_pr else "") + ":")
        for src, slot, tgt, src_addr, tgt_addr in sorted(dead_refs, key=lambda x: x[0])[:40]:
            print(f"  {src}.{slot} → {tgt} (op=0)")
        if len(dead_refs) > 40:
            print(f"  ... and {len(dead_refs)-40} more")
    elif pattern_r_dead:
        print(f"Dead references: none unexpected ✓  ({len(pattern_r_dead)} Pattern-R expected, suppressed)")
    else:
        print("Dead references: none ✓")
    if show_all and pattern_r_dead:
        print(f"  Pattern-R dead refs ({len(pattern_r_dead)}), target built at runtime:")
        for src, slot, tgt, src_addr, tgt_addr in sorted(pattern_r_dead, key=lambda x: x[0]):
            print(f"    {src}.{slot} → {tgt} (op=0)")
    print()

    # 3. Self-ref violations
    known_addrs = set(R.values())
    bad_selfref = [
        addr for addr in nova_addrs
        if a[addr] != addr and a[addr] not in known_addrs and a[addr] != 0
    ]
    if bad_selfref:
        print(f"SELF-REF VIOLATIONS — {len(bad_selfref)} found:")
        for addr in sorted(bad_selfref)[:20]:
            print(f"  {sym.get(addr, f'#{addr}')} @ {addr}: word={a[addr]} (expected {addr})")
        if len(bad_selfref) > 20:
            print(f"  ... and {len(bad_selfref)-20} more")
    else:
        print("Self-ref: all NOVA addrs ✓")
    print()

    # 4. NOVA operand bounds. A slot holding an address at or above the bump
    # points into never-allocated space: nothing ever wrote there, so the
    # value is garbage by construction. No classification needed and no
    # false positives possible -- unlike op=0, which is legitimately filled
    # later at runtime, an out-of-bounds address is never expected.
    oob = []
    for addr in nova_addrs:
        op = a[addr + SLOT_OP]
        if op >= bump:
            oob.append(f"{sym.get(addr, f'#{addr}')} @ {addr}: op={op} past bump {bump}")
        for slot, sn in [(SLOT_E1, 'e1'), (SLOT_E2, 'e2'),
                         (SLOT_EXIT, 'exit'), (SLOT_NEXT, 'next')]:
            v = a[addr + slot]
            if v and v >= bump:
                oob.append(f"{sym.get(addr, f'#{addr}')} @ {addr}: {sn}={v} past bump {bump}")
    if oob:
        print(f"OUT-OF-BOUNDS NOVA OPERANDS — {len(oob)} found:")
        for line in oob[:40]:
            print(f"  {line}")
        if len(oob) > 40:
            print(f"  ... and {len(oob)-40} more")
    else:
        print("NOVA operand bounds: all within bump ✓")
    print()

    # 5. Data lumen bounds. Walks each data lux's [rel, tgt] pairs. Bounded
    # against the whole aether rather than the bump: a data node may legally
    # hold a runtime pointer above the bump (K_CURSOR stores the bump itself),
    # so only a value past the aether's own end is unambiguously corrupt.
    data_oob = []
    for addr in ldr._data_addrs:
        if addr in ldr._nova_addrs_set:
            continue
        i = 1
        while addr + i < bump:
            rel = a[addr + i]
            if rel == 0:
                break
            tgt = a[addr + i + 1]
            if rel >= AETHER_SIZE:
                data_oob.append(f"{sym.get(addr, f'#{addr}')} @ {addr}+{i}: rel={rel} past aether end")
            if tgt >= AETHER_SIZE:
                data_oob.append(f"{sym.get(addr, f'#{addr}')} @ {addr}+{i}: tgt={tgt} past aether end")
            i += LUMEN_PAIR
    if data_oob:
        print(f"OUT-OF-BOUNDS DATA LUMINA — {len(data_oob)} found:")
        for line in data_oob[:40]:
            print(f"  {line}")
        if len(data_oob) > 40:
            print(f"  ... and {len(data_oob)-40} more")
    else:
        print("Data lumen bounds: all within aether ✓")
    print()

    total_real = (len(real_phantom) + len(dead_refs) + len(bad_selfref)
                  + len(oob) + len(data_oob))
    total_pr = len(pattern_r_phantom) + len(pattern_r_dead)
    if total_real == 0:
        if total_pr:
            print(f"✓ Lint clean  ({total_pr} Pattern-R phantoms/dead-refs "
                  f"suppressed as expected"
                  + ("" if show_all else "; --all to list them") + ")")
        else:
            print("✓ Lint clean — no issues found.")
        return 0
    msg = (f"✗ {total_real} issues ({len(real_phantom)} phantom, "
           f"{len(dead_refs)} dead refs, {len(bad_selfref)} self-ref violations, "
           f"{len(oob)} OOB operands, {len(data_oob)} OOB lumina)")
    if total_pr:
        msg += f"  +{total_pr} Pattern-R expected (suppressed)"
    print(msg)
    return 1


# ── freeze health (was diag.py mode_health) ───────────────────────────────
def _health(ctx) -> int:
    """Freeze health overview: symbol/lux counts, Pass2 errors, critical
    symbol presence, aspect self-ref, key register values."""
    print("=== Reca Freeze Health ===\n")
    ldr = ctx.loader
    a = ctx.aether
    R = ctx.symbols
    sym = ctx.rev
    errs = ctx.errs

    print(f"Symbols:    {len(R)}")
    print(f"Bump:       {ldr._bump}")
    print(f"Watermark:  {ldr._water}")
    print(f"NOVA luces:  {len(ldr._nova_addrs)}")
    print(f"Data luces: {len(ldr._data_addrs) - len(ldr._nova_addrs)}")
    print()

    if errs:
        print(f"PASS2 ERRORS ({len(errs)}):")
        for e in errs[:25]:
            print(f"  {e}")
        if len(errs) > 25:
            print(f"  ... and {len(errs)-25} more")
    else:
        print("Pass2: clean \u2713")
    print()

    critical = [
        'Move', 'Jump', 'JumpIf', 'Voca', 'Redi', 'Read', 'Write',
        'Add', 'Sub', 'Equal', 'Less', 'End', 'Exire', 'Not', 'Greater',
        'P0_NID', 'RA_LINK', 'K_CURSOR', 'SC_NR', 'SC_A0',
        'EMIT_STR_ENTRY', 'PUT_BYTE', 'PB_FLUSH',
        'ALLOC_LUX', 'ALLOC', 'HT_INSERT', 'HT_LOOKUP',
        'SCAN_ALL_LUX', 'SR_FIRST_LM', 'SR_NEXT_LM',
    ]
    missing = [s for s in critical if s not in R]
    if missing:
        print(f"MISSING CRITICAL ({len(missing)}): {missing}")
    else:
        print(f"Critical symbols ({len(critical)}): all present \u2713")
    print()

    nova_sym = a[R['NOVA_SIZE']] if 'NOVA_SIZE' in R else None
    if nova_sym is not None:
        tag = "\u2713" if nova_sym == NOVA_SIZE else "\u2717 MISMATCH"
        print(f"NOVA_SIZE: aether.py={NOVA_SIZE}, constants.re={nova_sym}  {tag}")
    print("Canon: aether[0]=0  " + ("\u2713" if a[0] == 0 else "\u2717 VIOLATION"))
    print()

    aspects = [
        'Read', 'Write', 'Add', 'Sub', 'Mul', 'Div', 'Rem', 'UDiv', 'URem',
        'And', 'Or', 'Xor', 'Left', 'Right', 'ARight', 'Equal', 'Less', 'ULess',
        'JumpIf', 'JumpReg', 'End', 'Exire', 'Voca', 'Redi',
        'Move', 'Jump', 'Load', 'Store', 'Not', 'Greater',
    ]
    bad = [f'{asp}@{R[asp]}:word={a[R[asp]+SLOT_WORD]}'
           for asp in aspects if asp in R and a[R[asp]+SLOT_WORD] != R[asp]]
    if bad:
        print(f"BAD SELF-REF ({len(bad)}):")
        for b in bad:
            print(f"  {b}")
    else:
        present = sum(1 for x in aspects if x in R)
        print(f"Self-ref for {present} aspects \u2713")
    print()

    key_regs = ['RA_LINK', 'RA_OB_POS', 'RA_INSTR', 'RA_SSA_CTR',
                'RA_RULE', 'RA_NXT_N', 'RA_SR_OUT']
    print("Key registers:")
    for rn in key_regs:
        if rn in R:
            addr = R[rn]
            val = a[addr]
            print(f"  {rn:15s} addr={addr}  word={val} ({sym.get(val, str(val))})")
    print()
    return 0


# ── broken infrastructure luces (was diag.py _mode_broken) ────────────────
def _broken(ctx) -> int:
    """Primitive self-ref check + next=0/op=0 check on key LOAD_MAIN symbols."""
    ldr = ctx.loader
    a = ctx.aether
    R = ctx.symbols
    prims = ['Equal', 'Read', 'Write', 'Add', 'Sub', 'Mul', 'Move', 'JumpIf',
             'Jump', 'Voca', 'Redi', 'Less', 'ULess', 'End', 'Exire']
    print("Primitive self-ref check:")
    for n in prims:
        addr = R.get(n, 0)
        if not addr:
            continue
        word = int(a[addr])
        ok = "✓" if word == addr else f"✗ word={word} expected {addr}"
        print(f"  {n:12s} @{addr}: {ok}")
    print("\nNext=0 check on key LOAD_MAIN symbols:")
    suspects = [k for k in R if k.startswith('LOAD_') or k.startswith('BS_') or k.startswith('SK_')]
    broken = []
    for sname in suspects:
        addr = R[sname]
        op = int(a[addr + SLOT_OP])
        if addr in ldr._nova_addrs_set and op == 0:
            broken.append((sname, addr))
    if broken:
        print(f"  Found {len(broken)} NOVA luces with op=0 (unset):")
        for sname, addr in broken[:20]:
            print(f"    {sname} @{addr}")
        return 1
    print("  None found ✓")
    return 0


# ── invariants (was diag.py mode_invariants) ──────────────────────────────
def _invariants(ctx) -> int:
    """Thirteen families of build invariant: ASCII constants, C_N values and
    address formula, critical NOVA wiring, macro WAX_CMD wiring, register
    sanity, lexer buffers, file-list integrity, aspect self-ref, dispatch
    wiring, live self-hosting depth, PRECISE fallthrough-hijack, and a
    NOLINK-after-call structural scan.

    Returns the FAIL count (0 = clean). WARNs are reported but do not fail:
    they are suspicious-but-possibly-intentional, e.g. the known C_N formula
    gaps caused by symbols inserted between consecutive C_N."""
    import os as _os

    a = ctx.aether
    R = ctx.symbols
    sym = ctx.rev

    fails = []
    warns = []
    oks = []

    def ok(msg):
        oks.append(msg)

    def warn(msg):
        warns.append(f'WARN  {msg}')

    def fail(msg):
        fails.append(f'FAIL  {msg}')

    def sym_val(name):
        addr = R.get(name, 0)
        return int(a[addr]) if addr else None

    def nova_slot(name, slot):
        addr = R.get(name, 0)
        return int(a[addr + slot]) if addr else None

    print('=== Reca Invariants ===\n')

    # 1. ASCII character constants
    ascii_expected = {
        'NUL': 0, 'TAB': 9, 'LF': 10, 'CR': 13, 'ESC': 27, 'SP': 32, 'DEL': 127,
        'MINUS': 45, 'PERCENT': 37, 'SLASH': 47, 'PERIOD': 46,
        'COLON': 58, 'SEMICOLON': 59, 'EQUALS': 61, 'QUESTION': 63,
        'LBRACE': 123, 'RBRACE': 125, 'LBRACKET': 91, 'RBRACKET': 93,
        'PIPE': 124, 'BACKSLASH': 92, 'TILDE': 126, 'UNDERSCORE': 95,
        'BACKTICK': 96, 'HASH': 35, 'AMP': 38, 'BANG': 33,
        'ASCII_0': 48, 'ASCII_9': 57,
        'ASCII_A': 65, 'ASCII_Z': 90, 'ASCII_a': 97, 'ASCII_z': 122,
    }
    print('1. ASCII character constants:')
    for name, expected in sorted(ascii_expected.items()):
        if name not in R:
            continue
        v = sym_val(name)
        if v == expected:
            ok(f'{name}={v}')
        else:
            fail(f'{name}: expected {expected}, got {v}')

    # 2. C_N numeric constants (word == N)
    print('\n2. C_N numeric constants (word == N):')
    c_constants = {}
    for sym_name, addr in sorted(R.items()):
        m = re.match(r'^C_(\d+)$', sym_name)
        if m:
            n = int(m.group(1))
            v = int(a[addr])
            c_constants[n] = (sym_name, addr, v)
            if v == n:
                ok(f'{sym_name}@{addr}={v}')
            else:
                fail(f'{sym_name}@{addr}: expected {n}, got {v}')

    cneg = R.get('C_NEG1', 0)
    if cneg:
        v = int(a[cneg])
        expected_neg1 = (1 << 64) - 1
        if v == expected_neg1:
            ok(f'C_NEG1@{cneg}=0xFFFF...FFFF')
        else:
            fail(f'C_NEG1@{cneg}: expected -1 (u64), got {v}')

    # 3. C_N layout
    #
    # This used to assert addr(C_N) == addr(C_0) + 2*N, and warned 58 times
    # on every run. The formula cannot hold: it assumes the constants form a
    # dense array indexed by their *value*, while the declared set is sparse
    # -- C_0..C_19, then C_32, C_33, C_48, C_63, C_64, C_100, C_255, C_256,
    # C_512, C_1024, C_5381. They are laid out consecutively in declaration
    # order, which is a different thing entirely.
    #
    # Nothing depended on the formula either: no source computes a constant's
    # address arithmetically from C_0, and RA_C0_REF -- the register that
    # would have held the base for such arithmetic -- is not in the build at
    # all, surviving only in one comment. The same is true of C_7_REF, whose
    # two checks here tested a symbol constants.re removed ("C_7_REF formerly
    # declared here -- removed once NOVAE gained its own"); both were dead
    # code guarded by `if c7ref`, quietly skipped on every run.
    #
    # What is worth checking is what the layout actually promises: the
    # constants sit consecutively, two luces apart, in declaration order.
    # A name inserted between two of them would break that, and unlike the
    # old formula this statement is true today, so a warning from it means
    # something.
    # There is deliberately no C_N *layout* check here.
    #
    # This section used to assert addr(C_N) == addr(C_0) + 2*N and warned 58
    # times on every run. That formula cannot hold: it assumes the constants
    # form a dense array indexed by their value, while the declared set is
    # sparse -- C_0..C_19, then C_32, C_33, C_48, C_63, C_64, C_100, C_255,
    # C_256, C_512, C_1024, C_5381. Nothing depended on it either: no source
    # computes a constant's address arithmetically, and RA_C0_REF, the
    # register that would have held the base for such arithmetic, is not in
    # the build at all. The C_7_REF checks that sat here tested a symbol
    # constants.re had already removed, and were skipped silently every run.
    #
    # Replacing it with "consecutive, stride 2" was tried and is also false:
    # lazy resolve mints a constant the moment any earlier-loading file
    # references it, so C_2 sits at 5061 beside SL_EOL_TOGGLE, both created
    # by `JEQ SL_EOL_MCK SL_MARK C_2 SL_EOL_TOGGLE` in comments.re, which
    # loads before constants.re. Any constant can be displaced that way and
    # nothing is wrong when it is.
    #
    # What a C_N constant actually promises is its *value*, which section 2
    # above checks. Where it lives is not promised by anything.


    # 4. Critical NOVA El field wiring
    print('\n4. Critical NOVA El field wiring:')
    nova_checks = [
        # SET's integer path lives in commands.re (PS_CMD_SET / PS_ST_WR);
        # wax.re's LOAD_CS_WINT was the second implementation and is gone. Same
        # guarantee, checked on the surviving one: the parsed value is
        # written to the named lux, not to an MA* slot. PS_ST_INT reads the
        # integer out of its constant cell into SK_TMP, and PS_ST_WR writes
        # SK_TMP -- so El2 is SK_TMP (the value), not RA_MA1. (The old PR_VAL
        # register this once named was retired -- unused; SK_TMP is the path.)
        ('PS_ST_WR', SLOT_E1, 'PR_EL1',
         'dest: the named lux (NOT RA_MA0 -- SET reads via PS_CMD_SET, not MA*)'),
        ('PS_ST_WR', SLOT_E2, 'SK_TMP',
         'value: parsed integer read into SK_TMP by PS_ST_INT (NOT RA_MA1)'),
    ]
    for nova_name, slot, expected_sym, reason in nova_checks:
        if nova_name not in R:
            warn(f'{nova_name}: not found')
            continue
        actual_addr = nova_slot(nova_name, slot)
        expected_addr = R.get(expected_sym, 0)
        actual_sym = sym.get(actual_addr, f'#{actual_addr}')
        slot_name = {SLOT_E1: 'El1', SLOT_E2: 'El2', SLOT_EXIT: 'Exit'}.get(slot, f's{slot}')
        if actual_addr == expected_addr:
            ok(f'{nova_name}.{slot_name} = {expected_sym} ({reason})')
        else:
            fail(f'{nova_name}.{slot_name}: expected {expected_sym}@{expected_addr}, '
                 f'got {actual_sym}@{actual_addr}  [{reason}]')

    # 5. Macro WAX_CMD wiring (word -> entry NOVA)
    print('\n5. Macro WAX_CMD wiring (word → entry NOVA):')
    macro_seals = wax_cmd_registrations()
    for macro_name, entry_name in sorted(macro_seals.items()):
        if macro_name not in R:
            continue
        macro_addr = R[macro_name]
        word = int(a[macro_addr])
        entry_addr = R.get(entry_name, 0)
        if not entry_addr:
            warn(f'{macro_name}: entry {entry_name} not found')
        elif word == entry_addr:
            ok(f'{macro_name}.word → {entry_name}@{entry_addr} ✓')
        else:
            fail(f'{macro_name}.word = {sym.get(word, f"#{word}")}@{word}, '
                 f'expected {entry_name}@{entry_addr}')

    # 6. Key register values after freeze
    print('\n6. Key register values after freeze:')
    # Registers whose zero means "the run finished cleanly": the call stack
    # unwound (RA_LINK), input reached EOF (RA_LOAD_BYTE/TLEN/RPOS/RLEN),
    # redirect mode is off (RA_REDIRECT_BASE), every indented block closed
    # (SK_IND_DEPTH). A non-zero one of these is worth looking at.
    #
    # Deliberately NOT listed: RA_REDIRECT_POS/LEN, RA_BS_EL0, RA_BS_RESULT,
    # RA_BS_FLAG, RA_MA1/MA2, SK_FIDX. Those are working registers that keep
    # their last value by design -- SK_FIDX ends at the file count (42),
    # RA_BS_RESULT holds the last interned lux, RA_MA1/MA2 the last macro's
    # arguments. They were in this list and warned on every single run with
    # "may be OK", which is a check admitting it cannot decide. Eight
    # permanent warnings hid the ones that mean something.
    regs_expected_zero = [
        'RA_LINK', 'RA_LOAD_BYTE', 'RA_LOAD_TLEN', 'RA_LOAD_RPOS', 'RA_LOAD_RLEN',
        'RA_REDIRECT_BASE', 'RA_BS_PIVAL', 'RA_MA0', 'RA_MA3', 'SK_IND_DEPTH',
    ]
    for rn in regs_expected_zero:
        if rn not in R:
            continue
        v = int(a[R[rn]])
        if v == 0:
            ok(f'{rn}=0')
        else:
            warn(f'{rn} = {v} (non-zero after freeze; may be OK)')

    # RA_C0_REF was listed here too, checked against addr(C_0). It is not in
    # the build -- it survives only in one comment in registers.re -- so the
    # entry never ran. Removed rather than left as a silent no-op.
    regs_known_nonzero = {
        'RA_SP': None,
        'LF': 10, 'TAB': 9, 'CR': 13, 'SP': 32, 'NUL': 0,
    }
    for rn, expected in regs_known_nonzero.items():
        if rn not in R:
            continue
        v = int(a[R[rn]])
        if expected is None:
            ok(f'{rn}={v} (non-zero expected ✓)')
        elif v == expected:
            ok(f'{rn}={v} ✓')
        else:
            fail(f'{rn}: expected {expected}, got {v}')

    # 7. Lexer buffer constants
    print('\n7. Lexer buffer constants:')
    buf_checks = {
        'BS_READBUF_SIZE': (lambda v: 64 <= v <= 65536, '64..65536'),
        'BS_READBUF_BASE': (lambda v: v > 0, '>0'),
        'BS_TOKBUF_BASE': (lambda v: v > 0, '>0'),
    }
    for bname, (pred, desc) in buf_checks.items():
        v = sym_val(bname)
        if v is None:
            warn(f'{bname}: not found')
        elif pred(v):
            ok(f'{bname}={v} ({desc} ✓)')
        else:
            fail(f'{bname}={v}: expected {desc}')

    # 8. File list integrity
    print('\n8. File list integrity:')
    bs_file_count = sym_val('BS_FILE_COUNT')
    bs_file_list = sym_val('BS_FILE_LIST')

    from loader import re_sources
    re_files = re_sources()
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

    # 9. Self-ref check (word == addr for data luces)
    print('\n9. Self-ref check (word == addr for data luces):')
    aspects = ['Read', 'Write', 'Add', 'Sub', 'Mul', 'Div', 'Rem',
               'And', 'Or', 'Xor', 'Left', 'Right', 'Equal', 'Less', 'ULess',
               'JumpIf', 'JumpReg', 'End', 'Exire', 'Voca', 'Redi',
               'Move', 'Jump', 'Not', 'Greater']
    asp_fails = []
    for asp in aspects:
        if asp not in R:
            continue
        addr = R[asp]
        word = int(a[addr])
        if word != addr:
            asp_fails.append(f'{asp}@{addr}:word={word}')
    if asp_fails:
        for f in asp_fails:
            warn(f'Aspect self-ref modified (LOAD_MAIN side effect?): {f}')
    else:
        ok(f'All {len([x for x in aspects if x in R])} aspects self-ref ✓')

    # 10. LOAD_MAIN dispatch wiring
    print('\n10. LOAD_MAIN dispatch wiring:')
    dispatch_checks = [
        # The first-byte SWITCH is gone: LOAD_DISPATCH_CORE looks the whole
        # token up and jumps to whatever the table says, the same way
        # parser.re does. The old entries here checked that byte dispatch's
        # internals (second-byte read, length-based fallthrough) and no
        # longer describe anything. What has to hold now is that the
        # lookup's result is what dispatch actually uses.
        ('LOAD_DC_SAVE', SLOT_E1, 'RA_BS_RESULT',
         'dispatch uses the looked-up handler address, not a re-derived guess'),
    ]
    for nova_name, slot, expected_ref, reason in dispatch_checks:
        if nova_name not in R:
            warn(f'{nova_name}: not found')
            continue
        if slot is not None:
            actual = nova_slot(nova_name, slot)
            expected = R.get(expected_ref, 0)
            slot_n = {SLOT_E1: 'El1', SLOT_E2: 'El2'}.get(slot, str(slot))
            if actual == expected:
                ok(f'{nova_name}.{slot_n} = {expected_ref} ✓  [{reason}]')
            else:
                fail(f'{nova_name}.{slot_n}: expected {expected_ref}@{expected}, '
                     f'got {sym.get(actual, f"#{actual}")}@{actual}  [{reason}]')

    # 11. Self-hosting processing depth (live traced run)
    print('\n11. Self-hosting processing depth:')
    try:
        from trace import make_patched_run as _mpr

        ldc_addr = R.get('LOAD_DISPATCH_CORE', 0)
        ldc_target = int(a[ldc_addr]) if ldc_addr else 0
        ldc_sym = sym.get(ldc_target, 'LOAD_DC_TLENCK')

        dispatch_counts = {}

        def _on_voca(ev):
            fi = ev['fi']
            dispatch_counts[fi] = dispatch_counts.get(fi, 0) + 1

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

    # 12. PRECISE fallthrough-hijack (folded in from --fallthrough-hijack --
    # see BUGS.md Pattern Z: 12 confirmed instances of this exact bug class
    # this session alone). WARN, not FAIL: a real hit is a strong signal,
    # but this scan can't rule out an intentional case, so a human should
    # look rather than have the build blocked outright.
    print('\n12. PRECISE fallthrough-hijack:')
    try:
        precise_hits = _scan_precise_fallthrough(ctx)
        if precise_hits:
            for name, faddr, landed_name, laddr, p1, p2 in precise_hits:
                loc1 = f"{_os.path.basename(p1[0])}:{p1[1]}" if p1 else "?"
                loc2 = f"{_os.path.basename(p2[0])}:{p2[1]}" if p2 else "?"
                warn(f'{name}@{faddr} ({loc1}) falls through to '
                     f'{landed_name}@{laddr} ({loc2}), separately '
                     f'RVOCA-called elsewhere too')
        else:
            ok('0 precise fallthrough-hijack candidates')
    except Exception as exc:
        warn(f'PRECISE fallthrough-hijack scan failed: {exc}')

    # 13. NOLINK immediately after an RVOCA/RREDI call (source-text
    # pattern only -- see _scan_nolink_after_call's own docstring).
    # Informational: not proof of a bug, just the exact shape that
    # produced 8 of this session's 12 Pattern Z instances, worth a human
    # glance every time rather than only when someone remembers to look.
    print('\n13. NOLINK immediately after RVOCA/RREDI (informational):')
    try:
        nolink_hits = _scan_nolink_after_call(ctx)
        if nolink_hits:
            for fp, lineno, line_text in nolink_hits:
                print(f'  {_os.path.basename(fp)}:{lineno}  {line_text}')
        else:
            print('  none found')
    except Exception as exc:
        print(f'  scan failed: {exc}')

    # Summary
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


# ── string layout (was diag.py mode_strings) ──────────────────────────────
def _strings(ctx) -> int:
    """String encoding layout: packed 8-bytes-per-lux chains for SF_* names."""
    print("=== String Encoding Layout ===\n")
    a = ctx.aether
    R = ctx.symbols
    print("Model: 8 bytes packed/lux (little-endian u64), linked list.")
    print("OB: 1 byte/lux (SPAN), SYS_WRITE_PACKED reads 8 bytes/lux.\n")
    for name in [k for k in R if k.startswith('SF_')][:12]:
        addr = R[name]
        first = a[addr]
        if not first:
            print(f"  {name}: (empty)")
            continue
        decoded = read_packed_string(a, first)
        # Count chunks along the same [word, next] chain read_packed_string
        # walks. Bounded: an out-of-range or self-referential `next` would
        # otherwise index past the aether (a real crash the old --strings
        # hit on every run) or spin forever.
        chunks = 0
        cur = first
        while cur and 0 < cur + 1 < len(a) and chunks < 4096:
            chunks += 1
            cur = int(a[cur+1])
        print(f"  {name}: {len(decoded)}ch {chunks}chunk  {decoded[:60]!r}")
    print()
    if 'OB_START' in R and 'OB_SIZE' in R:
        print(f"OB: SPAN@{R['OB_START']}, {a[R['OB_SIZE']]} luces")
    return 0


# ── lux wiring dump (was diag.py _mode_wiring) ────────────────────────────
def _wiring(ctx, args) -> int:
    """Print full lux wiring (word/op/e1/e2/exit/next) for named symbols."""
    a = ctx.aether
    R = ctx.symbols
    sym = ctx.rev
    sym_names = [x for x in args if not x.startswith('-')]
    if not sym_names:
        print("Usage: python3 iris.py --wiring SYM1 [SYM2 ...]")
        return 0
    for sname in sym_names:
        addr = R.get(sname, 0)
        if not addr:
            print(f"  {sname}: NOT FOUND in symbols")
            continue
        word = int(a[addr])
        op = int(a[addr+SLOT_OP])
        e1v = int(a[addr+SLOT_E1])
        e2v = int(a[addr+SLOT_E2])
        exv = int(a[addr+SLOT_EXIT])
        nxv = int(a[addr+SLOT_NEXT])
        self_ok = "✓" if word == addr else "✗ BAD(word≠addr)"
        opn = sym.get(op, f'#{op}')
        e1n = sym.get(e1v, e1v)
        e2n = sym.get(e2v, e2v)
        exn = sym.get(exv, exv)
        nxn = '0(implicit)' if nxv == 0 else sym.get(nxv, nxv)
        print(f"  {sname:30s}  @{addr}  word={word} {self_ok}")
        print(f"    op={opn}  e1={e1n}  e2={e2n}")
        print(f"    exit={exn}  next={nxn}")
    return 0


# ── runtime htable lookup (was diag.py --htable) ──────────────────────────
def _htable(ctx, args) -> int:
    """Look up a name in the self-hosted runtime hash table (SK_HTAB).

    Reuses _sk_lookup, the exact replica of htable.re's own HT_LOOKUP.
    The previous implementation hashed with a XOR-variant DJB2 and indexed
    with `h % size` at stride 1, while htable.re uses additive DJB2 with
    `h & mask` at stride 2 and confirms the hit against the stored name
    text -- so it computed a different bucket than the table it was reading
    and reported every name as absent (BUGS.md records --htable as broken).
    Sharing the one verified replica removes the second, drifting copy.
    """
    name_to_find = next((x for x in args if not x.startswith('-')), None)
    if not name_to_find:
        print("Usage: python3 iris.py --htable NAME")
        return 0
    a = ctx.aether
    R = ctx.symbols
    sym = ctx.rev

    ht_base_lux = R.get('SK_HT_BASE')
    ht_size_lux = R.get('BS_HT_SIZE')
    ht_mask_lux = R.get('BS_HT_MASK')
    if not (ht_base_lux and ht_size_lux and ht_mask_lux):
        print("  SK_HT_BASE/BS_HT_SIZE/BS_HT_MASK not found — build too old.")
        return 1
    ht_base = int(a[ht_base_lux])
    ht_size = int(a[ht_size_lux])
    ht_mask = int(a[ht_mask_lux])
    if not ht_base:
        print("  SK_HT_BASE is 0 — the self-hosted htable was never "
              "initialized (LOAD_MAIN did not run).")
        return 1

    h = _djb2(name_to_find)
    print(f"  Name: {name_to_find!r}")
    print(f"  DJB2 hash: {h}  bucket: {h & ht_mask}  (table @{ht_base}, "
          f"size {ht_size}, mask {ht_mask})")

    found_addr = _sk_lookup(a, ht_base, ht_size, ht_mask, name_to_find)
    if found_addr is None:
        print(f"  Not present in the self-hosted htable.")
        py_addr = R.get(name_to_find)
        if py_addr:
            print(f"  (Python's own build has it at {py_addr} — so the "
                  f"self-hosted side never registered this name.)")
        return 0

    print(f"  Found: addr={found_addr} ({sym.get(found_addr, '?')})")
    py_addr = R.get(name_to_find)
    if py_addr:
        match = "✓" if py_addr == found_addr else "✗ MISMATCH"
        print(f"  Python's addr: {py_addr}   {match}")
    return 0


# ── lazy stubs never filled in ────────────────────────────────────────────
def _stubs(ctx) -> int:
    """Report names that exist only because something referenced them.

    Lazy resolve mints a correctly-sized lux whenever a name is used before
    it is declared. That is a capability, not a failure: the graph builds
    itself, so a name may legitimately appear later, or be produced at
    runtime -- and Reca's own BS_INTERN_NOVA has always worked this way.
    Removing it would make this loader less capable than the language it
    describes.

    What the capability costs is visibility: a genuinely missing
    declaration takes exactly the shape of an ordinary forward reference.
    This check pays that cost back. Every lazily-minted name is recorded
    (loader._lazy_stubs); the ones whose lux is still untouched at the end
    of the build -- word pointing at itself, every other slot zero -- are
    checked against the project's real declarations (_scan_defs_refs, the
    same source `--deps` uses) before being reported: content shape alone
    cannot tell an unfilled stub from a name whose correct, final content
    IS exactly self-ref-plus-zero by design (e.g. Idem, declared via a bare
    `SET Name=Idem Idem` -- SET's own canonical self-ref form for aspects).
    Two names with identical bit patterns can have entirely different
    histories; only cross-checking against the source's own declarations
    tells them apart. A name genuinely declared somewhere is never a
    finding here, regardless of what its content looks like.

    A finding here is not proof of a bug: a name whose content arrives at
    runtime is expected to look exactly like this. It is a list to read,
    which is why the check reports rather than fails.
    """
    ldr = ctx.loader
    a = ctx.aether
    stubs = getattr(ldr, '_lazy_stubs', set())

    # Project-wide "really declared somewhere" set — same scan `--deps`
    # already relies on, reused here rather than re-derived, so both
    # commands agree on what counts as a real declaration.
    from loader import _read_and_parse as _rap
    naming_commands = ldr._discover_nova_naming_commands()
    all_defs = set()
    for fp in ctx.re_files:
        _lines, records = _rap(fp, ldr)
        defs, _refs = ldr._scan_defs_refs(records, naming_commands)
        all_defs |= defs

    empty, filled = [], []
    for name in stubs:
        addr = ldr.symbols.get(name)
        if addr is None:
            continue
        if name in all_defs:
            filled.append(name)  # genuinely declared -- shape is irrelevant
            continue
        word = int(a[addr])
        rest = [int(a[addr + i]) for i in range(1, NOVA_SIZE)]
        (empty if (word == addr and not any(rest)) else filled).append(name)

    print(f"Lazily created names: {len(stubs)}")
    print(f"  filled in later:    {len(filled)}   (ordinary forward references)")
    print(f"  never filled:       {len(empty)}")
    if not empty:
        print("\n✓ Every lazily-created name received content from a real "
              "declaration.")
        return 0
    print("\nNames referenced but never declared by anything -- each is "
          "either built at runtime, or a declaration that went missing:\n")
    for name in sorted(empty):
        print(f"  {name}  @{ldr.symbols[name]}")
    return 0


def _scan_precise_fallthrough(ctx):
    """Shared PRECISE-pass logic: a NOVA with SLOT_NEXT=0 (relies on
    fallthrough) whose landed-on address is itself an RVOCA target, far
    from the fallthrough site in the SOURCE file. Used by both
    --fallthrough-hijack (standalone report) and --invariants (folded
    into the regular check, so this runs on every pass instead of only
    when someone remembers to run it separately).

    Returns a list of (name, addr, landed_name, landed_addr, p1, p2).
    """
    ldr = ctx.loader
    a = ctx.aether
    sym_by_addr = {v: k for k, v in ldr.symbols.items()}

    rvoca_targets = set()
    name_pos = {}  # name -> (file, lineno), for the distance filter
    for fp in ctx.re_files:
        try:
            with open(fp, 'r', encoding='utf-8', errors='ignore') as f:
                text = f.read()
        except OSError:
            continue
        for lineno, line in enumerate(text.split('\n'), 1):
            m = re.match(r'^\s*RVOCA\s+(\S+)\s+(\S+)', line)
            if m:
                rvoca_targets.add(m.group(2))
                name_pos.setdefault(m.group(1), (fp, lineno))
                continue
            m = re.match(r'^\s*(?:NOVA|NEW|SET|CLEAR|NOP|RREDI)\s+Name=(\S+)', line)
            if m:
                name_pos.setdefault(m.group(1), (fp, lineno))

    precise = []
    _ignores_next = {ldr.symbols.get(n) for n in ('Jump', 'Voca', 'Redi', 'JumpReg')}
    _ignores_next.discard(None)
    for name, addr in ldr.symbols.items():
        if a[addr + SLOT_OP] == 0:
            continue  # plain data lux (e.g. a SET string constant), never executed
        if a[addr + SLOT_OP] in _ignores_next:
            continue  # this op never consults SLOT_NEXT at all
        if a[addr + SLOT_NEXT] != 0:
            continue  # has its own explicit next; not a fallthrough
        landed_addr = addr + NOVA_SIZE
        landed_name = sym_by_addr.get(landed_addr)
        if not landed_name or landed_name == name:
            continue
        if landed_name not in rvoca_targets:
            continue
        p1 = name_pos.get(name)
        p2 = name_pos.get(landed_name)
        if p1 and p2:
            if p1[0] == p2[0] and abs(p1[1] - p2[1]) < 200:
                continue  # same file, close by -- likely deliberate
        precise.append((name, addr, landed_name, landed_addr, p1, p2))
    return precise


def _scan_nolink_after_call(ctx):
    """Structural, source-text-only diagnostic (not a hard warning -- see
    its own use in --invariants): a NOLINK immediately following an
    RVOCA/RREDI line in the source. This is the exact shape that caused
    8 real infinite-loop bugs in one session (see BUGS.md Pattern Z) --
    NOLINK tells Python's own build-time bookkeeping "don't assume this
    line links to the previous one", but a call's own return address IS
    supposed to be wired to whatever comes right after it, so this
    specific placement silently strands the call's return on runtime
    address-adjacency instead -- fragile exactly the way plain
    fallthrough is (see _scan_precise_fallthrough).

    Not flagged as wrong outright: conceivably some call's return truly
    doesn't need wiring (e.g. it never returns, or the next instruction
    is unreachable some other way). Reported for a human to glance over,
    not to block a build over.

    Returns a list of (file, lineno, call_line_text).
    """
    hits = []
    for fp in ctx.re_files:
        try:
            with open(fp, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
        except OSError:
            continue
        for i in range(len(lines) - 1):
            line = lines[i]
            if not re.match(r'^\s*RVOCA\s+\S+\s+\S+', line):
                continue
            if lines[i + 1].strip() == 'NOLINK':
                hits.append((fp, i + 1, line.strip()))
    return hits


def _fallthrough_hijack(ctx) -> int:
    """Report candidates for a specific bug class: a NOVA with no explicit
    Jump (SLOT_NEXT=0, relying on plain address adjacency to reach its
    intended next instruction) whose fallthrough actually lands on some
    OTHER, unrelated instruction.

    Found and fixed six times this session (SL_MARKINC, TBR_R_ISHEAD,
    TBR_T_ISFIRST, LOAD_MAIN_SEAL_W, LOAD_APPLY_BACKFILL, LOAD_CN_INTERN):
    allocation-order changes (e.g. the Wave 2 single-pass merge) can move
    an intended fallthrough target elsewhere, so an unrelated instruction
    -- previously landing on its real target by simple address adjacency
    -- now lands somewhere else instead, silently doing the wrong thing.

    Two passes, reported together:

    1. PRECISE: for every symbol with SLOT_NEXT=0 (relies on fallthrough),
       check whether addr+NOVA_SIZE lands on an address that is ALSO used
       as an RVOCA call TARGET somewhere else in the project (`RVOCA
       <label> <this-name>`). A genuinely callable entry point -- one
       other code explicitly Voca's into -- is a strong signal this
       address was never meant to also be silently fallen-into: both
       confirmed non-"_J" bugs this session (LOAD_MAIN_SEAL_W landing on
       LOAD_CMD_SEAL, LOAD_APPLY_BACKFILL's own entry landing on
       LOAD_MAIN_PTOW05's neighborhood) had this exact shape. An earlier
       version of this pass compared against "the next Name= declaration
       in the same source file" instead -- reasonable-sounding, but wrong
       in practice: source order routinely has anonymous/unnamed
       instructions between named declarations, so it produced over 1500
       hits, nearly all false. This version does not.

    2. HEURISTIC (the original _J-pattern check): catches macro-internal
       steps pass 1 can't reach (a JZ/JEQ's own auto-generated "_J"
       companion isn't independently RVOCA-called by anything). Structural
       match only, not confirmed -- see the shape requirement in the code
       (self/backward jump target).

    Neither pass guarantees zero false positives -- only execution
    confirms a real bug -- but PRECISE should need far less cross-checking
    than HEURISTIC before acting on it.
    """
    ldr = ctx.loader
    a = ctx.aether
    sym_by_addr = {v: k for k, v in ldr.symbols.items()}

    precise = _scan_precise_fallthrough(ctx)

    # ---- Pass 2: heuristic _J-pattern check (original) ----
    heuristic = []
    for name, addr in ldr.symbols.items():
        if not name.endswith('_J'):
            continue
        base_name = name[:-2]
        base_addr = ldr.symbols.get(base_name)
        if base_addr is None or base_addr == addr - NOVA_SIZE:
            continue
        prev_addr = addr - NOVA_SIZE
        prev_name = sym_by_addr.get(prev_addr)
        if prev_name is None or prev_name == base_name:
            continue
        if a[prev_addr + SLOT_NEXT] != 0:
            continue
        jz_target = a[addr + SLOT_EXIT]
        if jz_target and jz_target <= prev_addr:
            heuristic.append((prev_name, prev_addr, name, addr,
                               base_name, base_addr, jz_target))

    print(f"PRECISE (fallthrough lands on an independently RVOCA-called "
          f"entry point, far from it in source): {len(precise)}\n")
    for name, addr, landed_name, landed_addr, p1, p2 in precise:
        loc1 = f"{os.path.basename(p1[0])}:{p1[1]}" if p1 else "?"
        loc2 = f"{os.path.basename(p2[0])}:{p2[1]}" if p2 else "?"
        print(f"  {name}@{addr} ({loc1}) falls through to "
              f"{landed_name}@{landed_addr} ({loc2}), separately "
              f"RVOCA-called elsewhere too")

    print(f"\nHEURISTIC (structural match only -- confirm by execution): "
          f"{len(heuristic)}")
    for prev_name, prev_addr, jname, jaddr, base_name, base_addr, jt in heuristic:
        print(f"  {prev_name}@{prev_addr} falls through into "
              f"{jname}@{jaddr} (own check: {base_name}@{base_addr}), "
              f"whose target {sym_by_addr.get(jt, jt)}@{jt} "
              f"{'==' if jt == prev_addr else '<'} {prev_name}@{prev_addr}")
    return 0


COMMANDS = [
    Command("stubs", "freeze", _stubs, is_check=False,
            help="lazily-created names nothing ever filled in"),
    Command("invariants", "fresh", _invariants, is_check=True,
            help="eleven families of build invariant (FAILs only)"),
    Command("lint", "freeze", _lint, is_check=True, takes_args=True,
            help="phantoms, dead refs, self-ref and bounds violations (--all)"),
    Command("health", "freeze", _health, is_check=False,
            help="freeze health overview (counts, criticals, registers)"),
    Command("broken", "freeze", _broken, is_check=True,
            help="broken/unset infrastructure luces"),
    Command("strings", "fresh", _strings, is_check=False,
            help="string encoding layout for SF_* names"),
    Command("wiring", "fresh", _wiring, is_check=False, takes_args=True,
            help="dump full lux wiring for named symbols"),
    Command("htable", "fresh", _htable, is_check=False, takes_args=True,
            help="look up a name in the self-hosted runtime htable"),
    Command("op-identity", "fresh", _op_identity, is_check=True,
            help="compiled op matches each name's source declaration"),
    Command("deref-mismatch", "fresh", _deref_mismatch, is_check=True,
            help="operand needs an address but got a dereferenced word"),
    Command("nova-sizing", "freeze", _nova_sizing, is_check=True,
            help="every NOVA-target name got a full NOVA_SIZE lux"),
    Command("save-integrity", "freeze", _save_integrity, is_check=True,
            help="SAVE/OWN/LOCK combo-block structural guarantee holds"),
    Command("graph-equivalence", "fresh", _graph_equivalence, is_check=True,
            takes_args=True,
            help="self-hosted SK_HTAB graph agrees with Python's, by name"),
    Command("fallthrough-hijack", "fresh", _fallthrough_hijack, is_check=False,
            help="candidates: fallthrough landing on an unrelated JZ/JEQ's "
                 "own _J step (structural match, not confirmed -- see help)"),
]
