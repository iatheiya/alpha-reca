#!/usr/bin/env python3
"""iris_source.py — source-text diagnostics: everything checkable by
reading .re files as text, before any build.

Every command here declares NEEDS="text": it consumes ctx.re_files and
nothing else. These are the fastest checks (no freeze, no thaw) and the
first line of defence — lexical/structural source problems that a build
would either mask or crash on.

Each command's detection logic is the original check_*.py / diag mode
algorithm, moved verbatim; only its harness (argv parsing, main(),
print wrappers) is replaced by the iris Command contract.
"""
import os
import re
from collections import defaultdict

from loader import (_strip_comments, _special_openers,
                    _parse_indent_blocks)

# The block-opening command words, discovered from the corpus's own `Open=`
# declarations (see loader._discover_block_openers). Read once here.
_BLOCK_OPENERS = _special_openers()

from iris import Command
from iris_common import (_ROOT, wax_cmd_macro_names, parse_source_line,
                         iter_source_lines)


# ── duplicate labels (was check_duplicate_labels.py) ──────────────────────
# Every op that introduces a new, named label at the position of its own
# first argument. FOR-template placeholders ({N} etc.) are skipped, since
# those aren't real, fixed names.
_LABEL_OPS = (
    'NOVA', 'RVOCA', 'RREDI', 'CLEAR', 'JEQ', 'JZ', 'NOP', 'ALLOC_TO',
    'NOVAE', 'LR', 'LT', 'LX', 'LH', 'WALK_NOVA', 'WALK_ONE', 'PUTBYTE',
    'EMIT', 'EMITI', 'RCALL_AT', 'UNLINK_OP', 'LINK_OP',
)


def _duplicate_labels(ctx) -> int:
    """Find every label defined more than once project-wide.

    Catches BUGS.md Pattern Z: Python's "name already exists, no-op"
    collision rule silently skips allocating a duplicate declaration's own
    instruction -- whichever definition the parallel build processes first
    wins, and the second's intended content is never allocated.
    """
    defs = defaultdict(list)
    for fn in ctx.re_files:
        # MATCH block context: a MATCH's indented arms are `KEYWORD target`
        # routing entries, NOT label definitions. An arm whose keyword is a
        # _LABEL_OPS word (e.g. `NOVA LOAD_PP_NOVADCL`) would otherwise be
        # miscounted as defining a lux named after its target -- colliding
        # with that target's real handler definition elsewhere. Track the
        # opener's indent and skip everything more-indented than it.
        match_indent = None
        for lineno, line in iter_source_lines(fn):
                indent = len(line) - len(line.lstrip(' '))
                rec = parse_source_line(line)
                if rec is None:
                    continue
                cmd, args, _slots = rec
                if match_indent is not None:
                    if indent > match_indent:
                        continue        # a MATCH arm -- not a definition
                    match_indent = None  # dedented out: block closed
                if cmd == 'MATCH':
                    match_indent = indent
                    continue
                if cmd not in _LABEL_OPS or not args:
                    continue
                name = args[0]
                if '{' in name:
                    continue
                defs[name].append((fn, lineno))

    dupes = {name: locs for name, locs in defs.items() if len(locs) > 1}
    print(f"Checked {len(defs)} unique labels project-wide.")
    if not dupes:
        print("✓ Clean -- no label defined more than once.")
        return 0

    print(f"\n✗ {len(dupes)} label(s) defined more than once -- the second (and")
    print("  any further) definition's own content was likely never allocated:\n")
    for name, locs in sorted(dupes.items()):
        loc_str = ", ".join(f"{fn}:{ln}" for fn, ln in locs)
        print(f"  {name}: {loc_str}")
    return 1


# ── dropped lines: source the loader silently discards ────────────────────
def _dropped(ctx) -> int:
    """Report every source line the loader parses to nothing.

    Runs the real pipeline -- _strip_comments, _parse_indent_blocks, _parse_record
    -- and reports each line that produces no record. Such a line is not an
    error to the loader: an unrecognised command word is indistinguishable
    from prose, so it is dropped in silence. That silence is the problem.
    It is how 663 `NEWSET` lines in generated/ stayed dead for months after
    the command was removed from the language, taking K_AETHER_SIZE and the
    whole PREAM_* preamble with them, while every check reported clean.

    Replaces the old label-order check, which looked for one specific
    typo shape (`NAME JEQ ...` instead of `JEQ NAME ...`) against a
    hand-written list of command words. That list was a second, drifting
    copy of what counts as a command, and the check it powered is a strict
    subset of this one: a label written before its command is exactly a
    line whose first word is not a command, so it lands here anyway --
    along with every other misspelling, removed command and stale
    generator, none of which the narrow check could see.

    A report, not a pass/fail check: decorative section headers are
    legitimate dropped lines, so a human reads the list. Headers made only
    of box-drawing or punctuation are filtered out; anything word-shaped
    is shown.
    """
    import interpreter as _interp_mod
    from loader import (Loader as _Loader, _strip_comments as _strip,
                        _parse_indent_blocks as _expand, _parse_record as _parse,
                        _known_commands as _known, seed_candidates)
    import io as _io, sys as _sys

    buf = _io.StringIO()
    old = _sys.stderr
    _sys.stderr = buf
    try:
        scan = _Loader(_interp_mod.Interpreter())
        # Seed only: command declarations (Cmd=) are discovered by the loader
        # itself from every .re file, so no command file is named here.
        scan.load_file(seed_candidates()[0])
    finally:
        _sys.stderr = old
    known = _known(scan)

    print("=== Dropped lines: source the loader parses to nothing ===\n")
    total = 0
    for fp in ctx.re_files:
        fname = os.path.basename(fp)
        try:
            with open(fp, encoding='utf-8', errors='replace') as f:
                raw = f.readlines()
        except OSError:
            continue
        try:
            expanded = _expand(_strip(raw), None, fname.replace('.re', ''))
        except Exception as exc:
            print(f"{fname}: expansion failed -- {exc}")
            continue

        hits = []
        for lineno, text in expanded:
            if not isinstance(text, str):
                continue  # direct-build record (already parsed), never dropped
            t = text.strip()
            if not t:
                continue
            head = t.split()[0]
            # Decoration: a heading rule, not a command -- no letters/digits.
            if not any(c.isalnum() for c in head):
                continue
            # A block opener's own header line (CHAIN/FOR/SWITCH/...) is
            # consumed by _parse_indent_blocks: it directed the expansion and has
            # no record of its own to produce. Surviving into the expanded
            # output is normal for it, not evidence of dead source.
            if head.upper() in _BLOCK_OPENERS:
                continue
            if _parse(t, known) is None:
                hits.append((lineno, t))
        if hits:
            total += len(hits)
            print(f"{fname} ({len(hits)}):")
            for lineno, t in hits[:10]:
                print(f"  {lineno:5d}  {t[:70]}")
            if len(hits) > 10:
                print(f"  ... and {len(hits)-10} more")
            print()

    if total == 0:
        print("No word-shaped line is dropped -- every one parses to a record.")
    else:
        print(f"{total} dropped line(s). Each is either prose that belongs "
              f"inside a comment, or a command the loader no longer knows -- "
              f"the second kind is silently dead code.")
    return 0


# ── unreachable entry guard (was check_unreachable_entry.py) ───────────────
# Deliberately narrower than _LABEL_OPS above: only the ops whose adjacency
# to an RVOCA entry point matters for the "guard placed before the real
# entry, never reached" pattern. Not the same list, on purpose.
_ENTRY_LABEL_OPS = ('NOVA', 'RVOCA', 'RREDI', 'CLEAR', 'JEQ', 'JZ', 'NOP',
                    'ALLOC_TO')


def _unreachable_entry(ctx) -> int:
    """Find guard/setup instructions positioned before a real (RVOCA-called)
    entry point, where the guard itself is never referenced -- so control
    never reaches it, it's silently dead."""
    prev_in_file: dict = {}
    prev_op: dict = {}

    for fn in ctx.re_files:
        prev_name = None
        prev_opname = None
        reset = True
        for _lineno, line in iter_source_lines(fn):
            stripped = line.strip()
            if stripped == 'NOLINK':
                reset = True
                continue
            if not stripped or stripped.startswith('/'):
                continue
            rec = parse_source_line(stripped)
            if rec is None:
                continue
            cmd, args, _slots = rec
            if cmd in _ENTRY_LABEL_OPS and args and '{' not in args[0]:
                name = args[0]
                if not reset and prev_name:
                    prev_in_file[name] = (fn, prev_name)
                    prev_op[prev_name] = prev_opname
                prev_name = name
                prev_opname = cmd
                reset = False

    rvoca_targets = set()
    referenced = set()
    for fn in ctx.re_files:
        for _ln, line in iter_source_lines(fn):
                rec = parse_source_line(line)
                if rec is not None:
                    cmd, args, slots = rec
                    if cmd == 'RVOCA' and len(args) >= 2:
                        rvoca_targets.add(args[1])
                        referenced.add(args[1])
                    if cmd in ('JEQ', 'JZ') and len(args) >= 4:
                        referenced.add(args[-1])
                    if cmd == 'SET' and 'Value' in slots:
                        referenced.add(slots['Value'])
                    if 'Exit' in slots:
                        referenced.add(slots['Exit'])

    suspects = []
    for name, (fn, prev) in prev_in_file.items():
        if name not in rvoca_targets:
            continue
        if prev in referenced:
            continue
        if prev_op.get(prev) == 'RREDI':
            continue
        suspects.append((fn, prev, prev_op.get(prev), name))

    checked = sum(1 for n in prev_in_file if n in rvoca_targets)
    print(f"Checked {checked} externally-called (RVOCA target) entry points.")
    if not suspects:
        print("✓ Clean -- no unreachable guard/setup instruction found.")
        return 0

    print(f"\n✗ {len(suspects)} suspect(s) -- likely never-reached "
          f"guard/setup, positioned before the real entry point:\n")
    for fn, prev, op, name in suspects:
        print(f"  {fn}:  [{op}] {prev}  -- positioned before real "
              f"entry point {name}, but {prev} is never itself referenced")
    return 1


# ── BS_LOOKUP sites (was check_lookup_sites.py) ───────────────────────────
def _lookup_sites(ctx) -> int:
    """List every plain BS_LOOKUP call site for manual forward-reference
    review. A listing tool, not pass/fail -- always returns 0."""
    sites = []
    for fn in ctx.re_files:
        with open(fn) as f:
            lines = f.readlines()
        for lineno, line in iter_source_lines(fn):
            i = lineno - 1
            rec = parse_source_line(line)
            if rec is None:
                continue
            cmd, args, _slots = rec
            if cmd != 'RVOCA' or len(args) != 2 or args[1] != 'BS_LOOKUP':
                continue
            label = args[0]
            context = ''.join(lines[i+1:i+4])
            sites.append((fn, i + 1, label, context))

    print(f"Found {len(sites)} plain BS_LOOKUP call site(s) -- each needs")
    print("manual review (does a miss silently proceed with 0/garbage, or")
    print("explicitly skip/allocate? only the former needs LOAD_INTERN):\n")
    for fn, lineno, label, context in sites:
        print(f"{fn}:{lineno}  RVOCA {label} BS_LOOKUP")
        for cl in context.rstrip().splitlines():
            print(f"    {cl}")
        print()
    return 0


# ── block lint: indent/style outliers in block bodies ─────────────────────
# Generalizes a real bug: an indentation-aware block (CHAIN/SWITCH/FOR/NOVAE)
# had sibling body lines at one indent depth and one line at a deeper indent
# with a stray "NOVA " prefix. The deeper indent silently changed how the line
# was parsed, and the resulting lux was never correctly wired -- the bug class
# BUGS.md documents repeatedly.
#
# A *finder*, not an authority: genuine multi-line continuations look the
# same, so every hit needs a human read of the surrounding block.
_BLOCK_PRIMITIVE_FIRST_TOKENS = frozenset({
    'NOVA', 'JZ', 'JEQ', 'RVOCA', 'RREDI', 'CLEAR', 'NOP', 'NOVAE',
    'EMIT', 'EMITI', 'WRITE_OUT', 'ALLOC_TO', 'LINK_OP', 'UNLINK_OP',
    'PUTBYTE', 'RCALL_AT', 'WALK_NOVA', 'WALK_ONE', 'LINK', 'LH', 'LT',
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
    """Return [(block_start_lineno, opener, finding_str), ...] for one file."""
    findings = []
    try:
        with open(filepath, encoding='utf-8', errors='replace') as f:
            raw_lines = f.readlines()
    except OSError:
        return findings
    stripped = _strip_comments(raw_lines)

    i = 0
    n = len(stripped)
    while i < n:
        lineno, text = stripped[i]
        first_tok = text.split()[0] if text.strip() else ''
        if first_tok in _BLOCK_OPENERS:
            opener_lineno = lineno
            opener_tok = first_tok
            body = []
            j = i + 1
            while j < n:
                bln, braw, bind, btext = stripped[j]
                if not btext.strip():
                    break  # blank line ends the body
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
                # Signal B: mixed primitive-keyword style at the modal depth
                modal_depth = max(depth_counts, key=lambda k: depth_counts[k])
                modal_lines = [(bln, btok, btext) for (bln, d, btok, btext) in body
                               if d == modal_depth]
                if len(modal_lines) >= 2:
                    primitive_count = sum(1 for (_, t, _) in modal_lines
                                          if t in _BLOCK_PRIMITIVE_FIRST_TOKENS)
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


def _resolve_targets(ctx, args):
    """Explicit file arguments (relative to project root) or every .re file."""
    targets = [x for x in args if not x.startswith('-')]
    if targets:
        return [f if os.path.isabs(f) else os.path.join(_ROOT, f) for f in targets]
    return ctx.re_files


def _block_lint(ctx, args) -> int:
    """Scan CHAIN/SWITCH/FOR/NOVAE body blocks for indent/style outliers."""
    files = _resolve_targets(ctx, args)
    print("=== Block-lint: indentation/style outliers in CHAIN/SWITCH/FOR/NOVAE bodies ===\n")
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
    return 0


# ── unwrapped: non-ASCII lines, classified by risk ────────────────────────
def _scan_unwrapped_file(filepath: str):
    """Return [(lineno, in_block, opener_tok, text), ...] for one file.

    Flags lines containing non-ASCII bytes -- the actual failure mode: the
    self-hosted lexer reads byte-by-byte with no Unicode decoding, so a
    multi-byte character like '─' becomes 2-4 bytes matching no token.
    Deliberately narrower than "first token isn't a known command", which
    produces hundreds of false positives on legitimate block-body syntax.
    """
    findings = []
    try:
        with open(filepath, encoding='utf-8', errors='replace') as f:
            raw_lines = f.readlines()
    except OSError:
        return findings
    stripped = _strip_comments(raw_lines)

    block_stack = []
    for lineno, text in stripped:
        if not text.strip():
            # No content: takes no part in structure. Blank lines and
            # comment lines alike are simply not there as far as block
            # nesting is concerned -- a level ends at a line with content
            # whose indent is no deeper than its opener, and nothing else.
            continue
        indent = len(text) - len(text.lstrip(' \t'))
        while block_stack and indent <= block_stack[-1][1]:
            block_stack.pop()

        if any(ord(ch) > 127 for ch in text):
            in_block = bool(block_stack)
            opener = block_stack[-1][0] if block_stack else None
            findings.append((lineno, in_block, opener, raw.rstrip()[:70]))

        first_tok = text.split()[0] if text.strip() else ''
        if first_tok in _BLOCK_OPENERS:
            block_stack.append((first_tok, indent))

    return findings


def _unwrapped(ctx, args) -> int:
    """Find non-ASCII lines outside comments; flag those inside an
    indentation-aware block body as needing a human read."""
    files = _resolve_targets(ctx, args)
    print("=== Unwrapped lines: non-ASCII content outside any comment, "
          "classified by risk ===\n")
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
    return 0


# ── file dependency graph (was diag.py mode_deps) ─────────────────────────
def _deps(ctx) -> int:
    """Per-file symbol counts and cross-file name dependencies.

    Builds its own minimal Loader (seed only) rather than using ctx: it
    needs per-file defs/refs before anything is wired, which a finished
    build no longer distinguishes."""
    print("=== .re File Dependency / Load Order ===\n")
    import io as _io
    import sys as _sys
    import interpreter as _interp_mod
    from loader import Loader as _Loader, _read_and_parse as _rap

    interp = _interp_mod.Interpreter()
    ldr = _Loader(interp)
    buf = _io.StringIO()
    old = _sys.stderr
    _sys.stderr = buf
    from loader import seed_candidates
    seed_path = os.path.abspath(seed_candidates()[0])
    ldr.load_file(seed_path)
    _sys.stderr = old

    all_files = [os.path.abspath(p) for p in ctx.re_files
                 if os.path.abspath(p) != seed_path]

    naming_commands = ldr._discover_nova_naming_commands()
    file_info = {}
    for fp in all_files:
        _lines, records = _rap(fp, ldr)
        defs, refs = ldr._scan_defs_refs(records, naming_commands)
        file_info[fp] = {'defs': defs, 'refs': refs - defs}

    name_to_file = {}
    for fp, info in file_info.items():
        for name in info['defs']:
            if name not in name_to_file:
                name_to_file[name] = fp

    dep_graph = {fp: set() for fp in all_files}
    for fp, info in file_info.items():
        for name in info['refs']:
            if name not in ldr.symbols:
                dep_fp = name_to_file.get(name)
                if dep_fp and dep_fp != fp:
                    dep_graph[fp].add(dep_fp)

    print(f"aspects.re  [seed]  {len(ldr.symbols)} symbols\n")
    for fp in sorted(all_files, key=lambda f: -len(file_info[f]['defs'])):
        name = os.path.basename(fp)
        deps = sorted(os.path.basename(d) for d in dep_graph[fp])
        n_defs = len(file_info[fp]['defs'])
        dep_str = ', '.join(deps) if deps else '(none)'
        print(f"{name:24s} {n_defs:4d} defs  depends on: {dep_str}")

    # Direct two-file cycles only (A<->B) -- informational. The loader needs
    # no cycle detection at all: Wave 1 sizes every name before Wave 2 mints
    # one, so file order is irrelevant to correctness. Longer cycles
    # (A->B->C->A) aren't flagged; adding Tarjan just to power this report
    # would mean maintaining an algorithm the real load path never uses.
    seen_pairs = set()
    cycles = []
    for fp, deps in dep_graph.items():
        for dep in deps:
            if fp in dep_graph.get(dep, ()):
                pair = tuple(sorted((fp, dep)))
                if pair not in seen_pairs:
                    seen_pairs.add(pair)
                    cycles.append(pair)
    if cycles:
        print(f"\nDirect two-file cycles ({len(cycles)}):")
        for a, b in sorted(cycles):
            print(f"  {os.path.basename(a)} <-> {os.path.basename(b)}")
    print(f"\nTotal files: {len(all_files)}")
    return 0


# ── indent synthesis log (was diag.py mode_indent) ────────────────────────
def _indent(ctx) -> int:
    """Show every line the indent mechanism synthesises into a full command.

    Runs the loader's own pipeline -- _strip_comments then _parse_indent_blocks --
    and reports where the expansion differs from the source line. It used to
    re-implement both halves (its own character-by-character comment scanner
    and its own copy of the leader/synthesis rules), which meant the report
    could drift from what the build actually does and quietly describe a
    mechanism that no longer existed. Calling the real functions makes the
    report incapable of disagreeing with the loader.

    _parse_indent_blocks needs no Loader instance (it takes one only for a
    historical counter that is gone), so this stays a text-level command.
    """
    print("=== Indent Synthesis Log ===\n")

    total = 0
    for fp in ctx.re_files:
        fname = os.path.basename(fp)
        try:
            with open(fp, encoding='utf-8', errors='replace') as f:
                raw = f.readlines()
        except OSError:
            continue
        stripped = _strip_comments(raw)
        original = {}
        for lineno, text in stripped:
            original.setdefault(lineno, text.strip())
        try:
            expanded = _parse_indent_blocks(stripped, None, fname.replace('.re', ''))
        except Exception as exc:
            print(f"{fname}: expansion failed -- {exc}")
            continue

        synth = [(lineno, original.get(lineno, ''), text.strip())
                 for lineno, text in expanded
                 if text.strip() and text.strip() != original.get(lineno, '')]
        if synth:
            total += len(synth)
            print(f"{fname} ({len(synth)}):")
            for ln, orig, result in synth[:8]:
                print(f"  {ln:4d}  {orig[:32]:32s} \u2192 {result[:50]}")
            if len(synth) > 8:
                print(f"  ... and {len(synth)-8} more")
            print()
    print(f"Total synthesised: {total}")
    return 0



# ── macro parity for one file (was diag.py mode_parity) ───────────────────
def _parity(ctx, args) -> int:
    """Dump every NOVA lux one target file contributes, with named slots.

    Loads everything else first, then the target, so the diff is exactly
    what that file added."""
    import io as _io
    import sys as _sys
    from aether import NOVA_SIZE as _NOVA_SIZE
    from interpreter import Interpreter as _Interp
    from loader import Loader as _Loader

    targets = [x for x in args if not x.startswith('-')]
    if not targets:
        print("Usage: python3 iris.py --parity FILE.re")
        return 0
    target_path = targets[0]
    if not os.path.isabs(target_path):
        target_path = os.path.join(_ROOT, target_path)

    print(f"=== Macro Parity: {os.path.basename(target_path)} ===\n")
    if not os.path.exists(target_path):
        print(f"File not found: {target_path}")
        return 1

    buf = _io.StringIO()
    old = _sys.stderr
    _sys.stderr = buf
    ldr_a = _Loader(_Interp())
    from loader import seed_candidates
    _seed = os.path.abspath(seed_candidates()[0])
    ldr_a.load_file(_seed)
    for f in ctx.re_files:
        if os.path.abspath(f) not in (os.path.abspath(target_path), _seed):
            ldr_a.load_file(f)
    sym_before = set(ldr_a.symbols.keys())
    ldr_a.load_file(target_path)
    _sys.stderr = old

    a_a = ldr_a.interp.aether.aether
    R_a = ldr_a.symbols
    sym_a = {v: k for k, v in R_a.items()}
    target_instrs = {
        name: [a_a[addr+i] for i in range(_NOVA_SIZE)]
        for name, addr in R_a.items()
        if name not in sym_before and addr in ldr_a._nova_addrs_set
    }
    print(f"Python loader: {len(target_instrs)} NOVA luces from target")
    slot_labels = ['word', 'op', 'e1', 'e2', 'exit', 'next', 'pad']
    printed = 0
    for name, slots in sorted(target_instrs.items()):
        slotstr = '  '.join(
            f"{slot_labels[i]}={sym_a.get(slots[i], slots[i])}"
            for i in range(_NOVA_SIZE) if slots[i]
        )
        print(f"  {name}: {slotstr}")
        printed += 1
        if printed >= 40:
            print(f"  ... ({len(target_instrs)-printed} more)")
            break
    return 0


# ── macro registry vs usage (was diag.py mode_macros) ─────────────────────
# Commands expected to be used directly as a line-starting keyword: a
# curated distinction, not an attempt to enumerate every macro (that is what
# wax_cmd_macro_names() is for).
_DIRECT_PRIMITIVES = {
    'NEW', 'NOVA', 'RVOCA', 'RREDI', 'LINK',
    'NOLINK', 'ALLOC_TO', 'JEQ', 'JZ', 'SWITCH', 'FOR',
    'CLEAR', 'NOP', 'SET',
    # CHAIN/SAVE: genuinely used directly, but they are preprocessor
    # constructs fully expanded before a literal token reaches this scan --
    # so they always show as "never seen as p[0]" regardless of real usage.
    'CHAIN', 'SAVE',
    'YAKU_NEXO', 'YAKU_NEXO_TERM', 'YAKU_NEXO_CMP',
    'YAKU_NEXO_ARITH', 'YAKU_NEXO_ALIAS', 'RCALL_AT',
    'LH', 'LR', 'LT', 'WALK_ONE', 'EMIT', 'EMITI',
    'PUTBYTE', 'LINK_OP', 'UNLINK_OP',
}


def _macros(ctx) -> int:
    """Which Reca macro commands are defined vs actually invoked as a
    line-starting command across the .re sources."""
    from loader import _read_re_file, _parse_line

    print("=== Macro Registry vs .re Usage ===\n")
    all_macros = wax_cmd_macro_names(ldr=ctx.loader)
    all_commands = _DIRECT_PRIMITIVES | all_macros
    used = set()
    for fp in ctx.re_files:
        in_block = False
        for _, line in _read_re_file(fp):
            s = line.strip()
            if not s:
                continue
            if s.startswith('//'):
                in_block = not in_block
                continue
            if in_block or s.startswith('/'):
                continue
            p = _parse_line(line)
            if p and p[0] in all_commands:
                used.add(p[0])

    # Most WAX_CMD macros are legitimately helper-only (only ever an RVOCA
    # target), so "never invoked as p[0]" is informational. Only a curated
    # primitive going unused is worth flagging.
    never_as_command = all_commands - used
    suspicious = never_as_command & _DIRECT_PRIMITIVES
    helper_only = never_as_command - _DIRECT_PRIMITIVES
    if suspicious:
        print(f"In primitives (expected as a direct command) but never seen as one "
              f"({len(suspicious)}): {sorted(suspicious)}")
    else:
        print("Every primitive expected as a direct command is used as one  \u2713")
    print(f"Helper-only macros (never used as p[0], only as an RVOCA/etc. "
          f"target -- expected, not a problem) ({len(helper_only)})")
    print(f"Commands: {len(_DIRECT_PRIMITIVES)} primitives + {len(all_macros)} "
          f"WAX_CMD-defined = {len(all_commands)} total")
    return 0


COMMANDS = [
    Command("duplicate-labels", "text", _duplicate_labels, is_check=True,
            help="find any label defined more than once"),
    Command("dropped", "text", _dropped, is_check=False,
            help="source lines the loader silently parses to nothing"),
    Command("unreachable-entry", "text", _unreachable_entry, is_check=True,
            help="find never-reached guard before an entry point"),
    Command("deps", "text", _deps, is_check=False,
            help="per-file symbol counts and cross-file dependencies"),
    Command("indent", "text", _indent, is_check=False,
            help="every line synthesised by the indent mechanism"),
    Command("parity", "text", _parity, is_check=False, takes_args=True,
            help="dump NOVA luces one target file contributes"),
    Command("macros", "fresh", _macros, is_check=False,
            help="macro registry vs actual line-starting usage"),
    Command("block-lint", "text", _block_lint, is_check=False, takes_args=True,
            help="indent/style outliers in CHAIN/SWITCH/FOR/NOVAE bodies"),
    Command("unwrapped", "text", _unwrapped, is_check=False, takes_args=True,
            help="non-ASCII lines, flagged when inside a block body"),
    Command("lookup-sites", "text", _lookup_sites, is_check=False,
            help="list BS_LOOKUP sites for manual review (listing only)"),
]
