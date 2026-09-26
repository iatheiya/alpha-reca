#!/usr/bin/env python3
"""iris_common.py — shared diagnostic helpers used across theme modules.

Domain utilities that more than one theme (and the standalone check files
kept outside the hub, e.g. check_comments.py) genuinely share: the macros.re
WAX_CMD-registration scanners, packed-string decoding, address naming, and
file discovery. None of these belong to a single theme, and none of them is
build orchestration (that is iris.Context's job) — they are pure helpers on
already-available data or plain source text.

Deliberately not in iris.py: the hub owns the command contract and the
build context, nothing domain-specific. Deliberately not duplicated in each
theme: these drifted out of sync in the past when two modules each kept
their own copy of the registration scan (see BUGS.md Pattern X).
"""
import os

from loader import _read_and_parse, Loader

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = _HERE if os.path.exists(os.path.join(_HERE, 'aether.py')) \
    else os.path.dirname(_HERE)

_wax_cmd_registrations_cache = None


def all_re_files(root=None):
    """Every .re file under root (default: project root), sorted."""
    from loader import re_sources
    return re_sources(root or _ROOT)


def name_at(addr, rev):
    """Name of the symbol at `addr` given a reverse map (addr -> name),
    or a '#addr' placeholder / 'NULL' for 0."""
    if addr == 0:
        return 'NULL'
    return rev.get(addr, f'#{addr}')


def read_packed_string(a, first_chunk):
    """Decode a packed byte-chain in DATA LUX format: [word, 0] pairs,
    stride=2. Used for htable string values and bootstrap intern names.

    NOTE: a dense stride=1 format (no [word,0] padding) exists elsewhere
    (BS_PACK_TOKBUF) for a different encoding; this is not it.
    """
    cur = first_chunk
    raw = b''
    for _ in range(4096):
        if cur == 0:
            break
        w = a[cur]
        if w == 0:
            break
        raw += w.to_bytes(8, 'little')
        cur += 2  # advance to next 2-lux chunk
    nul = raw.find(b'\x00')
    return (raw[:nul] if nul >= 0 else raw).decode('ascii', errors='replace')


def iter_source_lines(path: str):
    """Yield (lineno, text) for every code line of a .re file, with comments
    already removed by the loader's own _strip_comments.

    Checks must read source through here rather than iterating raw file
    lines. Wave 0 strips `//...//` blocks and `/` lines before anything
    else sees them, so a check reading raw lines is reading text the build
    never parses -- and since command words are matched case-insensitively,
    ordinary prose collides with real commands: a comment reading
    "Wave 1 (lcount): scan all files" parses as the naming command NOVAE
    declaring a label "1". Sharing the loader's stripping is what keeps a
    check's idea of "a line of code" identical to the build's.
    """
    from loader import _strip_comments
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            raw = f.readlines()
    except OSError:
        return
    for lineno, text in _strip_comments(raw):
        if text.strip():
            yield lineno, text


def parse_source_line(line: str):
    """(cmd, args, slots) for one raw .re line, or None if it holds no
    command -- parsed with the loader's own tokenizer and arg/slot split.

    Every check that reads a name, an operand or a slot out of source text
    must go through here rather than its own regex or `line.split()`. A
    naming command's label can be written either way:

        NOVA FOO Move El1=X          -- positional
        NOVA Name=FOO Move El1=X     -- keyword

    and `_split_args_slots` normalises both into args[0]. A hand-rolled
    `parts[1]` sees the literal string "Name=FOO" instead: it either
    records a label that does not exist, or fails to match its symbol and
    skips the line in silence -- a check that reports clean because it
    stopped looking. Sharing the loader's parser is the only way a check
    stays honest across a syntax change.

    Unlike loader._parse_record there is no known-command gate: checks scan
    raw text, including lines whose first token is prose or a not-yet-known
    macro, and decide relevance themselves.
    """
    from loader import _tokenize_quoted, _split_args_slots
    p = _tokenize_quoted(line)
    if not p:
        return None
    cmd = p[0].upper()
    args, slots = _split_args_slots(p[1:])
    return cmd, args, slots


def wax_cmd_registrations(macros_path=None, ldr=None) -> dict:
    """Every `WAX_CMD Name=name entry` (or positional `WAX_CMD name entry`)
    pair in expand.re/synth.re, as {name: entry}.

    The single source of truth for "what entry point should this macro's
    word field point to" -- every WAX_CMD line states this exactly, so
    nothing needs a hand-maintained parallel guess (see BUGS.md Pattern X
    for a real case of such a guess going stale). Replaces the old
    SEAL-registration scan of the same name and shape: SEAL was retired
    as a pure subset of SET (loader.py) and no longer registers anything,
    self-hosted or otherwise -- WAX_CMD has been the real, live
    registration form all along.

    Scans both expand.re and synth.re (macros.re, formerly the one file
    for all of this, was split between the two and then deleted -- see
    each file's own header for the split criterion) unless macros_path is
    given explicitly, in which case only that one file is scanned.

    Diagnostics-only: nothing in the real freeze() path calls this. Reads
    via loader._read_and_parse (the same single-parse path everything uses)
    so it inherits the quote-aware tokenizer instead of a second copy of the
    old naive-split bug.
    """
    global _wax_cmd_registrations_cache
    # Every .re file, like loader._discover_nova_naming_commands: any file
    # may declare a command, and a hardcoded <root>/commands.re silently
    # found nothing once files moved into subdirectories.
    default_paths = all_re_files()
    if macros_path is None and ldr is None and _wax_cmd_registrations_cache is not None:
        return _wax_cmd_registrations_cache
    paths = [macros_path] if macros_path is not None else default_paths
    regs: dict = {}
    scan_ldr = ldr
    if scan_ldr is None or not hasattr(scan_ldr, '_discover_nova_naming_commands'):
        import interpreter as _interp_mod
        scan_ldr = Loader(_interp_mod.Interpreter())
    for path in paths:
        if not os.path.exists(path):
            continue
        # _read_and_parse needs _discover_nova_naming_commands, which minimal
        # loader stand-ins don't have -- fall back to a full Loader for the scan
        # itself rather than requiring every caller's loader to be complete.
        _lines, records = _read_and_parse(path, scan_ldr)
        for _lineno, rec in records:
            if not rec:
                continue
            cmd, args, slots = rec
            if cmd == 'WAX_CMD' and args:
                name = slots.get('Name') or args[0]
                body = args[1] if len(args) > 1 else (args[0] if slots.get('Name') else None)
                if name and body:
                    regs[name] = body
    if macros_path is None and ldr is None:
        _wax_cmd_registrations_cache = regs
    return regs


def wax_cmd_macro_names(macros_path=None, ldr=None) -> set:
    """Every WAX_CMD-declared macro name in expand.re/synth.re, as a set.

    Shared by any diagnostic needing "is this name a real macro command"
    (the macros command, check_comments.py's swallowed-comment detector,
    check_deref_mismatch) without each keeping its own hand-written scan.
    """
    return set(wax_cmd_registrations(macros_path, ldr).keys())
