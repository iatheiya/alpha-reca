"""
loader.py — Reca bootstrap loader + Aether persistence

BOOTSTRAP ONLY: builds Aether from .re text files during freeze.
After reca.bin is written, this file is never called at runtime.

LUX MODEL:
  Two lux kinds share the same u64-lux Aether:

  ITO (instruction) luces — compact fixed-slot layout:
    slot 0  = word  (self-ref: aether[addr] = addr)
    slot 1  = op    (primitive ID — dispatch key)
    slot 2  = e1    (first operand)
    slot 3  = e2    (second operand)
    slot 4  = exit  (destination: store target for Move/ALU, jump target for Jump/JumpIf,
                     RA_LINK address for Voca)
    slot 5  = next  (SLOT_NEXT: 0 = implicit fall-through pc+ITO_SIZE; !0 = explicit link)
    slot 6  = pad   (zeroed — terminates lumen scan before extra lumina)
    slot 7+ = extra LINK lumina as (rel_addr, tgt_addr) pairs, 0-terminated

  Data luces — arbitrary lumen pairs:
    slot 0  = word  (value)
    slot 1+ = lumen pairs (rel_addr, tgt_addr), 0-terminated

  ITO and Data luces are distinguished by Op (slot 1): nonzero = ITO, zero = Data.
  Extra lumina on ITO luces start at slot 7 (ITO_SIZE), appended by LINK commands.

GLOBAL LUMEN PREPASS:
  Before any allocation, all .re files are scanned for:
    1. Explicit LINK src rel tgt  → lumen (rel, exit) on src
    2. Implicit Next              → sequential ITO luces linked by (Next, nxt)
       (same _last_ito tracking as pass2, NOLINK/RREDI/etc reset flow)
  Result: {sym_name: [tgt_name, ...]} tells _alloc_ito/_alloc_data the
  correct lux count for each lux before allocation happens.
"""
import os
import sys
import glob

from symphony import _MASK64, SLOT_WORD, SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, \
    ITO_SIZE, SLOT_NEXT, BOOTSTRAP_FIRST_FREE, LUMEN_PAIR, DATA_LUX_MIN


# Aliases for internal use in loader.py
_WORD     = SLOT_WORD
_OP       = SLOT_OP
_E1       = SLOT_E1
_E2       = SLOT_E2
_EXIT     = SLOT_EXIT
_NEXT     = SLOT_NEXT
_ITO_SIZE = ITO_SIZE
_BOOTSTRAP_FIRST_FREE = BOOTSTRAP_FIRST_FREE

# ITO opcodes that unconditionally end a basic block (reset _last_ito auto-Next).
# Jump/JumpReg/End: direct terminators via ITO command.
# Redi: handled by RREDI macro (which also resets _last_ito), but listed here
#        so a bare "ITO X Redi" also terminates the flow.
_TERMINATORS = frozenset({"Jump", "JumpReg", "End", "Redi"})

# Bootstrap primitives — the irreducible minimum Python handles directly.
# Everything else is a Reca macro: loader looks up {CMD}_MACRO in symbols and calls it.
_BOOTSTRAP_CMDS = frozenset({
    "NEW", "SET", "LINK", "NEWREF", "NEWSET", "SETREF", "ALIAS",
    "ITO", "BLOCK", "NOLINK",
})

# Macro dispatch: when loader sees unknown CMD, it looks up {CMD} in symbols.
# No registry needed — convention-based. Reca defines the logic, Python just calls.


def _decode_string_word(aether, head_addr: int, max_chars: int = 256) -> str:
    """Decode a packed byte-chain back into a Python string.

    Walks consecutive luces (stride=1, 8 bytes per lux, little-endian).
    Stops at the first lux whose word is 0 (NUL terminator lux) or at
    the first NUL byte within a word.
    to_bytes is faster than per-byte shifts.
    """
    if not head_addr:
        return ""
    out = bytearray()
    lux = head_addr
    safety = max_chars
    while lux and safety > 0:
        w = aether[lux]
        if w == 0:
            break
        raw = w.to_bytes(8, 'little')
        nul = raw.find(0)
        if nul != -1:
            out += raw[:nul]
            return out.decode('utf-8', 'replace')
        out += raw
        lux += 1   # stride=1: matches Reca BS_PACK_TOKBUF (BPT_DSTINC +C_1)
        safety -= 1
    return out.decode('utf-8', 'replace')


# Universal indent context: maps leader command (upper) → (mode, data)
#
# mode='nexo'    — indented line "body" synthesises "NEXO <name> <body>"
#                  data = name string (token at name_index)
# mode='prepend' — indented line "CMD els" synthesises "CMD <name> els"
#                  data = name string
# mode='switch'  — indented line "<val> <dest>" or "<v1> <v2> > <dest>" synthesises JEQ chains
#                  data = (reg, counter_base) where reg is the register to compare against
#                  Handled specially in _read_re_file (not via simple string substitution).
# mode='save'    — indented lines are the body; on close, restore-ITO lines are injected.
#                  data = regs_to_restore (RA_LINK is never in this list — see SAVE handling)
#                  Handled specially in _read_re_file.
# mode='chain'   — indented lines are <name> <rel> <val> triplets; on close, emits a
#                  chain of ITO+LINK pairs for each triplet.
# mode='for'     — indented line "CMD els" substituting {N}=index and {X}=element name.
#                  data = (elements, counter, name_index)
#
# Adding a new nexo/prepend leader: register it here. No changes needed in _read_re_file.
_INDENT_LEADERS: dict = {
    # Allocation family — NEW X / SET 5 → SET X 5
    "NEW":             ('prepend', 1),
    "NEWSET":          ('prepend', 1),
    "BLOCK":           ('prepend', 1),
}

# Special-opener tokens: first token of any line that opens a new indented
# context (pushes onto _ctx_stack or has its own dedicated dispatch path).
# Lines starting with these MUST NOT be swallowed by rel>0 raw passthrough
# inside CHAIN bodies -- they need to go through the main dispatch
# loop so they're correctly recognized and expanded regardless of where
# they appear in the source.
_SPECIAL_OPENERS: frozenset = frozenset({
    'SWITCH', 'CHAIN', 'SAVE', 'FOR', 'WAVE',
    'WRITE_OUT', 'READ_BODY',
})


_strip_comments_cache: dict = {}  # (filepath, mtime) -> stripped result


def _strip_comments(raw_lines: list) -> list:
    """Strip //...// block comments from raw file lines.

    Returns [(lineno, original_line, is_indented, stripped_text), ...].
    Blank lines after stripping are included as (lineno, line, is_indented, '')
    so _expand_indent can handle blank-line context resets correctly.

    Rules:
    - A line whose first non-whitespace char is '/' but NOT '//' is a whole-line
      comment: output is empty; // is NOT scanned inside it.
    - // outside a string literal opens a block comment.
    - The next // closes it; rest of that line discarded.
    - Inside double-quoted strings, // is not a delimiter.
    """
    result = []
    in_block = False
    for lineno, raw in enumerate(raw_lines, 1):
        line = raw.rstrip('\n')
        n = len(line)
        is_indented = n > 0 and line[0] in (' ', '\t')
        stripped_line = line.lstrip()

        # Whole-line single-slash comment: starts with / but NOT //
        # Do NOT scan for // inside — avoids false block opens.
        if not in_block and stripped_line and stripped_line[0] == '/' and (len(stripped_line) < 2 or stripped_line[1] != '/'):
            result.append((lineno, line, is_indented, ''))
            continue

        out = []
        in_str = False
        i = 0
        while i < n:
            ch = line[i]
            if in_str:
                out.append(ch)
                if ch == '"':
                    in_str = False
                i += 1
            elif in_block:
                if (ch == '/' and i + 1 < n and line[i+1] == '/'
                        and (i == 0 or line[i-1] != '/')
                        and (i + 2 >= n or line[i+2] != '/')):
                    in_block = False
                    break
                else:
                    i += 1
            else:
                if ch == '"':
                    in_str = True
                    out.append(ch)
                    i += 1
                elif (ch == '/' and i + 1 < n and line[i+1] == '/'
                      and (i == 0 or line[i-1] != '/')
                      and (i + 2 >= n or line[i+2] != '/')):
                    in_block = True
                    i += 2
                else:
                    out.append(ch)
                    i += 1
        result.append((lineno, line, is_indented, ''.join(out).strip()))
    return result


_REG_PREFIXES = ('RA_', 'SC_', 'SAL_', 'S_')


def _expand_indent(stripped_lines: list, loader) -> list:
    """Expand indent-based syntax into flat .re lines.

    Uses a STACK of contexts for correct nesting. Each context records its
    opener_indent so closing is relative: a line with indent <= opener_indent
    closes the context (pops the stack).

    Indent levels within a context are RELATIVE to body_base:
      rel = current_indent_chars - body_base
      rel == 0  → new item at this context's level
      rel >  0  → sub-item (belongs to previous item)

    This means CHAIN works correctly inside SAVE/FUNC blocks.
    """
    result = []
    _ctx_stack: list = []
    _pending_restore: list = []
    _pending_norestore: bool = False

    def _flush_restore():
        nonlocal _pending_restore
        result.extend(_pending_restore)
        _pending_restore = []

    def _count_indent(raw: str) -> int:
        n = 0
        for ch in raw:
            if ch == '\t':  n += 4
            elif ch == ' ': n += 1
            else: break
        return n

    def _close_ctx(ctx, lineno=0):
        mode = ctx['mode']
        if mode == 'save':
            _flush_restore()
        elif mode == 'for':
            result.extend(_for_expand(ctx))

    # ── FOR v2: header/override parsing and expansion ────────────────────────
    def _for_parse_header(tokens):
        """Parse a FOR header's token list into a flat slot list:
        [(position, name_or_None), ...], 0-indexed. A purely-numeric token
        is a count block (N consecutive anonymous slots); any other token
        is one named slot. Every slot -- named or anonymous -- consumes one
        position in a single running count (a name is an alias for its
        position, not a separate addressing system)."""
        slots = []
        pos = 0
        for tok in tokens:
            if tok.isdigit():
                for _ in range(int(tok)):
                    slots.append((pos, None))
                    pos += 1
            else:
                slots.append((pos, tok))
                pos += 1
        return slots

    def _for_parse_selector_token(tok, by_name, total):
        """Return (set_of_positions, specificity) for one selector token.
        specificity: 0=name, 1=position/range. Raises ValueError if the
        token resolves to nothing -- loud failure, not a silent no-op."""
        if tok in by_name:
            return {by_name[tok]}, 0
        if '-' in tok:
            lo_s, _, hi_s = tok.partition('-')
            if lo_s.isdigit() and hi_s.isdigit():
                lo, hi = int(lo_s), int(hi_s)
                if lo > hi or lo < 0 or hi >= total:
                    raise ValueError(
                        f"FOR override: range '{tok}' out of bounds (0-{total-1})")
                return set(range(lo, hi + 1)), 1
        if tok.isdigit():
            p = int(tok)
            if p < 0 or p >= total:
                raise ValueError(
                    f"FOR override: position '{tok}' out of bounds (0-{total-1})")
            return {p}, 1
        raise ValueError(
            f"FOR override: '{tok}' is not a known element name or position in this FOR")

    def _for_parse_override_line(line, slots, order_idx):
        """Parse one override-block line into a list of
        (position, specificity, kv_dict, order_idx) entries -- one per
        slot the line's selector(s) resolve to, each carrying the line's
        key=value pairs. A line with no selector before '>' is a default:
        applies to every slot, at the lowest specificity (always beaten by
        a more specific line for the same slot/key)."""
        if '>' not in line:
            raise ValueError(f"FOR override line missing '>': {line!r}")
        sel_part, _, kv_part = line.partition('>')
        kv = {}
        for pair in kv_part.split():
            if '=' not in pair:
                raise ValueError(f"FOR override: bad key=value token {pair!r}")
            k, v = pair.split('=', 1)
            kv[k] = v
        sel_toks = sel_part.split()
        total = len(slots)
        if not sel_toks:
            return [(p, 2, kv, order_idx) for p, _name in slots]
        by_name = {name: p for p, name in slots if name is not None}
        entries = []
        for tok in sel_toks:
            positions, specificity = _for_parse_selector_token(tok, by_name, total)
            for p in positions:
                entries.append((p, specificity, kv, order_idx))
        return entries

    def _for_expand(ctx):
        """Expand a closed FOR context into final (lineno, text) pairs,
        applying the override hierarchy: most specific selector wins
        (name > position/range > default), tie-broken by last-in-text for
        equal specificity. A placeholder with no resolved value anywhere
        (no override, no default) is left as literal text in the output
        -- not substituted with an empty string -- so a forgotten value
        surfaces as a distinctive, traceable token instead of silently
        becoming a malformed but textually-unremarkable line."""
        slots = ctx['slots']
        all_entries = []
        for idx, (_lineno, line) in enumerate(ctx['override_lines']):
            all_entries.extend(_for_parse_override_line(line, slots, idx))

        out = []
        for pos, name in slots:
            known = {'N': str(pos)}
            if name is not None:
                known['X'] = name
            keys_here = {k for p, _s, kv, _o in all_entries if p == pos for k in kv}
            for key in keys_here:
                candidates = [(spec, oidx, kv[key]) for p, spec, kv, oidx in all_entries
                              if p == pos and key in kv]
                candidates.sort(key=lambda c: (c[0], -c[1]))
                known[key] = candidates[0][2]
            for lineno, body_line in ctx['body_lines']:
                expanded = body_line
                for key, val in known.items():
                    expanded = expanded.replace('{' + key + '}', val)
                out.append((lineno, expanded))
        return out

    # ── Unified name-mode helpers (used by CHAIN, SWITCH) ─────────────
    def _parse_iris_mode(tokens, macro_idx):
        """Return (block_name, anon).
        block_name=None, anon=False → manual (explicit names in body)
        block_name=None, anon=True  → fully anonymous (__prefix_N)
        block_name=NAME, anon=False → auto-named (IRIS_N)
        """
        if len(tokens) <= macro_idx:
            return None, False
        name = tokens[macro_idx]
        if name == '_':
            return None, True
        return name, False

    def _auto_name(block_name, anon, prefix, idx, line_stripped):
        """Build the ITO line for a body entry in auto-name mode.

        Naming convention: bare `block_name` is item 0 (no suffix at
        all); item 1 onward is `block_name` + index, no separator
        (`NAME1`, `NAME2`, ...). Matches the `{Y}`/`{Y1}`/`{Y2}`
        convention already established for FOR's override placeholders
        -- these are purely internal, arbitrary generated identifiers
        with no external convention to clash with.
        """
        if anon:
            return f'ITO __{prefix}_{idx} {line_stripped}'
        elif block_name is None:
            return f'ITO {line_stripped}'   # manual: first tok = name
        else:
            parts = line_stripped.split(None, 1)
            first_tok = parts[0]
            rest = parts[1] if len(parts) > 1 else ''
            suffix = '' if idx == 0 else str(idx)
            if first_tok.startswith('_') and len(first_tok) > 1:
                # _SUFFIX → NAME_SUFFIX (idx 0) / NAME_SUFFIX_{idx} (idx>0)
                lux_name = f'{block_name}_{first_tok[1:]}{"" if idx == 0 else f"_{idx}"}'
            else:
                # no underscore → NAME / NAME{idx}, first_tok is the op
                lux_name = f'{block_name}{suffix}'
                rest = f'{first_tok} {rest}'.strip()
            return f'ITO {lux_name} {rest}'

    for lineno, _raw, is_indented, stripped in stripped_lines:
        raw_stripped = _raw.rstrip('\n')
        cur_indent = _count_indent(raw_stripped)

        if not stripped:
            while _ctx_stack:
                _close_ctx(_ctx_stack.pop(), lineno)
            _flush_restore()
            continue

        # Pop contexts closed by this indent level
        while _ctx_stack and cur_indent <= _ctx_stack[-1]['opener_indent']:
            _close_ctx(_ctx_stack.pop(), lineno)

        # Dispatch to active context (top of stack)
        if _ctx_stack:
            ctx = _ctx_stack[-1]
            mode = ctx['mode']

            # First indented line sets body_base
            if ctx['body_base'] is None and cur_indent > ctx['opener_indent']:
                ctx['body_base'] = cur_indent

            body_base = ctx['body_base']
            if body_base is not None:
                rel = cur_indent - body_base  # 0=item, >0=sub-item

                if mode == 'chain':
                    if rel > 0:
                        first_tok = stripped.split(None, 1)[0].upper() if stripped else ''
                        if first_tok not in _SPECIAL_OPENERS:
                            result.append((lineno, stripped))
                            continue
                        # Special opener: fall through to main dispatch below.
                    else:
                        block_name = ctx['block_name']
                        anon       = ctx['anon']
                        idx        = ctx['idx']
                        per_item_nolink = ctx['per_item_nolink']
                        body_line = stripped
                        btoks = body_line.split(None, 1)
                        link_to_prev = bool(btoks) and btoks[0] == '-'
                        if link_to_prev:
                            if not per_item_nolink:
                                raise ValueError(
                                    f"CHAIN: '-' marker used at line {lineno} but "
                                    f"this block has no NOLINK modifier active -- "
                                    f"items already auto-link by default, so '-' "
                                    f"has nothing to override.")
                            body_line = btoks[1] if len(btoks) > 1 else ''
                        if per_item_nolink and not link_to_prev:
                            result.append((lineno, 'NOLINK'))
                        generated = _auto_name(block_name, anon, 'ch', idx, body_line)
                        result.append((lineno, generated))
                        if idx == 0 and ctx['own_name'] and anon and not ctx['aliased']:
                            gen_name = generated.split(None, 2)[1]
                            result.append((lineno, f'ALIAS {ctx["own_name"]} {gen_name}'))
                            ctx['aliased'] = True
                        ctx['idx'] = idx + 1
                        continue

                if mode == 'prepend':
                    parts = stripped.split(None, 1)
                    cmd  = parts[0]
                    rest = parts[1] if len(parts) > 1 else ''
                    result.append((lineno, f'{cmd} {ctx["name"]} {rest}'.rstrip()))
                    continue

                if mode == 'for':
                    # Collect; actual expansion happens in _close_ctx via
                    # _for_expand, since override lines (rel>0) are only
                    # fully known once the whole FOR block has been read.
                    if rel > 0:
                        ctx['override_lines'].append((lineno, stripped))
                    else:
                        ctx['body_lines'].append((lineno, stripped))
                    continue

                if mode == 'switch':
                    reg        = ctx['reg']
                    block_name = ctx.get('block_name')
                    idx        = ctx.get('idx', 0)
                    toks_s = stripped.split()
                    if '>' in toks_s:
                        gt   = toks_s.index('>')
                        vals = toks_s[:gt]
                        dest = toks_s[gt + 1] if gt + 1 < len(toks_s) else ''
                    else:
                        if len(toks_s) < 2: continue
                        vals, dest = [toks_s[0]], toks_s[1]
                    if not dest: continue
                    for val in vals:
                        if block_name is not None:
                            jeq_name = f'{block_name}_{idx}'
                        else:
                            n = loader._switch_counter; loader._switch_counter += 1
                            jeq_name = f'__sw_{n}'
                        result.append((lineno, f'JEQ {jeq_name} {reg} {val} {dest}'))
                        ctx['idx'] = idx + 1
                        idx += 1
                    continue

                if mode == 'save':
                    pass  # fall through — body passes through, but inner contexts handle themselves

        # ── Non-context line (or save/func passthrough) ───────────────────────
        toks = stripped.split()
        first = toks[0].upper() if toks else ''

        if first == 'ALLOC_RAW' and len(toks) >= 3:
            if len(toks) >= 4:
                # ALLOC_RAW name size target -- caller wants the first
                # generated instruction to be a specific, real jump target
                # (e.g. needs to be addressable from elsewhere) instead of
                # an anonymous __ar_N. Mirrors SAVE's entry_name.
                first_name, size_sym, target = toks[1], toks[2], toks[3]
            else:
                first_name, size_sym, target = None, toks[1], toks[2]
            n = loader._alloc_counter; loader._alloc_counter += 1
            ito_name = first_name if first_name else f'__ar_{n}'
            result.append((lineno, f'ITO {ito_name}   Move El1={size_sym} Exit=RA_ALLOC_COUNT'))
            result.append((lineno, f'RVOCA __ar_{n}_j ALLOC_RAW'))
            result.append((lineno, f'ITO __ar_{n}_k Move El1=RA_ALLOC_RESULT Exit={target}'))
            continue

        if first == 'NORESTORE':
            _pending_norestore = True; continue

        if first == 'SWITCH':
            if len(toks) < 2: continue
            reg = toks[1]
            # SWITCH reg        → anonymous JEQs (__sw_N)
            # SWITCH reg NAME   → named JEQs (IRIS_N)
            block_name = toks[2] if len(toks) >= 3 else None
            _ctx_stack.append({'mode': 'switch', 'reg': reg,
                               'block_name': block_name, 'anon': block_name is None,
                               'idx': 0,
                               'opener_indent': cur_indent, 'body_base': None})
            continue

        if first == 'CHAIN':
            # CHAIN [name-form] | [modifier modifier ...]
            # name-form (NAME / _ / absent) before '|', exactly as before;
            # modifiers after '|', space-separated, order-independent.
            rest_toks = toks[1:]
            if '|' in rest_toks:
                pipe_idx = rest_toks.index('|')
                name_toks = rest_toks[:pipe_idx]
                modifiers = set(t.upper() for t in rest_toks[pipe_idx + 1:])
            else:
                name_toks = rest_toks
                modifiers = set()
            block_name, anon = _parse_iris_mode(name_toks, 0)
            own_name = block_name  # preserved even if ANON clears block_name
                                    # below, for the NAME | ANON alias case
            if 'ANON' in modifiers:
                anon = True
                block_name = None
            result.append((lineno, 'NOLINK'))
            _ctx_stack.append({'mode': 'chain', 'block_name': block_name,
                               'anon': anon, 'idx': 0,
                               'own_name': own_name,
                               'per_item_nolink': 'NOLINK' in modifiers,
                               'aliased': False,
                               'opener_indent': cur_indent, 'body_base': None})
            continue

        if first == 'SAVE' and len(toks) >= 2:
            reg_tokens = toks[1:]
            norestore = _pending_norestore; _pending_norestore = False
            entry_name = None
            if reg_tokens and not any(reg_tokens[0].startswith(p) for p in _REG_PREFIXES):
                entry_name, reg_tokens = reg_tokens[0], reg_tokens[1:]
            # RA_LINK is preserved automatically by the call stack (Voca/Redi
            # push/pop on RA_SP) -- it is never a valid manual-save target.
            # Strip it unconditionally rather than relying on callers to omit
            # it correctly.
            regs = [r for r in reg_tokens if r != 'RA_LINK']
            if not regs:
                # Nothing left to save. SAVE's only remaining "value" would be
                # naming the entry point -- but that's better done directly on
                # the body's own first real instruction (zero extra
                # instructions, no indirection) than via a synthetic
                # placeholder here. Fail loudly rather than silently
                # inventing one: fix it at the source, the same way
                # RTB_LUX_BODY/P0_COLLECT_BODY/PRELOAD_ARG were fixed.
                if entry_name:
                    raise ValueError(
                        f"line {lineno}: 'SAVE {entry_name}' has no registers "
                        f"left to save (RA_LINK doesn't count -- the call "
                        f"stack already covers it). Name the body's own "
                        f"first real instruction '{entry_name}' directly "
                        f"instead of wrapping it in SAVE.")
                continue
            for reg in regs:
                result.append((lineno, f'NEW S_{reg}'))
            for i, reg in enumerate(regs):
                n = loader._alloc_counter; loader._alloc_counter += 1
                ito_name = entry_name if (i == 0 and entry_name) else f'__sv_{n}'
                result.append((lineno, f'ITO {ito_name} Move El1={reg} Exit=S_{reg}'))
            if not norestore:
                for reg in regs:
                    n = loader._alloc_counter; loader._alloc_counter += 1
                    _pending_restore.append((lineno, f'ITO __sv_{n} Move El1=S_{reg} Exit={reg}'))
            _ctx_stack.append({'mode': 'save', 'regs': regs,
                               'opener_indent': cur_indent, 'body_base': None})
            continue

        # WRITE_OUT NAME VAL → ITO NAME Write El1=LD_NI_OUT El2=VAL
        #                      ITO IRIS_i Add El1=LD_NI_OUT El2=C_1 Exit=LD_NI_OUT
        if first == 'WRITE_OUT' and len(toks) >= 3:
            n, val = toks[1], toks[2]
            result.append((lineno, f'ITO {n}   Write El1=LD_NI_OUT  El2={val}'))
            result.append((lineno, f'ITO {n}_i Add   El1=LD_NI_OUT  El2=C_1  Exit=LD_NI_OUT'))
            continue

        # READ_BODY NAME DST → ITO NAME Read El1=LD_NI_PTR Exit=DST
        #                      ITO IRIS_i Add El1=LD_NI_PTR El2=C_1 Exit=LD_NI_PTR
        if first == 'READ_BODY' and len(toks) >= 3:
            n, dst = toks[1], toks[2]
            result.append((lineno, f'ITO {n}   Read  El1=LD_NI_PTR  Exit={dst}'))
            result.append((lineno, f'ITO {n}_i Add   El1=LD_NI_PTR  El2=C_1  Exit=LD_NI_PTR'))
            continue

        result.append((lineno, stripped))

        # FOR elem0 elem1 ... — iteration with {N}/{X} substitution in body
        # (and optional count blocks / override block -- see FOR_V2_DESIGN.md)
        parts = stripped.split()
        if parts[0].upper() == 'FOR' and len(parts) >= 2:
            slots = _for_parse_header(parts[1:])
            _ctx_stack.append({'mode': 'for', 'slots': slots,
                               'body_lines': [], 'override_lines': [],
                               'opener_indent': cur_indent, 'body_base': None})
            continue

        # Universal prepend-mode leader
        parts = stripped.split()
        if len(parts) >= 2:
            first_up = parts[0].upper()
            entry = _INDENT_LEADERS.get(first_up)
            if entry is not None:
                mode, name_idx = entry
                if len(parts) > name_idx:
                    _ctx_stack.append({'mode': mode, 'name': parts[name_idx],
                                      'opener_indent': cur_indent, 'body_base': None})
            else:
                _ctx_stack.append({'mode': 'prepend', 'name': parts[1],
                                  'opener_indent': cur_indent, 'body_base': None})

    # EOF: close all remaining contexts
    while _ctx_stack:
        _close_ctx(_ctx_stack.pop(), 0)
    _flush_restore()
    return result


def _read_re_file(filepath: str, loader=None) -> list:
    """Read a .re file and return [(lineno, line), ...].

    Pipeline: read raw lines → strip //...// comments → expand indent syntax.
    loader is required for counter state (_switch_counter, _alloc_counter).
    If loader is None (legacy callers like diag.py), a dummy counter object is used.
    """
    try:
        mtime = os.stat(filepath).st_mtime_ns
        cache_key = (filepath, mtime)
        cached_stripped = _strip_comments_cache.get(cache_key)
        if cached_stripped is None:
            with open(filepath, encoding='utf-8', errors='replace') as f:
                raw_lines = f.readlines()
            cached_stripped = _strip_comments(raw_lines)
            _strip_comments_cache[cache_key] = cached_stripped
        stripped = cached_stripped
    except OSError:
        return []

    class _Counters:
        _switch_counter = 0
        _alloc_counter  = 0

    # RCALL/RRET: REMOVED (macros no longer exist, zero .re callers — see
    # macros.re's "RCALL/RRET: REMOVED" note). No expansion needed.
    expanded = [(lineno, raw, is_indented, text)
                for lineno, raw, is_indented, text in stripped]

    return _expand_indent(expanded, loader if loader is not None else _Counters())


def _parse_line(line: str) -> tuple | None:
    """Parse one raw .re line.

    Canon rule: the loader splits on whitespace. The first token is the
    command (uppercased). Unrecognised commands — or lines whose first
    token is not a known keyword — are silently skipped; the rest of the
    parts after a known command are consumed until an unrecognised token
    is hit (Key=Val pairs stop at the first non-pair).

    No comment stripping is done here. In Reca, a line that starts with
    an unrecognised token is simply ignored (it IS the comment). Content
    after a recognisable prefix is also dropped by each command's own
    element parser when it hits an unexpected token.

    Multi-line /.../ block comments are an aria feature (comments.re),
    not a core loader concern. The loader itself has no concept of
    comment syntax.

    Returns (cmd_upper, parts, raw_line) or None if line is empty.
    """
    parts = line.split()
    if not parts:
        return None
    return parts[0].upper(), parts, line




class LoadError(Exception):
    pass


def _is_integer(s: str) -> bool:
    if not s: return False
    c = s[0]
    return (c.isdigit() or c == '-') and (len(s) == 1 or s[1:].isdigit() or
            (c == '-' and len(s) > 1 and s[1:].isdigit()))


class Loader:
    __slots__ = (
        "interp", "symbols",
        "_loaded_files", "_lux_registry", "_next_file_id",
        "_last_ito",
        "_data_addrs",
        "_ito_addrs",
        "_ito_addrs_set",
        "_lumen_prepass",
        "_lumen_count",      # dict[src_addr, int] — O(1) lumen append
        "_ito_names",        # set of names that appear as RVOCA/RREDI/ITO targets
                              # anywhere (global prepass) — these always need the
                              # full ITO_SIZE layout, regardless of which command
                              # sees the name FIRST during allocation.
        "_ito_naming_commands",  # cache: set of macro names discovered (from
                              # macros.re itself) to use this same convention —
                              # see _discover_ito_naming_commands.
        "_bump",
        "_aether_size",   # cached len(aether) for _bump_alloc hot path
        "_water",
        "_inscriptions",
        "_inscr_seq",
        "_switch_counter",  # counter for unique SWITCH/JEQ lux names
        "_alloc_counter",   # counter for unique SAVE/ALLOC_RAW lux names
        "_lit_const_cache", # dict[int value, addr] — memoized C_<N> literal registers
                              # minted on demand by _resolve for bare numeric
                              # operands that aren't an existing C_<N> constant.
    )

    def __init__(self, interp) -> None:
        self.interp        = interp
        self.symbols: dict = {}
        self._loaded_files = set()
        self._lux_registry = {}
        self._next_file_id = 0
        self._last_ito           = 0
        self._data_addrs   = []   # ordered list of all lux addresses (data + instr)
        self._ito_addrs  = []   # ordered list of instruction lux addresses
        self._ito_addrs_set: set = set()
        self._lumen_prepass: dict = {}    # {sym_name: [tgt_name, ...]} from global scan
        self._lumen_count: dict = {}       # {src_addr: n_lumens_written} for O(1) append
        self._ito_names: set = set()       # names needing full ITO layout (global scan)
        self._ito_naming_commands = None   # cache for _discover_ito_naming_commands()
        self._bump         = _BOOTSTRAP_FIRST_FREE
        self._aether_size  = len(self.interp.aether.aether)  # cached for _bump_alloc
        self._water        = _BOOTSTRAP_FIRST_FREE - 1
        self._inscriptions = []   # raw texts; materialised at freeze-time
        self._inscr_seq    = 0
        self._switch_counter = 0  # unique names for JEQ luces from SWITCH
        self._alloc_counter  = 0  # unique names for SAVE/ALLOC_RAW luces
        self._lit_const_cache: dict = {}

    # ── Allocation helpers ────────────────────────────────────────────────────

    def _bump_alloc(self, n: int) -> int:
        """
Bump-allocate n contiguous luces. Returns start address.

        This is the bootstrap allocator. Loader maintains its own cursor
        as a Python int. At freeze time the final values are written into
        K_CURSOR.word and K_WATERMARK.word so aria code (alloc.re) and
        the compiled binary (preamble.ll) can continue allocating from
        the same point.
        """
        ptr = self._bump
        end = ptr + n
        if end >= self._aether_size:
            print(f"HALT: out of Aether (alloc {n} at {ptr})", file=sys.stderr)
            raise SystemExit(1)
        self._bump = end
        if end - 1 > self._water:
            self._water = end - 1
        return ptr

    def _alloc_data(self, name: str = '') -> int:
        """Allocate data lux [word, rel0, exit0, rel1, exit1, ..., 0].
        Each lumen = 2 luces (rel, exit). Size = 1 + 2*n_lumens + 1 (terminator).
        Lumen count comes from global prepass (_lumen_prepass).
        The trailing 0 slot is the lumen-chain sentinel — explicitly allocated
        so that lumen-scanning code can safely read it without bounds checks.

        If name is anywhere the target of RVOCA/RREDI/ITO (per the global
        _ito_names prepass), it needs the full ITO_SIZE layout instead —
        delegate to _alloc_ito so the FIRST mention of the name (whatever
        command that happens to be: NEW, NEWREF's X or Y, NEWSET, ...)
        already reserves enough room. Without this, a later RVOCA/RREDI/ITO
        line for the same name would silently no-op against the existing
        (too-small) allocation — see _define's "already exists" guard — and
        the lux would overlap with whatever gets allocated right after it."""
        if name and name in self._ito_names:
            return self._alloc_ito(name)
        n = len(self._lumen_prepass.get(name, []))
        size = 1 + 2 * n + 1   # lux + 2*lumina + absence
        addr = self._bump_alloc(size)
        self._data_addrs.append(addr)
        return addr

    def _alloc_raw_block(self, n: int) -> int:
        """Allocate n contiguous raw luces. NOT registered in LUX_REGISTRY."""
        return self._bump_alloc(n)


    def _alloc_ito(self, name: str = '') -> int:
        """Allocate an ITO lux with compact fixed-slot layout.
        Slots 0-6: word, op, e1, e2, exit, next, pad (ITO_SIZE=7 luces).
        Slot 5 (next) written by _link_itos in pass2.
        Slot 7+ (extra lumina from LINK commands) appended here from _lumen_prepass.
        """
        n_extra = len(self._lumen_prepass.get(name, []))
        size = _ITO_SIZE + 2 * n_extra + (1 if n_extra else 0)  # +1 terminator after extra lumen
        addr = self._bump_alloc(size)
        self._data_addrs.append(addr)
        self._ito_addrs.append(addr)
        self._ito_addrs_set.add(addr)
        return addr


    def _write_registry(self, addrs: list, base_sym: str, size_sym: str) -> None:
        """Write a list of addresses into Aether as a contiguous block, update symbols."""
        a = self.interp.aether.aether
        n = len(addrs)
        base = self._bump_alloc(n) if n > 0 else 0
        for i, addr in enumerate(addrs):
            a[base + i] = addr
        for sym, val in ((base_sym, base), (size_sym, n)):
            s = self.symbols.get(sym, 0)
            if s: a[s + _WORD] = val


    def _add_lumen(self, src_addr: int, rel_addr: int, tgt_addr: int) -> None:
        """Append (rel_addr, tgt_addr) as next extra lumen of src_addr's lux.

        For ITO luces: _emit_ito sets lumen_count=3, so _add_lumen writes at
        slot 7 (ITO_SIZE) and beyond — the extra lumen area after the compact fields.
        For Data luces: lumen_count starts at 0, lumina append from slot 1 onward.
        """
        if not src_addr or not tgt_addr:
            return
        a = self.interp.aether.aether
        # ITO luces store compact fields in slots 0-6; extra lumina start at
        # slot 7 (ITO_SIZE).  _emit_ito normally seeds _lumen_count to
        # ITO_SIZE//LUMEN_PAIR=3 so subsequent _add_lumen calls land at [7]+.
        # But builder-class macros (RCALL_AT, LR, EMIT, …) are never expanded
        # by Python's Wave-B — their target lux is in _ito_addrs_set but
        # _lumen_count was never seeded, so _add_lumen would fall back to
        # count=0 and write at [1]/[2], clobbering op/e1.  Guard against that:
        if src_addr not in self._lumen_count and src_addr in self._ito_addrs_set:
            self._lumen_count[src_addr] = _ITO_SIZE // LUMEN_PAIR
        count = self._lumen_count.get(src_addr, 0)
        i = 1 + count * LUMEN_PAIR
        a[src_addr + i]     = rel_addr
        a[src_addr + i + 1] = tgt_addr
        self._lumen_count[src_addr] = count + 1


    def _define(self, name: str, alloc_fn) -> int:
        """Create or return a lux for name. alloc_fn() → addr."""
        if name in self.symbols:
            return self.symbols[name]
        addr = alloc_fn()
        self.symbols[name] = addr
        self.interp.aether.aether[addr + _WORD] = addr  # self-ref by default
        return addr


    def _define_data(self, name: str) -> int:
        return self._define(name, lambda: self._alloc_data(name))


    def _define_ito(self, name: str) -> int:
        addr = self._define(name, lambda: self._alloc_ito(name))
        self.interp.aether.aether[addr + _WORD] = addr  # ITO invariant: slot 0 = self-ref
        return addr


    def _link_itos(self, prev: int, nxt: int) -> None:
        """Write nxt into SLOT_NEXT (slot 5) of prev ITO lux — only when not physically adjacent.

        If nxt == prev + ITO_SIZE, the interpreter's fall-through (SLOT_NEXT=0 → pc+ITO_SIZE)
        handles it automatically. Writing is only needed for explicit non-adjacent links.
        NOLINK resets _last_ito to 0 so this is never called for isolated blocks.
        """
        if not prev or not nxt:
            return
        if nxt == prev + _ITO_SIZE:
            return  # adjacent: fall-through (SLOT_NEXT stays 0)
        self.interp.aether.aether[prev + _NEXT] = nxt


    def _pack_string(self, text: str) -> int:
        """Pack a string into a chain of 1-lux slots (8 bytes per word, NUL-terminated).

        Stride = 1 lux per 8-byte chunk — matches Reca BS_PACK_TOKBUF (BPT_DSTINC +C_1).
        NUL terminator: extra lux with word=0.
        int.from_bytes is faster than manual shifts — built-in C function.
        """
        a   = self.interp.aether.aether
        raw = text.encode('utf-8')
        first = None
        prev_lux = None
        for i in range(0, len(raw), 8):
            chunk = raw[i:i+8]
            word  = int.from_bytes(chunk, 'little')  # NUL-padding automatic if chunk < 8
            lux  = self._alloc_raw_block(1)           # stride=1, matches Reca
            a[lux] = word
            if first is None:
                first = lux
            prev_lux = lux
        # NUL terminator — separate slot with word=0
        lux = self._alloc_raw_block(1)
        a[lux] = 0
        if first is None:
            first = lux
        return first


    def _record_inscription(self, text: str) -> None:
        """Queue free text for possible materialisation as an inscription.

        Whether the text actually becomes a Lux is decided at freeze
        time by `_finalize_inscriptions()`, which checks whether
        the `inscription.re` aria has registered the `INSCRIPTION`
        relation. If yes, every queued text becomes a Lux with a
        self-lumen to INSCRIPTION. If no, the queue is silently
        discarded — the canon never grows the Aether without an aria
        asking it to.

        Texts are buffered in pure Python (list of strings) — no
        Aether allocation happens here. This means recording is
        cheap and works during aspects.re's load (before any aria
        could exist), so first-loaded files don't lose their
        comments.

        Whitespace-only and empty texts are dropped — they carry no
        information.
        """
        text = text.strip()
        if not text:
            return
        # Buffer the raw text. Allocation happens in _finalize_inscriptions,
        # which knows whether the inscription aria is loaded.
        self._inscriptions.append(text)

    def _finalize_inscriptions(self) -> None:
        """Materialise queued inscriptions if the inscription aria is loaded.

        Called once after every yaku group has loaded. By that
        point, if `inscription.re` is among the loaded files, the
        INSCRIPTION relation will be in the symbol table. In that
        case every queued text gets allocated as a Lux, packed into
        a byte chain, and linked with INSCRIPTION via a self-lumen.

        If `inscription.re` was not loaded, the queue is dropped
        silently — the canon never inflates the Aether without an
        aria opting in.

        This is the event-driven hook: Python observes a symbol
        defined by an aria and changes its behaviour accordingly,
        without holding hardcoded knowledge of which aria did what.
        """
        pending = self._inscriptions
        self._inscriptions = []
        if not pending:
            return
        ins_rel = self.symbols.get("INSCRIPTION", 0)
        if not ins_rel:
            # No aria to receive — text is dropped.
            return
        a = self.interp.aether.aether
        for text in pending:
            # Allocate data lux with room for 1 lumen: [word, rel, exit, 0] = 4 luces.
            # _add_lumen below writes (ins_rel, addr) into slots 1 and 2; slot 3 stays 0.
            addr = self._alloc_raw_block(4)
            name = f"__ins_{self._inscr_seq}"
            self._inscr_seq += 1
            self.symbols[name] = addr
            self._data_addrs.append(addr)
            first = self._pack_string(text)
            a[addr + _WORD] = first
            self._add_lumen(addr, ins_rel, addr)



    def _run_pass_over(self, handler, label: str,
                       file_data: list, file_order=None) -> None:
        """Execute handler over every line of file_order (or file_data).

        Resets _last_ito between files so auto-link chains don't bleed.
        Called by _run_waves via lambda — lives here so it isn't duplicated
        inside every calling method.
        """
        order = file_order if file_order is not None else file_data
        for fp, file_id, start_ptr, lines in order:
            self._last_ito = 0
            for lineno, line in lines:
                try: handler(line, fp, lineno)
                except LoadError as e:
                    print(f"[{fp}:{lineno}] {label}: {e}", file=sys.stderr)
                except SystemExit: raise
                except Exception as e:
                    print(f"[{fp}:{lineno}] {label}: {type(e).__name__}: {e}",
                          file=sys.stderr)
            self._last_ito = 0

    def _run_waves(self, file_data: list, _run_pass) -> None:
        """Run all loading waves (A, A2, B0, B) over file_data, then finalise.

        Called by both auto_discover and _load_group so the wave protocol
        lives in exactly one place.
        """
        _run_pass(self._wave1a_line, "Wave-A")
        _run_pass(self._wave1_line,  "Wave-A2")
        self.interp.update_relations(self.symbols)
        macro_first = (
            [fd for fd in file_data if os.path.basename(fd[0]) == "macros.re"] +
            [fd for fd in file_data if os.path.basename(fd[0]) != "macros.re"]
        )
        # Wave-B0: NEWREF/SETREF first so macro entry points exist before Wave-B.
        _run_pass(self._wave_newref_line, "Wave-B0", file_order=macro_first)
        # Wave-B: wire everything; macro files first so their bodies are ready.
        _run_pass(self._wave2_line, "Wave-B", file_order=macro_first)

        for fp, file_id, start_ptr, lines in file_data:
            self._lux_registry[file_id] = {
                "name": os.path.basename(fp), "filepath": fp,
                "start_ptr": start_ptr, "end_ptr": self._bump,
            }
        self.interp.update_relations(self.symbols)
        # Register load aspects AFTER the final update_relations so they
        # are not overwritten by the dispatch rebuild.
        self._setup_load_aspects()

    def auto_discover(self, root_dir: str, exclude_seed: str = "aspects.re") -> None:
        """Discover all .re files and load them in two global phases.

        Wave A (all files): NEW/NEWREF/NEWSET/ITO/BLOCK — allocate all names.
        Wave B (all files): SET/LINK/NOLINK and Reca programs — wire everything.

        No SCC/Tarjan. Two-phase makes ordering irrelevant: all names exist
        before any wiring. Forward references resolved automatically.
        """

        seed_path = os.path.abspath(os.path.join(root_dir, exclude_seed))
        all_files = []
        for path in sorted(glob.glob(
                os.path.join(root_dir, '**', '*.re'), recursive=True)):
            abspath = os.path.abspath(path)
            if abspath == seed_path or abspath in self._loaded_files:
                continue
            all_files.append(abspath)
        if not all_files:
            return

        # Lumen prepass: count all LINK targets so luces can be sized correctly.
        # ITO-name prepass: collect every name that needs the full ITO_SIZE
        # layout (RVOCA/RREDI/ITO targets), so _alloc_data can give such names
        # the correct layout regardless of which command sees them first.
        lumen_prepass: dict = {}
        ito_names: set = set()
        if os.path.exists(seed_path):
            for src, tgt in self._scan_file(seed_path):
                lumen_prepass.setdefault(src, []).append(tgt)
            ito_names |= self._scan_ito_names(seed_path)
        for fp in all_files:
            for src, tgt in self._scan_file(fp):
                lumen_prepass.setdefault(src, []).append(tgt)
            ito_names |= self._scan_ito_names(fp)
        self._lumen_prepass = lumen_prepass
        self._ito_names |= ito_names

        # Read all files once, cache lines.
        file_data = []
        for fp in all_files:
            if fp in self._loaded_files:
                continue
            self._loaded_files.add(fp)
            file_id   = self._next_file_id; self._next_file_id += 1
            start_ptr = self._bump
            lines = _read_re_file(fp, self)
            file_data.append((fp, file_id, start_ptr, lines))

        if not file_data:
            return

        self._run_waves(file_data,
                        lambda handler, label, file_order=None:
                            self._run_pass_over(handler, label, file_data, file_order))

    def _load_group(self, filepaths):
        """Load a list of files using the two-phase protocol."""
        lumen_prepass: dict = {}
        ito_names: set = set()
        file_data = []
        for filepath in sorted(filepaths):
            if not os.path.exists(filepath):
                continue
            canonical = os.path.abspath(filepath)
            if canonical in self._loaded_files:
                continue
            self._loaded_files.add(canonical)
            file_id   = self._next_file_id; self._next_file_id += 1
            start_ptr = self._bump
            for src, tgt in self._scan_file(filepath):
                lumen_prepass.setdefault(src, []).append(tgt)
            ito_names |= self._scan_ito_names(filepath)
            lines = _read_re_file(filepath, self)
            file_data.append((filepath, file_id, start_ptr, lines))
        if not file_data:
            return
        for k, vs in lumen_prepass.items():
            self._lumen_prepass.setdefault(k, []).extend(vs)
        self._ito_names |= ito_names

        self._run_waves(file_data,
                        lambda handler, label, file_order=None:
                            self._run_pass_over(handler, label, file_data, file_order))

    def load_file(self, filepath):
        if not os.path.exists(filepath):
            return False
        canonical = os.path.abspath(filepath)
        if canonical in self._loaded_files:
            return True
        self._load_group([filepath])
        return True

    # ── File scanning ─────────────────────────────────────────────────────────

    def _scan_file(self, filepath):
        """Scan one .re file. Returns list of (src_name, tgt_name) from LINK commands.
        Only used for lumen prepass — counting LINKs to size luces correctly.
        No dependency graph, no SCC.
        """
        links = []
        for _lineno, line in _read_re_file(filepath):
            parsed = _parse_line(line)
            if not parsed: continue
            cmd, parts, _ = parsed
            if cmd == 'LINK' and len(parts) >= 4:
                links.append((parts[1], parts[3]))
        return links

    def _discover_ito_naming_commands(self):
        """Scan macros.re itself to find every macro whose body, anywhere
        within its NEWREF...next-NEWREF span, writes its caller's first
        positional argument (RA_MA0) into RA_MC_LUX before a WRITE_ITO_SLOTS-
        style self-reference write -- i.e. 'ITO <label> Move El1=RA_MA0
        ... Exit=RA_MC_LUX'. This is the universal calling-convention
        signature for "this macro's first argument (parts[1] on the call
        site line) becomes the address of a new, self-referential ITO
        entry, so it needs the full ITO_SIZE=7 layout from its *first*
        mention, not whatever smaller layout a name picks up if first seen
        as a plain NEW/NEWREF/NEWSET argument".

        Discovering this automatically (rather than hand-maintaining a list
        of command names) means any future macro written the same way is
        covered with zero changes here. The previous hand-maintained list
        (RVOCA/RREDI/NOP/JZ/JEQ/CLEAR/ALLOC_TO) silently missed LR/LT/
        RCALL_AT/EMIT/EMITI/PUTBYTE/WALK_ONE/LINK_OP/WALK_ITO/UNLINK_OP/LX/LH
        -- all of which follow this exact convention -- which is what broke
        RTB_LUX_BODY (named via LR, referenced earlier in yaku.re as a
        plain register before its own definition) -- see BUGS.md.

        A macro's body is *not* required to use RA_MA0 at its own entry
        label specifically (WALK_ONE's entry label uses RA_MA3 for an
        internal sub-step; RA_MA0/"name" itself is only wired at a later
        instruction within the same body) -- so this walks the whole span
        between one NEWREF and the next, not just the first line.

        Cached on first call (macros.re doesn't change mid-run).
        """
        if self._ito_naming_commands is not None:
            return self._ito_naming_commands

        macros_path = os.path.join(_HERE, 'macros.re')
        commands = set()
        current_macro = None
        if os.path.exists(macros_path):
            for _lineno, line in _read_re_file(macros_path):
                parsed = _parse_line(line)
                if not parsed:
                    continue
                cmd, parts, _ = parsed
                if cmd == 'NEWREF' and len(parts) >= 2:
                    current_macro = parts[1]
                    continue
                # Old pattern: ITO <label> Move El1=RA_MA0 ... Exit=RA_MC_LUX
                if (current_macro and cmd == 'ITO' and len(parts) >= 5
                        and parts[2] == 'Move' and parts[3] == 'El1=RA_MA0'
                        and 'Exit=RA_MC_LUX' in parts):
                    commands.add(current_macro)
                # New pattern: WAVE <label> ... At=RA_MA0
                # WAVE-converted macros use At=RA_MA0 to mark the naming arg
                if (current_macro and cmd == 'WAVE' and len(parts) >= 3
                        and any(p == 'At=RA_MA0' for p in parts[3:])):
                    commands.add(current_macro)

        self._ito_naming_commands = commands
        return commands

    def _scan_ito_names(self, filepath):
        """Scan one .re file (post indent-expansion, same view every other wave
        sees). Returns the set of names that appear as the target of any
        macro discovered by _discover_ito_naming_commands (RVOCA, RREDI,
        ITO, JZ, JEQ, CLEAR, LR, LT, RCALL_AT, EMIT, EMITI, PUTBYTE, and any
        future macro following the same convention) -- i.e. names that need
        the full ITO_SIZE=7 layout.

        This exists because allocation otherwise happens incrementally, in
        file order: whichever command first mentions a name decides its lux
        size (_alloc_data's small layout vs _alloc_ito's full layout). A name
        that is first seen as a NEW/NEWREF/NEWSET argument but is *later*
        the target of one of these commands would otherwise be stuck with a
        small lux that a later definition silently fails to enlarge
        (_define's "already exists" guard returns the old, undersized
        address). Same root cause and same fix shape as the LINK lumen
        prepass above — both need the *global* picture before allocating.

        ITO itself is a special case handled directly below (it's the base
        primitive, not a NEWREF-defined macro, so it can't be discovered the
        same way). ALLOC_TO is also special-cased: beyond its own name, its
        macro expansion needs '_J'/'_K' suffix luces pre-sized too.

        JZ/JEQ/CLEAR/NOP/ALLOC_TO/etc. are included alongside RVOCA/RREDI/ITO
        because all of them are fast-pathed directly in Wave-B's
        _ito_lux/_wave2_line (not deferred to LOAD_MAIN), and _ito_lux has
        the exact same reallocation hazard: if a NEWREF alias elsewhere
        resolves the same name *before* Wave-B reaches its definition, the
        alias caches the stale Wave-1 stub address instead of the final one.
        """
        naming_commands = self._discover_ito_naming_commands()
        names = set()
        for _lineno, line in _read_re_file(filepath):
            parsed = _parse_line(line)
            if not parsed: continue
            cmd, parts, _ = parsed
            if cmd in ('ITO', 'WAVE') and len(parts) >= 3:
                name = parts[1]
                if '{' in name or any('{' in p for p in parts[2:]):
                    continue
                names.add(name)
                # WAVE also needs _lux/_op/_e1/_e2/_ex/_w suffixed nodes
                if cmd == 'WAVE':
                    # label itself needs ITO-size (may be ref_name of a NEWREF)
                    names.add(name)
                    has_at  = any(p.startswith('At=')   for p in parts[3:])
                    has_nxt = any(p.startswith('Next=') for p in parts[3:])
                    suffixes = ['_op','_e1','_e2','_ex','_w']
                    if not has_at:
                        suffixes = ['_lux'] + suffixes
                    if has_nxt:
                        suffixes += ['_nl', '_nlw']
                    for sfx in suffixes:
                        names.add(name + sfx)
            elif cmd == 'ALLOC_TO' and len(parts) >= 4:
                name = parts[1]
                if '{' in name or any('{' in p for p in parts[2:]):
                    continue
                names.add(name)
                names.add(name + '_J')
                names.add(name + '_K')
            elif cmd in naming_commands and len(parts) >= 2:
                name = parts[1]
                if '{' in name or (len(parts) >= 3 and '{' in parts[2]):
                    continue  # FOR-macro template body, not a real name yet
                names.add(name)
        return names

    def _primitive_defines(cmd, parts):
        """Names defined by primitive commands (NEW/NEWREF/NEWSET/ITO/BLOCK)."""
        if cmd in ("NEW", "NEWREF", "NEWSET") and len(parts) >= 2:
            yield parts[1]
        elif cmd == "BLOCK" and len(parts) >= 3:
            name = parts[1]
            try: count = int(parts[2])
            except ValueError: count = 0
            if count > 0:
                yield name
                base = name
                if '_' in name:
                    p, s = name.rsplit('_', 1)
                    if s.isdigit(): base = p
                for off in range(1, count):
                    yield f"{base}_{off:03d}"
        elif cmd == "ITO" and len(parts) >= 3:
            yield parts[1]

    @staticmethod

    def _primitive_refs(cmd, parts):
        """Names referenced by primitives (for dependency graph)."""
        if cmd == "SET" and len(parts) >= 2:
            # SET name value_or_ref — reference to name (and to ref if not a number)
            yield parts[1]
            if len(parts) >= 3 and not _is_integer(parts[2]) and '"' not in parts[2]:
                yield parts[2]
        elif cmd == "SETREF" and len(parts) >= 3:
            yield parts[1]; yield parts[2]
        elif cmd == "ALIAS" and len(parts) >= 3:
            yield parts[1]; yield parts[2]
        elif cmd in ("NEW", "NEWREF") and len(parts) >= 2:
            # NEWREF X Y — cross-ref: X depends on Y (only if Y is a valid identifier)
            if cmd == "NEWREF" and len(parts) >= 3:
                ref = parts[2]
                if not _is_integer(ref) and not ref.startswith('/') and '"' not in ref:
                    yield ref
        elif cmd == "NEWSET" and len(parts) >= 2:
            yield parts[1]
            if len(parts) >= 3 and not _is_integer(parts[2]) and '"' not in parts[2]:
                yield parts[2]
        elif cmd == "LINK" and len(parts) >= 4:
            for n in parts[1:4]: yield n
        elif cmd == "ITO" and len(parts) >= 3:
            yield parts[2]
            for pair in parts[3:]:
                if "=" not in pair: break
                k, v = pair.split("=", 1)
                if k in ("El1", "El2", "Exit", "Next"):
                    yield v


    def _wave1a_line(self, line, filepath, lineno):
        parsed = _parse_line(line)
        if not parsed:
            return
        cmd, parts, cmd_part = parsed

        # WAVE: pre-allocate the label and all generated suffix nodes as ITO luces.
        # This mirrors what ITO does in wave1a so that wave2 can wire them.
        if cmd == "WAVE" and len(parts) >= 3:
            label = parts[1]
            has_at  = any(p.startswith("At=")   for p in parts[3:])
            has_nxt = any(p.startswith("Next=") for p in parts[3:])
            nodes = [label]
            if not has_at:
                nodes.append(label + "_lux")
            nodes += [label + sfx for sfx in ("_op", "_e1", "_e2", "_ex", "_w")]
            if has_nxt:
                nodes += [label + "_nl", label + "_nlw"]
            for node in nodes:
                if node not in self.symbols:
                    addr = self._alloc_ito(node)
                    self.symbols[node] = addr
                    self.interp.aether.aether[addr + _WORD] = addr
            return

        if cmd not in _BOOTSTRAP_CMDS:
            return  # non-bootstrap → Reca macro, no pass1 allocation needed

        if cmd in ("NEW", "NEWREF") and len(parts) >= 2:
            # NEWREF X [Y] is matryoshka:
            #   NEWREF X   = NEW X + SETREF X X  (self-ref, word=addr(X))
            #   NEWREF X Y = NEW X + SETREF X Y  (cross-ref, word=addr(Y))
            # Pass1a handles the NEW part for both: allocate if absent.
            name = parts[1]
            if name not in self.symbols:
                addr = self._alloc_data(name)
                self.symbols[name] = addr
            # NEWREF X Y: also pre-allocate Y so Wave B can wire X→Y.
            # Y is typically the first ITO of the function body (e.g. JEQ_START).
            if cmd == "NEWREF" and len(parts) >= 3:
                ref_name = parts[2]
                if (ref_name and not _is_integer(ref_name)
                        and not ref_name.startswith('/') and '"' not in ref_name
                        and ref_name not in self.symbols):
                    ref_addr = self._alloc_data(ref_name)
                    self.symbols[ref_name] = ref_addr
                    self.interp.aether.aether[ref_addr + _WORD] = ref_addr  # self-ref invariant
            if cmd == "NEWREF":
                a = self.interp.aether.aether
                name_addr = self.symbols.get(parts[1])
                if name_addr:
                    # Y is a valid cross-ref target only if it looks like an identifier:
                    # not a number, not starting with '/' (old-style comment fragment),
                    # not a string literal. Bare NEWREF X falls back to self-ref.
                    ref_name = parts[2] if len(parts) >= 3 else None
                    if ref_name and not _is_integer(ref_name) and not ref_name.startswith('/') and '"' not in ref_name:
                        # NEWREF X Y — cross-ref: word(X) = addr(Y) if Y known
                        ref_addr = self.symbols.get(ref_name)
                        if ref_addr:
                            a[name_addr + _WORD] = ref_addr
                        # else: Y not yet defined, defer to pass2
                    else:
                        # NEWREF X — self-ref: word(X) = addr(X), done eagerly
                        # so aspects are self-referential before any pass2 wiring.
                        a[name_addr + _WORD] = name_addr
                return
            # Support inline: NEW X SETREF X Y — sets word(X) = addr(Y) immediately
            if len(parts) >= 5 and parts[2] == "SETREF":
                a = self.interp.aether.aether
                name_addr = self.symbols.get(parts[3])
                ref_name  = parts[4].split()[0]
                ref_addr  = self.symbols.get(ref_name)
                if name_addr and ref_addr:
                    a[name_addr + _WORD] = ref_addr
            return

        if cmd == "NEWSET" and len(parts) >= 2:
            # NEWSET Name [value_or_string]
            # Matryoshka: NEW Name (allocate) + SET Name value (if given).
            # Pass1a handles allocation and integer SET; string SET deferred to pass2.
            name = parts[1]
            if name not in self.symbols:
                addr = self._alloc_data(name)
                self.symbols[name] = addr
            if len(parts) >= 3 and '"' not in cmd_part:
                try:
                    val = int(parts[2])
                    a = self.interp.aether.aether
                    a[self.symbols[name] + _WORD] = val & _MASK64
                except ValueError:
                    pass  # symbolic ref — handled in pass2
            return

        if cmd == "SET" and '"' not in cmd_part and len(parts) >= 3:
            try: val = int(parts[2])
            except ValueError: return
            name = parts[1]
            if name not in self.symbols:
                addr = self._alloc_data(name)
                self.symbols[name] = addr
            a = self.interp.aether.aether
            a[self.symbols[name] + _WORD] = val & _MASK64
            return

    # ── Pass 1b: allocate instruction / BLOCK Lux ─────────────────────────────


    def _wave1_line(self, line, filepath, lineno):
        parsed = _parse_line(line)
        if not parsed: return
        cmd, parts, _ = parsed
        if cmd in ("NEW", "NEWREF", "SET"): return

        # RVOCA/RREDI pre-allocate named ITO luces so Wave-B can wire them directly.
        if cmd in ("RVOCA", "RREDI") and len(parts) >= 2:
            name = parts[1]
            if name not in self.symbols or self.symbols[name] not in self._ito_addrs_set:
                # Not yet allocated, or was a data lux stub — (re-)allocate as proper ITO.
                self._define_ito(name)
            return

        # Unknown command (Reca macro call): pre-allocate identifier els as ITO luces.
        # Macros like JEQ write full 7-lux ITO into MA0, MA4, MA5 (all lux names).
        # Pre-allocating all identifier els as ITO avoids lux overlap.
        if cmd not in _BOOTSTRAP_CMDS and len(parts) >= 2:
            if (cmd.startswith('/') or cmd.startswith('"')
                    or cmd.startswith("'") or not cmd[0].isalpha()):
                return
            for name in parts[1:]:
                if (name and not _is_integer(name) and '"' not in name
                        and not name.startswith('/')
                        and (name[0].isupper() or name[0] == '_')
                        and name.replace('_','').isalnum()
                        and name not in self.symbols):
                    addr = self._alloc_ito(name)
                    self.symbols[name] = addr
                    self.interp.aether.aether[addr + _WORD] = addr
            return

        if cmd not in _BOOTSTRAP_CMDS: return  # macros handled by _pass2 via Reca

        if cmd == "BLOCK":
            if len(parts) < 3: return
            name = parts[1]
            if name in self.symbols: return
            count = self._resolve_block_count(parts[2], filepath, lineno)
            if count is None: return
            # BLOCK name N = NEW name + NEW name_001 + ... + NEW name_(N-1)
            # Matryoshka: BLOCK is N sequential NEWs of raw 2-lux slots.
            # Raw (not graph luces) because BLOCK is structural Aether —
            # jump tables, byte buffers, register files — not lumen graphs.
            first = self._alloc_raw_block(DATA_LUX_MIN)
            self.symbols[name] = first
            base = name
            if '_' in name:
                p, s = name.rsplit('_', 1)
                if s.isdigit(): base = p
            for off in range(1, count):
                addr = self._alloc_raw_block(DATA_LUX_MIN)
                alias = f"{base}_{off:03d}"
                if alias not in self.symbols:
                    self.symbols[alias] = addr
            return

        # Primitive ITO: allocate an instruction lux.
        if cmd == "ITO":
            if len(parts) < 3: return
            name = parts[1]
            a = self.interp.aether.aether
            if name in self.symbols:
                existing = self.symbols[name]
                # Check if it was pre-allocated as a 1-lux data stub (not a real ITO).
                # A real ITO has self-ref word and was allocated with _alloc_ito.
                # A data stub has word=addr but was only 1 lux — the next slots
                # belong to other objects. Re-allocate as proper ITO.
                if existing not in self._ito_addrs_set:
                    # Was a data lux — reallocate as proper ITO
                    addr = self._alloc_ito(name)
                    self.symbols[name] = addr
                    a[addr + _WORD] = addr
                else:
                    addr = existing
                    a[addr + _WORD] = addr
            else:
                addr = self._alloc_ito(name)
                self.symbols[name] = addr
                a[addr + _WORD] = addr
            return

        # Non-bootstrap commands are Reca macros — no pass1 allocation needed.


    def _setup_load_aspects(self) -> None:
        """Register the 1 load-time native op: __LT_ALLOC_ITO.

        __LT_ALLOC_ITO is used as the op-code of luces that need to
        bump-allocate an ITO block at load time. It is a true native op —
        Reca code sets a lux's op field to addr(__LT_ALLOC_ITO) and the
        interpreter calls this Python handler.

        ALLOC_LUCES is a Reca program (not a native op) — it runs normally
        through the interpreter. No registration needed.
        """
        sym      = self.symbols
        dispatch = self.interp._dispatch
        loader_ref = self

        lt_alloc_addr = sym.get("__LT_ALLOC_ITO", 0)
        ra_ma_ret     = sym.get("RA_MA_RET", 0)
        if lt_alloc_addr:
            def _lt_alloc_ito(a1, a2, exit, nxt, aether):
                addr = loader_ref._bump_alloc(_ITO_SIZE)
                aether[addr + _WORD] = addr
                if ra_ma_ret: aether[ra_ma_ret] = addr
                return nxt
            dispatch[lt_alloc_addr] = _lt_alloc_ito

    def _resolve_block_count(self, token, filepath, lineno):
        try: return int(token)
        except ValueError: pass
        addr = self.symbols.get(token)
        if addr is None:
            raise LoadError(f"BLOCK: '{token}' not defined")
        count = self.interp.aether.aether[addr + _WORD]
        if count <= 0:
            raise LoadError(f"BLOCK: count must be > 0, got {count}")
        return count

    # ── Pass 2: wire everything ───────────────────────────────────────────────

    def _resolve(self, name) -> int:
        """Resolve a name or integer literal to an address.

        A bare numeric literal (e.g. "0", "47") must resolve to the ADDRESS
        of a register holding that value, never the literal value itself —
        every ITO field is read by the interpreter through a dereference
        (aether[a1], aether[a2], aether[exit], ...), so storing a raw value
        directly in an e1/e2/exit slot makes the interpreter compare against
        whatever happens to live at that address instead of the intended
        constant (and, for the value 0 specifically, the interpreter's own
        truthiness-gated ops like Equal/Add treat a zero *address* as "no
        operand", silently skipping the operation rather than comparing).

        This is exactly the convention the codebase already uses by hand for
        the common case — e.g. the JZ macro never compares against a bare
        "0"; it uses RA_C0_REF, the address of the C_0 constant register —
        generalized here so it applies uniformly to any bare numeric operand
        resolved through this function (JEQ/Equal/Move/... operands written
        as plain digits, including ones synthesized by SWITCH-to-JEQ
        expansion in _expand_indent).

        Reuses the project's existing "C_<value>" named constant where one
        is already defined (the established convention, e.g. C_0, C_1,
        C_33); otherwise lazily allocates — and memoizes — a dedicated
        1-lux literal-constant register, so repeated uses of the same
        literal share one address rather than each minting a new lux.
        """
        if name in self.symbols:
            return self.symbols[name]
        try:
            v = int(name)
        except ValueError:
            raise LoadError(f"Unknown name: '{name}'")
        if v < 0:
            raise LoadError(f"Unknown name: '{name}'")
        c_name = f"C_{v}"
        if c_name in self.symbols:
            return self.symbols[c_name]
        if v in self._lit_const_cache:
            return self._lit_const_cache[v]
        addr = self._alloc_data(c_name)
        self.symbols[c_name] = addr
        self.interp.aether.aether[addr + _WORD] = v & _MASK64
        self._lit_const_cache[v] = addr
        return addr

    def _wire_word_value(self, addr: int, parts: list, cmd_part: str) -> None:
        """Write word(addr) from a value token — shared by SET and NEWSET Wave-B handlers.

        Supports:
          "string literal"  → packed string first-chunk addr
          123               → integer literal
          SYMBOL_NAME       → word(symbol)  (symbolic copy)
        """
        a = self.interp.aether.aether
        if '"' in cmd_part:
            q = cmd_part.find('"')
            text = cmd_part[q+1:cmd_part.rfind('"')]
            text = (text.replace("\\n", "\n").replace("\\t", "\t")
                        .replace("\\\\", "\\"))
            a[addr + _WORD] = self._pack_string(text)
        else:
            try:
                a[addr + _WORD] = int(parts[2]) & _MASK64
            except ValueError:
                ref = self._resolve(parts[2])
                a[addr + _WORD] = a[ref + _WORD]

    def _wire_ito_full(self, addr: int, op=0, e1=0, e2=0, exit=0, nxt=0):
        """Wire ITO lux with compact layout [word, op, e1, e2, exit, next, ...].
        Writes values directly to fixed slots — no rel slots.
        nxt written to SLOT_NEXT only if non-adjacent (adjacent = fall-through, SLOT_NEXT stays 0).
        """
        a = self.interp.aether.aether
        if op:   a[addr + _OP]   = op
        if e1:   a[addr + _E1]   = e1
        if e2:   a[addr + _E2]   = e2
        if exit: a[addr + _EXIT] = exit
        if nxt and nxt != addr + _ITO_SIZE:
            a[addr + _NEXT] = nxt
        # extra LINK lumina start at slot ITO_SIZE (=7); initial count = ITO_SIZE // LUMEN_PAIR
        self._lumen_count[addr] = _ITO_SIZE // LUMEN_PAIR

    def _emit_ito(self, name: str, op: str, *, e1=None, e2=None,
                    exit=None, auto_next=True) -> int:
        """Wire a named ITO lux with compact layout.
        Layout: [word=self, op, e1, e2, exit, next=0, ...extra_lumen..., 0]
        Values written to fixed slots — no rel slots. Next is at SLOT_NEXT (5).
        Returns the addr of this instruction.
        """
        addr    = self._resolve(name)
        op_addr = self._resolve(op)
        a       = self.interp.aether.aether
        a[addr + _OP]   = op_addr
        a[addr + _E1]   = self._resolve(e1)   if e1   is not None else 0
        a[addr + _E2]   = self._resolve(e2)   if e2   is not None else 0
        a[addr + _EXIT] = self._resolve(exit) if exit is not None else 0
        # SLOT_NEXT (5) stays 0 for adjacent luces (fall-through). _link_itos writes
        # only for non-adjacent explicit links.
        # lumen_count=3 → extra LINK lumina appended at addr+ITO_SIZE (slot 7).
        self._lumen_count[addr] = _ITO_SIZE // LUMEN_PAIR  # extra lumina start at slot ITO_SIZE
        if auto_next and self._last_ito:
            self._link_itos(self._last_ito, addr)
        self._last_ito = addr
        return addr

    def _wave_newref_line(self, line, filepath, lineno):
        """Wave-B0: wire only NEWREF/SETREF cross-refs.
        Must run before Wave-B so macro entry points are correct
        when _call_reca_macro executes macros during Wave-B.
        """
        parsed = _parse_line(line)
        if not parsed: return
        cmd, parts, _ = parsed
        a = self.interp.aether.aether

        if cmd == "NEWREF":
            if len(parts) < 2: return
            name_addr = self._resolve(parts[1])
            ref_name = parts[2] if len(parts) >= 3 else None
            if ref_name and not _is_integer(ref_name) and not ref_name.startswith('/') and '"' not in ref_name:
                ref_addr = self._resolve(ref_name)
                a[name_addr + _WORD] = ref_addr
            else:
                a[name_addr + _WORD] = name_addr
            return

        if cmd == "ALIAS":
            # ALIAS X Y — X becomes the *same address* as Y (true alias,
            # zero allocation), unlike SETREF/NEWREF which both allocate a
            # separate lux whose word field points at the target (one hop
            # of indirection via Voca's "read word, jump there" semantics).
            # A true alias behaves identically to the original name in
            # every context (Read/Write/Voca alike), not just when jumped
            # to -- the safer choice when the future use isn't fully known.
            if len(parts) < 3: return
            self.symbols[parts[1]] = self._resolve(parts[2])
            return

        if cmd == "SETREF":
            if len(parts) < 3: return
            name_addr = self._resolve(parts[1])
            ref_addr  = self._resolve(parts[2])
            a[name_addr + _WORD] = ref_addr
            return

    def _wave2_line(self, line, filepath, lineno):
        """Wave-B: wire operands, lumina, Next pointers."""
        parsed = _parse_line(line)
        if not parsed: return
        cmd, parts, cmd_part = parsed
        a = self.interp.aether.aether

        # ── Primitive commands ───────────────────────────────────────────────

        if cmd == "NEW":
            return  # already allocated in wave1a

        if cmd == "NEWSET":
            # NEWSET Name value_or_string — allocation done in Wave-A.
            # Wave-B handles string packing and symbolic references.
            if len(parts) < 3: return
            self._wire_word_value(self._resolve(parts[1]), parts, cmd_part)
            return

        if cmd == "NOLINK":
            # Python resets _last_ito=0 (breaks chain completely).
            # saku.re LOAD_CMD_NOLINK only clears RA_MC_PREV (preserves BS_LAST_ITO).
            # Difference: after a NOLINK block, Python won't autolink the next ITO
            # to the pre-NOLINK ITO. In practice this is fine because NOLINK blocks
            # are always self-contained — but this is a known divergence.
            self._last_ito = 0
            return

        if cmd == "SET":
            if len(parts) < 3: return
            self._wire_word_value(self._resolve(parts[1]), parts, cmd_part)
            return

        if cmd == "NEWREF":
            # NEWREF X [Y]
            #   NEWREF X   → word(X) = addr(X)  (self-ref, canonical for aspects)
            #   NEWREF X Y → word(X) = addr(Y)  (cross-ref, replaces NEW X + SETREF X Y)
            # Y must be a valid identifier — not a number, not a /comment fragment.
            if len(parts) < 2: return
            name_addr = self._resolve(parts[1])
            ref_name = parts[2] if len(parts) >= 3 else None
            if ref_name and not _is_integer(ref_name) and not ref_name.startswith('/') and '"' not in ref_name:
                ref_addr = self._resolve(ref_name)
                a[name_addr + _WORD] = ref_addr
            else:
                a[name_addr + _WORD] = name_addr  # self-ref
            return

        if cmd == "SETREF":
            if len(parts) < 3: return
            name_addr = self._resolve(parts[1])
            ref_addr  = self._resolve(parts[2])
            a[name_addr + _WORD] = ref_addr
            return

        if cmd == "LINK":
            if len(parts) < 4: return
            src = self._resolve(parts[1])
            rel = self._resolve(parts[2])
            tgt = self._resolve(parts[3])
            self._add_lumen(src, rel, tgt)
            return

        if cmd == "ITO":
            if len(parts) < 3: return
            name = parts[1]
            # Template names (contain '{') are FOR-macro bodies expanded at runtime — skip.
            if '{' in name or any('{' in p for p in parts[2:]): return
            op   = parts[2]
            kw: dict = {}
            for pair in parts[3:]:
                if "=" not in pair: break
                k, v = pair.split("=", 1)
                kw[k] = v
            is_term = op in _TERMINATORS
            self._emit_ito(
                name, op,
                e1=kw.get("El1"),   e2=kw.get("El2"),
                exit=kw.get("Exit"),
                auto_next=True,
            )
            if is_term:
                self._last_ito = 0
            return

        if cmd == "BLOCK":
            return  # allocated in Wave-A (_wave1a_line); no wiring needed

        # WAVE: synthesize an ITO at runtime.
        # WAVE label Op [El1=X] [El2=Y] [Exit=Z]
        # Mirrors ITO syntax. Expands to the standard builder sequence:
        #   __LT_ALLOC_ITO + 5 Move nodes + Voca WRITE_ITO_SLOTS
        # The label names the __LT_ALLOC_ITO node (the visible entry point).
        # Operands are _REF-style: addresses of luces holding the values.
        # Absent El2/Exit default to C_0 (zero/null), matching CLEAR semantics.
        if cmd == "WAVE" and len(parts) >= 3:
            label = parts[1]
            op    = parts[2]
            kw: dict = {}
            for pair in parts[3:]:
                if "=" not in pair: break
                k, v = pair.split("=", 1)
                kw[k] = v
            e1    = kw.get("El1")
            e2    = kw.get("El2")
            exit_ = kw.get("Exit")
            at    = kw.get("At")    # optional: reuse existing lux instead of allocating
            nxt   = kw.get("Next")   # optional: wire next-slot of synthesized ITO to this addr
            reset = kw.get("Reset")  # optional: use WRITE_ITO_SLOTS_RESET instead of WRITE_ITO_SLOTS

            # All operands passed as STRINGS to _emit_ito (which calls _resolve).
            # Passing integers would route through _resolve's C_N path, giving
            # the wrong address (e.g. _resolve(484) -> addr(C_484) != addr(Move)).

            if at:
                # At=X: reuse existing lux at X — skip __LT_ALLOC_ITO.
                # Move the at= address into RA_MC_LUX so WRITE_ITO_SLOTS knows the target.
                self._emit_ito(label,           "Move",  e1=at,          exit="RA_MC_LUX",      auto_next=True)
            else:
                # Default: allocate a fresh ITO slot at runtime.
                self._emit_ito(label,           "__LT_ALLOC_ITO",                               auto_next=True)
                self._emit_ito(label + "_lux",  "Move",  e1="RA_MA_RET", exit="RA_MC_LUX",      auto_next=True)
            self._emit_ito(label + "_op",   "Move",  e1=op,           exit="RA_MC_OP",       auto_next=True)
            self._emit_ito(label + "_e1",   "Move",  e1=e1 or "C_0",  exit="RA_MC_E1",       auto_next=True)
            self._emit_ito(label + "_e2",   "Move",  e1=e2 or "C_0",  exit="RA_MC_E2",       auto_next=True)
            self._emit_ito(label + "_ex",   "Move",  e1=exit_ or "C_0", exit="RA_MC_DEST",   auto_next=True)
            wis = "WRITE_ITO_SLOTS_RESET" if reset else "WRITE_ITO_SLOTS"
            if nxt:
                # Next=N: after WRITE_ITO_SLOTS, also wire the synthesized ITO's
                # next-slot to N. Emits: Add src+C_5→RA_MC_SLOT; Write slot←N.
                # Uses RA_MC_LUX which still holds the synthesized ITO's address.
                self._emit_ito(label + "_w",    "Voca",  e1=wis, exit="RA_LINK", auto_next=True)
                self._emit_ito(label + "_nl",   "Add",   e1="RA_MC_LUX", e2="C_5", exit="RA_MC_SLOT", auto_next=True)
                self._emit_ito(label + "_nlw",  "Write", e1="RA_MC_SLOT", e2=nxt,  auto_next=True)
            else:
                self._emit_ito(label + "_w",    "Voca",  e1=wis, exit="RA_LINK", auto_next=True)
            return

        # RVOCA/RREDI: pre-allocated ITO lux (name in symbols from pass1).
        # Wire it: pass lux addr as MA0, sub/RA_LINK target as MA1.
        # This allows the Reca RVOCA/RREDI program to fill in slots correctly.
        if cmd in ("RVOCA", "RREDI") and len(parts) >= 2:
            name = parts[1]
            # Template names from FOR macro bodies — skip during Wave-B
            if '{' in name or (len(parts) >= 3 and '{' in parts[2]): return
            lux_addr = self.symbols.get(name, 0)
            if not lux_addr:
                # Not pre-allocated — allocate now
                lux_addr = self._alloc_ito(name)
                self.symbols[name] = lux_addr
                self.interp.aether.aether[lux_addr + _WORD] = lux_addr
            # sub is parts[2] for RVOCA, absent for RREDI
            sub_addr = self.symbols.get(parts[2], 0) if len(parts) >= 3 else 0
            # Directly wire the lux
            a = self.interp.aether.aether
            op_addr = self.symbols.get("Voca" if cmd == "RVOCA" else "Redi", 0)
            ra_link = self.symbols.get("RA_LINK", 0)
            a[lux_addr + _WORD]  = lux_addr   # self-ref
            a[lux_addr + 1]      = op_addr      # op
            # RVOCA: e1=sub (the callee). RREDI: e1=RA_LINK (return via link register)
            a[lux_addr + 2]      = sub_addr if cmd == "RVOCA" else ra_link
            a[lux_addr + 3]      = 0            # e2
            a[lux_addr + 4]      = ra_link if cmd == "RVOCA" else 0  # exit: RVOCA needs RA_LINK; Redi does not use exit
            a[lux_addr + 5]      = 0            # next (autolink below)
            a[lux_addr + 6]      = 0            # lumen terminator
            # Autolink — only if not physically adjacent (fall-through handles adjacent)
            if self._last_ito and lux_addr != self._last_ito + _ITO_SIZE:
                a[self._last_ito + 5] = lux_addr
            self._last_ito = lux_addr
            if cmd == "RREDI":
                self._last_ito = 0  # RREDI resets chain (like NOLINK after)
            return

        # ── Macro commands: wire luces Python-side so LOAD_MAIN can execute ──
        # These macros are not in _BOOTSTRAP_CMDS but produce ITO luces that
        # must be wired before LOAD_MAIN runs. Without these, LOAD_MAIN encounters
        # op=0 luces and stops.

        def _ito_lux(name, op_name, e1=None, e2=None, exit=None):
            """Alloc or find ITO lux, wire it, autolink."""
            addr = self.symbols.get(name)
            if addr is None or addr not in self._ito_addrs_set:
                addr = self._alloc_ito(name)
                self.symbols[name] = addr
            op_addr = self.symbols.get(op_name, 0)
            a[addr + _WORD] = addr
            a[addr + 1] = op_addr
            a[addr + 2] = self._resolve(e1)   if e1   is not None else 0
            a[addr + 3] = self._resolve(e2)   if e2   is not None else 0
            a[addr + 4] = self._resolve(exit) if exit is not None else 0
            a[addr + 5] = 0
            a[addr + 6] = 0
            if self._last_ito and addr != self._last_ito + _ITO_SIZE:
                a[self._last_ito + 5] = addr
            self._last_ito = addr
            return addr

        # CLEAR name target  →  ITO name Move El1=C_0 Exit=target
        if cmd == "CLEAR" and len(parts) >= 3:
            _ito_lux(parts[1], "Move", e1="C_0", exit=parts[2])
            return

        # NOP name  →  ITO name Move El1=C_0 Exit=C_0
        if cmd == "NOP" and len(parts) >= 2:
            _ito_lux(parts[1], "Move", e1="C_0", exit="C_0")
            return

        # JEQ name reg1 reg2 dest
        # Expands matching LOAD_MAIN JEQ macro exactly (optimized, no _K NOP):
        #   Equal(name, e1=reg1, e2=reg2, exit=RA_JEQ_FLAG)
        #   JumpIf(_J,  e1=RA_JEQ_FLAG, exit=dest)
        # JumpIf's own slot 5 (next) is the fall-through point for autolink —
        # the next ordinary ITO in the file links into _J's next, which the
        # interpreter follows when the condition is false. No NOP lux needed.
        # Using RA_JEQ_FLAG (not a per-lux _F) makes LOAD_MAIN re-wire idempotent.
        if cmd == "JEQ" and len(parts) >= 5:
            name, reg1, reg2, dest = parts[1], parts[2], parts[3], parts[4]
            ji_name  = name + "_J"
            _ito_lux(name,     "Equal",   e1=reg1, e2=reg2, exit="RA_JEQ_FLAG")
            ji = _ito_lux(ji_name,  "JumpIf",  e1="RA_JEQ_FLAG", exit=dest)
            self._last_ito = ji
            return

        # JZ name reg dest
        # Expands matching LOAD_MAIN JZ macro (optimized, no _K NOP):
        #   Equal(name, e1=reg, e2=C_0, exit=RA_JEQ_FLAG)
        #   JumpIf(_J,  e1=RA_JEQ_FLAG, exit=dest)
        if cmd == "JZ" and len(parts) >= 4:
            name, reg, dest = parts[1], parts[2], parts[3]
            ji_name  = name + "_J"
            _ito_lux(name,     "Equal",   e1=reg, e2="C_0", exit="RA_JEQ_FLAG")
            ji = _ito_lux(ji_name,  "JumpIf",  e1="RA_JEQ_FLAG", exit=dest)
            self._last_ito = ji
            return

        # ALLOC_TO name dest count
        # Expands matching LOAD_MAIN ALLOC_TO macro: 3 chained ITOs —
        #   name   : Move(e1=count,            exit=RA_ALLOC_COUNT)
        #   name_J : Voca(e1=ALLOC_LUCES,       exit=RA_LINK)
        #   name_K : Move(e1=RA_ALLOC_RESULT,   exit=dest)
        # ALLOC_TO had no Python-side fast path (unlike JZ/JEQ/CLEAR/NOP/
        # RCALL_AT), yet it's used early — via SAVE, BS_INTERN's lazy-alloc
        # path, etc. — by files LOAD_MAIN processes long before it would
        # otherwise reach macros.re's own ALLOC_TO definition. Same
        # bootstrap chicken-and-egg problem the other fast-paths already
        # solve; ALLOC_TO was simply missing from that list.
        if cmd == "ALLOC_TO" and len(parts) >= 4:
            name, dest, count = parts[1], parts[2], parts[3]
            j_name = name + "_J"
            k_name = name + "_K"
            _ito_lux(name,   "Move", e1=count,              exit="RA_ALLOC_COUNT")
            _ito_lux(j_name, "Voca", e1="ALLOC_LUCES",       exit="RA_LINK")
            k = _ito_lux(k_name, "Move", e1="RA_ALLOC_RESULT", exit=dest)
            self._last_ito = k
            return

        # Unknown command: LOAD_MAIN Wave-B handles remaining macros.
        return

    # ── RO_* graph builder ────────────────────────────────────────────────────

    # ── Aether persistence ────────────────────────────────────────────────────

_HERE = os.path.dirname(os.path.abspath(__file__))
_BIN  = os.path.join(_HERE, "reca.bin")
_SYM  = os.path.join(_HERE, "reca.sym")


def _bin_is_stale() -> bool:
    """True if reca.bin/reca.sym are missing or older than any .re source
    file (or the bootstrap .py files that build them).

    Generalizes gen_compiler.py's _needs_regen() mtime-check pattern —
    same shape (target missing or older than a source → stale), applied
    to the frozen Aether binary instead of the generated .re artifacts.
    Used by load_or_freeze() so callers get a correct, fast thaw when
    nothing changed, and a correct, slower freeze() when something did,
    instead of every caller picking one of those two unconditionally.
    """
    if not (os.path.exists(_BIN) and os.path.exists(_SYM)):
        return True
    bin_mtime = os.path.getmtime(_BIN)
    sources = glob.glob(os.path.join(_HERE, '**', '*.re'), recursive=True)
    sources += [
        os.path.join(_HERE, 'loader.py'),
        os.path.join(_HERE, 'interpreter.py'),
        os.path.join(_HERE, 'symphony.py'),
    ]
    for s in sources:
        try:
            if os.path.getmtime(s) > bin_mtime:
                return True
        except OSError:
            continue
    return False


def discover_newref_macro_names(macros_path=None):
    """Every NEWREF-declared macro name in macros.re, as a set.

    Standalone, no Loader instance required -- shared by any diagnostic
    script that needs to know "is this name a real macro command" (e.g.
    classify_spans.py's swallowed-comment detector, diag.py's mode_macros
    coverage report) without each maintaining its own hand-written copy of
    this scan, which is exactly how those two drifted out of sync with the
    actual macro set in the past (see BUGS.md).
    """
    if macros_path is None:
        macros_path = os.path.join(_HERE, 'macros.re')
    names = set()
    if not os.path.exists(macros_path):
        return names
    for _lineno, line in _read_re_file(macros_path):
        parsed = _parse_line(line)
        if not parsed:
            continue
        cmd, parts, _ = parsed
        if cmd == 'NEWREF' and len(parts) >= 2:
            names.add(parts[1])
    return names


def freeze() -> "Loader":
    """Bootstrap Aether from .re sources.

    INTENTIONAL DUPLICATION: the Python loader and Reca LOAD_MAIN do the same thing.
    The Python layer is an honest bootstrap layer. General-purpose, extensible, no magic.
    Once LOAD_MAIN reliably builds the graph identically to Python — the Python layer is removed.
    Until then: Python is the source of truth, Reca runs in parallel.

    No special file lists, no magic bootstrap sets.
    Python loads everything through the standard Loader. LOAD_MAIN gets the same list.
    """
    from interpreter import Interpreter

    # Ensure generated artifacts (aria/layout.re, preamble.re, compiler_sf.re)
    # are up to date before loading .re files. gen_compiler.generate_artifacts()
    # only writes files — it does NOT call freeze() (no circular dependency).
    import gen_compiler as _gc
    if _gc._needs_regen():
        _gc.generate_artifacts(verbose=True)

    print("Reca freeze: Python loader (source of truth)...", file=sys.stderr)
    interp = Interpreter()
    loader = Loader(interp)

    # Python loads all files — fully, without restrictions.
    # aspects.re must load first (its 24 names are used as literal op= values
    # by everything else). Found by recursive filename search, not a
    # hardcoded path — the canonical location is core/aspects.re, but the
    # search itself doesn't assume any particular directory layout.
    seed_candidates = sorted(glob.glob(
        os.path.join(_HERE, '**', 'aspects.re'), recursive=True))
    if not seed_candidates:
        print("Error: aspects.re not found anywhere under project root", file=sys.stderr)
        sys.exit(1)
    seed = seed_candidates[0]
    if len(seed_candidates) > 1:
        print(f"Warning: multiple aspects.re found, using {seed}; "
              f"others ignored: {seed_candidates[1:]}", file=sys.stderr)
    if not loader.load_file(seed):
        print(f"Error: failed to load seed file {seed}", file=sys.stderr)
        sys.exit(1)
    loader.auto_discover(_HERE, exclude_seed=os.path.relpath(seed, _HERE))
    loader._finalize_inscriptions()
    interp.update_relations(loader.symbols)
    loader._setup_load_aspects()   # re-register after final update_relations

    # Reca LOAD_MAIN: a parallel pass over the same files.
    # Builds BS_HTAB independently. Not yet the source of truth.
    a = interp.aether.aether
    _all = sorted(
        p for p in glob.glob(os.path.join(_HERE, "**", "*.re"), recursive=True)
        if os.path.abspath(p) != os.path.abspath(seed)
    )
    # macros.re first — it defines SWITCH, ITO, RVOCA etc. needed by other files.
    # Bootstrap files are already wired by Python Wave-B; LOAD_MAIN Sweep-B skips them.
    # macros.re: Python Wave-B fully wires it; LOAD_MAIN skips it.
    # All other files: LOAD_MAIN processes them (Python Wave-B = scaffolding only).
    _rest = list(_all)
    all_files = _rest + [seed]

    ptr = loader._bump
    path_addrs = []
    for fpath in all_files:
        raw = fpath.encode("utf-8") + b"\x00"
        first_lux = None
        for i in range(0, len(raw), 8):
            chunk = raw[i:i+8].ljust(8, b"\x00")
            word = int.from_bytes(chunk, "little")
            a[ptr] = word
            if first_lux is None:
                first_lux = ptr
            ptr += 1
        path_addrs.append(first_lux)
    file_list_base = ptr
    for addr in path_addrs:
        a[ptr] = addr
        ptr += 1
    loader._bump = ptr

    bs_file_list  = loader.symbols.get("BS_FILE_LIST", 0)
    bs_file_count = loader.symbols.get("BS_FILE_COUNT", 0)
    if bs_file_list:  a[bs_file_list]  = file_list_base
    if bs_file_count: a[bs_file_count] = len(all_files)

    load_main_addr = loader.symbols.get("LOAD_MAIN", 0)
    if load_main_addr:
        # Bootstrap: set base pointers that BS_INIT would set at runtime.
        def _set_base(base_sym: str, block_sym: str):
            base_lux  = loader.symbols.get(base_sym, 0)
            block_addr = loader.symbols.get(block_sym, 0)
            if base_lux and block_addr:
                a[base_lux] = block_addr
        _set_base("BS_HT_BASE",       "BS_HTAB_000")
        _set_base("BS_TOKBUF_BASE",   "BS_TOKBUF_000")
        _set_base("BS_READBUF_BASE",  "BS_READBUF_000")

        # Clear htable
        ht_base_lux = loader.symbols.get("BS_HT_BASE", 0)
        ht_size_lux = loader.symbols.get("BS_HT_SIZE", 0)
        if ht_base_lux and ht_size_lux:
            ht_addr = a[ht_base_lux]
            ht_size = int(a[ht_size_lux])
            if ht_addr and ht_size:
                for i in range(ht_size):
                    a[ht_addr + i] = 0

        # Pre-populate runtime htable with all known symbols so LOAD_MAIN
        # can dispatch commands (NEWREF, ITO, etc.) and resolve refs via BS_LOOKUP.
        # Entry format: upper 32 bits = hash_low32, lower 32 bits = addr_low32.
        _ht_base_lux = loader.symbols.get("BS_HT_BASE", 0)
        _ht_size_lux = loader.symbols.get("BS_HT_SIZE", 0)
        _ht_mask_lux = loader.symbols.get("BS_HT_MASK", 0)
        if _ht_base_lux and _ht_size_lux:
            _ht_base_addr = a[_ht_base_lux]
            _ht_sz = int(a[_ht_size_lux])
            _ht_mask = int(a[_ht_mask_lux]) if _ht_mask_lux else (_ht_sz - 1)
            _MASK64 = (1 << 64) - 1
            _M32 = 0xFFFFFFFF

            def _djb2(b: bytes) -> int:
                h = 5381
                for x in b:
                    h = ((h * 33) + x) & _MASK64
                return h

            for _sym_name, _sym_addr in loader.symbols.items():
                if not _sym_addr or not isinstance(_sym_name, str):
                    continue
                # Skip individual BLOCK luces (name_NNN where NNN is 3+ digit numeric index).
                # Keeps C_0, C_1, C_32 etc. (single/double digit = constants, not block luces).
                # Block lux pattern: suffix is 3+ digits AND != '000' (base address).
                if '_' in _sym_name:
                    _sfx = _sym_name.rsplit('_', 1)[1]
                    if _sfx.isdigit() and len(_sfx) >= 3 and _sfx != '000':
                        continue
                _h = _djb2(_sym_name.encode("utf-8"))
                # Slot index: only the low bits (table-size mask) — matches the
                # "And RA_HT_HASH RA_HT_MASK" HT_LOOKUP/HT_INSERT do internally.
                _slot = _h & _ht_mask
                # Stored comparison value: the *wide* 32-bit hash (matches
                # BS_LOOKUP/BS_INTERN, which now mask to MASK_LOW32, not the
                # narrow table-size BS_HT_MASK — using the same narrow mask
                # for both the slot index AND the stored hash would make any
                # two names that collide on slot index indistinguishable from
                # a real match, since the "collision check" would be comparing
                # the same already-slot-masked value against itself).
                _h32 = _h & _M32
                _entry = (_h32 << 32) | (_sym_addr & _M32)
                for _ in range(_ht_sz):
                    _existing = a[_ht_base_addr + _slot]
                    if _existing == 0:
                        a[_ht_base_addr + _slot] = _entry
                        break
                    if (_existing & _M32) == (_sym_addr & _M32):
                        break  # already inserted
                    _slot = (_slot + 1) & _ht_mask

        # LUX_REGISTRY / ITO_REGISTRY: write contiguous address blocks before
        # K_CURSOR is frozen, so _bump_alloc moves the cursor correctly.
        lrb = loader.symbols.get("LUX_REGISTRY_BASE")
        lrs = loader.symbols.get("LUX_REGISTRY_SIZE")
        if lrb and lrs and loader._data_addrs:
            reg_base = loader._bump_alloc(len(loader._data_addrs))
            for i, addr in enumerate(loader._data_addrs):
                a[reg_base + i] = addr
            a[lrb] = reg_base
            a[lrs] = len(loader._data_addrs)

        irb = loader.symbols.get("ITO_REGISTRY_BASE")
        irs = loader.symbols.get("ITO_REGISTRY_SIZE")
        if irb and irs and loader._ito_addrs:
            ito_base = loader._bump_alloc(len(loader._ito_addrs))
            for i, addr in enumerate(loader._ito_addrs):
                a[ito_base + i] = addr
            a[irb] = ito_base
            a[irs] = len(loader._ito_addrs)

        # K_CURSOR/K_WATERMARK/K_TRACE_POS: double-deref pointer pattern.
        #   a[K_X] = K_X+1  (stable pointer)
        #   a[K_X+1] = actual value
        # Read El1=K_X → a[a[K_X]] = a[K_X+1] = value.
        k_cursor = loader.symbols.get("K_CURSOR", 0)
        if k_cursor:
            a[k_cursor] = k_cursor + 1
            a[k_cursor + 1] = loader._bump
        k_watermark = loader.symbols.get("K_WATERMARK", 0)
        if k_watermark:
            a[k_watermark] = k_watermark + 1
            a[k_watermark + 1] = loader._water
        k_trace = loader.symbols.get("K_TRACE_POS", 0)
        if k_trace:
            a[k_trace] = k_trace + 1
            a[k_trace + 1] = 0

        # Initialise call stack registers.
        # K_STACK_TOP: stable pointer lux (single-deref) — word = STACK_TOP index.
        # RA_SP: stack pointer register — word = initial SP = STACK_TOP.
        # RA_FRAME_SIZE: constant — word = FRAME_SIZE (8).
        # RA_STACK_GUARD: constant — word = STACK_BOTTOM.
        from symphony import STACK_TOP, STACK_BOTTOM, FRAME_SIZE, FLUX_BOTTOM, FLUX_TOP
        k_stack_top = loader.symbols.get("K_STACK_TOP", 0)
        if k_stack_top:
            a[k_stack_top] = STACK_TOP
        ra_sp = loader.symbols.get("RA_SP", 0)
        if ra_sp:
            a[ra_sp] = STACK_TOP
        ra_frame_size = loader.symbols.get("RA_FRAME_SIZE", 0)
        if ra_frame_size:
            a[ra_frame_size] = FRAME_SIZE
            # interp._ra_frame_size was cached by the earlier update_relations()
            # call (before this write existed) and would otherwise stay 0
            # forever — refresh it now that the real value is in place.
            interp._ra_frame_size = FRAME_SIZE
        ra_stack_guard = loader.symbols.get("RA_STACK_GUARD", 0)
        if ra_stack_guard:
            a[ra_stack_guard] = STACK_BOTTOM

        # Flux zone cursor and bounds.
        k_flux = loader.symbols.get("K_FLUX_CURSOR", 0)
        if k_flux:
            a[k_flux] = k_flux + 1
            a[k_flux + 1] = FLUX_BOTTOM
        k_flux_fl = loader.symbols.get("K_FLUX_FREELIST", 0)
        if k_flux_fl:
            a[k_flux_fl] = k_flux_fl + 1
            a[k_flux_fl + 1] = 0  # empty free-list
        ra_flux_bottom = loader.symbols.get("RA_FLUX_BOTTOM", 0)
        if ra_flux_bottom:
            a[ra_flux_bottom] = FLUX_BOTTOM
        ra_flux_top = loader.symbols.get("RA_FLUX_TOP", 0)
        if ra_flux_top:
            a[ra_flux_top] = FLUX_TOP

        # Restore self-referential flag luces — Wave-B macros may have written
        # runtime values into them. LOAD_MAIN needs Equal→JumpIf to use same lux.
        for _fn in ("RA_BS_FLAG", "LD_FLAG", "RA_JEQ_FLAG", "RA_FLAG", "RA_MC_FLAG"):
            _fa = loader.symbols.get(_fn, 0)
            if _fa: a[_fa] = _fa  # word = self-ref addr
        # RA_JEQ_FLAG_PTR: immutable pointer holding addr(RA_JEQ_FLAG).
        # JEQ/JZ macros write this value into Equal.exit/JumpIf.e1 slots.
        # Must be set AFTER RA_JEQ_FLAG self-ref is restored above.
        _jfp = loader.symbols.get("RA_JEQ_FLAG_PTR", 0)
        _jfa = loader.symbols.get("RA_JEQ_FLAG", 0)
        if _jfp and _jfa: a[_jfp] = _jfa  # word = addr(RA_JEQ_FLAG)

        # Restore BLOCK buffer self-refs — LOAD_MAIN clears them as part of buffer init.
        # Each BLOCK buffer lux[0] must equal its own address so Move reads the addr.
        for _bn in ("LD_LCOUNT_BUF_000", "LD_BACKFILL_BUF_000", "LD_BODY_BUF_000"):
            _ba = loader.symbols.get(_bn, 0)
            if _ba: a[_ba] = _ba  # restore self-ref

        print(f"Reca freeze: LOAD_MAIN ({len(all_files)} files, parallel)...",
              file=sys.stderr)
        _bump_pre = loader._bump

        # Minimal write protection: guard only system luces that must not change
        # during LOAD_MAIN. These are never legitimately written by .re commands.
        # K_CURSOR/K_WATERMARK/K_TRACE_POS: cursor double-deref integrity
        # BS_FILE_COUNT/BS_FILE_LIST: file list set by loader.py
        _protected_words: dict = {}
        for _sym, _val in [
            ("K_CURSOR",      k_cursor + 1 if k_cursor else 0),
            ("K_WATERMARK",   k_watermark + 1 if k_watermark else 0),
            ("K_TRACE_POS",   k_trace + 1 if k_trace else 0),
            ("BS_FILE_COUNT", len(all_files)),
            ("BS_FILE_LIST",  file_list_base),
        ]:
            _addr = loader.symbols.get(_sym, 0)
            if _addr:
                _protected_words[_addr] = _val

        _orig_write = interp._dispatch.get(a[loader.symbols.get("Write", 0)], None)

        def _protected_write(a1, a2, exit, nxt, aether):
            if a1 and aether[a1] in _protected_words:
                aether[aether[a1]] = _protected_words[aether[a1]]
                return nxt
            if _orig_write:
                return _orig_write(a1, a2, exit, nxt, aether)
            if a1 and a2:
                aether[aether[a1]] = aether[a2]
            return nxt

        _write_op_addr = a[loader.symbols.get("Write", 0)]
        if _write_op_addr and _protected_words:
            interp._dispatch[_write_op_addr] = _protected_write

        try:
            interp.execute_aether(load_main_addr, no_cache=True, max_steps=100_000_000)
            print("  LOAD_MAIN complete.", file=sys.stderr)
        except Exception as e:
            import traceback as _tb
            print(f"  LOAD_MAIN error: {type(e).__name__}: {e}", file=sys.stderr)
            _tb.print_exc()

            # Reconstruct symbols dict from intern htable (lux[1] = packed name)
            rebuilt = _rebuild_symbols_from_aether(loader, interp)
            if rebuilt:
                loader.symbols.update(rebuilt)
                print(f"  Symbols rebuilt from aether: {len(rebuilt)} names", file=sys.stderr)

    # LUX_REGISTRY and ITO_REGISTRY are built by LOAD_MAIN directly
    # (LOAD_REG_LUX/LOAD_REG_ITO subroutines + LOAD_FINALISE_REGISTRY).
    # Python _write_registry no longer needed.

    # Restore system registers that LOAD_MAIN's builder macros may have corrupted.
    # These are set by loader.py init, but JZ/SWITCH builders overwrite them if
    # the _J/_K ITOs happen to be allocated at the same address as system luces.
    _bs_fc = loader.symbols.get("BS_FILE_COUNT", 0)
    if _bs_fc: a[_bs_fc] = len(all_files)
    _bs_fl = loader.symbols.get("BS_FILE_LIST", 0)
    if _bs_fl: a[_bs_fl] = file_list_base

    # Restore command handler words: each command lux must point to its handler.
    # Any file can wire a cross-ref via "NEWREF X Y" before LOAD_MAIN; after
    # LOAD_MAIN we restore them since builder overwrites can corrupt them (if a
    # _J/_K ITO from some other macro's expansion happens to land at the same
    # address as a command word, it silently clobbers the word value, breaking
    # any later first-token dispatch to that command, or — for plain constant
    # cross-refs like C_7_REF in constants.re — silently reverting word(X) back
    # toward whatever LOAD_MAIN happened to leave there).
    #
    # Derived automatically from every "NEWREF X Y" pair in *every* .re file,
    # rather than a hand-maintained list of command names or a single
    # hardcoded source file. A hand-maintained list silently misses any
    # macro added later that's used as a direct line-starting command (this
    # is exactly what happened: EMIT/EMITI/ALLOC_TO/UNLINK_OP/LINK_OP are all
    # genuinely dispatched as commands elsewhere in the project -- 25/12/7/2/8
    # uses respectively -- but were absent from the old hand-maintained list).
    # Restricting the scan to macros.re alone is the same class of mistake at
    # the file level: any other file's NEWREF cross-refs (e.g. constants.re's
    # C_7_REF) are just as vulnerable to LOAD_MAIN corruption and deserve the
    # same restoration. Restoring unconditionally for every NEWREF pair in
    # every file is harmless for refs that are only ever resolved once,
    # statically, at a call site (their own word value is never read via
    # dispatch), and correct by construction for every ref that *is* read
    # through Voca-style "read word, jump there" indirection -- no need to
    # guess which file or which macro.
    for _re_path in all_files:
        if os.path.abspath(_re_path) == os.path.abspath(seed):
            continue  # aspects.re: no NEWREF cross-refs, self-ref handled separately
        for _lineno, _line in _read_re_file(_re_path):
            _parsed = _parse_line(_line)
            if not _parsed:
                continue
            _pcmd, _pparts, _ = _parsed
            if _pcmd == 'NEWREF' and len(_pparts) >= 3:
                _cmd  = loader.symbols.get(_pparts[1], 0)
                _hdlr = loader.symbols.get(_pparts[2], 0)
                if _cmd and _hdlr:
                    a[_cmd] = _hdlr

    k_cursor    = loader.symbols.get("K_CURSOR", 0)
    k_watermark = loader.symbols.get("K_WATERMARK", 0)
    k_trace     = loader.symbols.get("K_TRACE_POS", 0)
    # After LOAD_MAIN, read back cursor from K_CURSOR+1 (updated by ALLOC_LUCES).
    # Also restore K_X.word in case LOAD_MAIN builder macros corrupted it.
    if k_cursor:
        final_bump = a[k_cursor + 1]
        if final_bump and final_bump > loader._bump:
            loader._bump = final_bump
            loader._water = final_bump - 1
        a[k_cursor] = k_cursor + 1   # restore double-deref pointer
    if k_watermark:
        wm_val = a[k_watermark + 1]
        if wm_val and wm_val > loader._water:
            loader._water = wm_val
        a[k_watermark] = k_watermark + 1  # restore double-deref pointer
    if k_trace:
        a[k_trace + 1] = 0
        a[k_trace] = k_trace + 1     # restore double-deref pointer

    # Restore volatile registers to their canonical post-freeze values.
    # LOAD_MAIN leaves stale execution state in them; callers always set
    # these before use, so the frozen value is irrelevant — use clean defaults.
    ra_link_addr = loader.symbols.get("RA_LINK")
    if ra_link_addr:
        a[ra_link_addr] = 0          # RA_LINK: 0 = "no return address" default

    # K_LIMITER_ADOPTED: 1 if limiter.re was loaded (LIMITER_ACCORD in symbols).
    # Lets LIMITER_ADOPTED skip SCAN_ALL_LUX when limiter is not present — O(1).
    k_lim = loader.symbols.get("K_LIMITER_ADOPTED")
    if k_lim:
        a[k_lim] = 1 if "LIMITER_ACCORD" in loader.symbols else 0

    from symphony import STACK_TOP
    ra_sp_addr = loader.symbols.get("RA_SP")
    if ra_sp_addr:
        a[ra_sp_addr] = STACK_TOP    # RA_SP: reset to empty-stack position

    interp.aether.freeze(_BIN, loader._water)
    with open(_SYM, "w", encoding="utf-8") as f:
        for name, addr in sorted(loader.symbols.items(), key=lambda kv: kv[1]):
            f.write(f"{addr}\t{name}\n")

    used      = loader._water + 1
    n_syms    = len(loader.symbols)
    bin_size = os.path.getsize(_BIN)
    info      = interp.aether.read_header(_BIN)
    print(f"Frozen:  {used} luces touched, {info['luces_total']} live, "
          f"{info['segments']} segments, {n_syms} symbols", file=sys.stderr)
    print(f"Written: {_BIN}  ({bin_size//1024} KB, {info['format']})",
          file=sys.stderr)
    print(f"         {_SYM}  ({n_syms} lines)", file=sys.stderr)

    # Self-hosting progress: how many .re files did LOAD_MAIN complete?
    total_files = loader.symbols.get('BS_FILE_COUNT', 0)
    total_files = int(a[total_files]) if total_files else 0
    ld_fidx     = loader.symbols.get('SK_FIDX', 0)
    done_files  = int(a[ld_fidx]) if ld_fidx else 0
    if total_files:
        filled = int(20 * done_files / total_files)
        bar    = '█' * filled + '░' * (20 - filled)
        pct    = 100 * done_files // total_files
        print(f"Self-hosting: [{bar}] {done_files}/{total_files} files ({pct}%)",
              file=sys.stderr)

    return loader


def _read_packed_name(aether, lux: int) -> str:
    """Read a packed string stored at aether[lux] (stride=1, null-terminated).
    Alias for _decode_string_word — same format, kept for call-site clarity."""
    return _decode_string_word(aether, lux)


def _rebuild_symbols_from_aether(loader, interp) -> dict:
    """Reconstruct name→addr symbol dict by scanning the intern htable.
    Each intern lux has lux[1] = addr of dense packed-string name.
    Returns {name: intern_lux_addr}."""
    sym   = loader.symbols
    a     = interp.aether.aether

    ht_base_lux = sym.get("BS_HT_BASE", 0)
    ht_size_lux = sym.get("BS_HT_SIZE", 0)
    if not ht_base_lux or not ht_size_lux:
        return {}

    ht_base = a[ht_base_lux]
    ht_size = int(a[ht_size_lux])
    if not ht_base or not ht_size:
        return {}

    rebuilt: dict = {}
    alen = len(a)
    # Packed string addrs are allocated above the Wave-A bump (~200K+).
    # Plain BS_INTERN allocs 1 lux so lux[1] = a[intern+1] = 0 or some
    # unrelated value.  A valid packed-name ptr must be a reasonable address.
    min_ptr = ht_base + ht_size  # above htable region
    for i in range(ht_size):
        slot_val = a[ht_base + i]
        if not slot_val:
            continue
        # HT slot: upper 32 bits = hash, lower 32 bits = intern_lux_addr
        intern_addr = slot_val & 0xFFFFFFFF
        if not intern_addr:
            continue
        # lux[1] = packed string addr (only present for BS_INTERN_NAMED interns)
        if intern_addr + 1 >= alen:
            continue
        name_ptr = a[intern_addr + 1]
        if not name_ptr or name_ptr < min_ptr or name_ptr >= alen:
            continue
        name = _read_packed_name(a, name_ptr)
        if name:
            rebuilt[name] = intern_addr
    return rebuilt
def load_symbols() -> dict:
    if not os.path.exists(_SYM): return {}
    syms = {}
    with open(_SYM, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            addr_str, name = line.split("\t", 1)
            syms[name] = int(addr_str)
    return syms


def load_or_freeze() -> tuple:
    """Load from reca.bin+reca.sym if available and up to date, else run freeze().

    Returns (interp, symbols) where interp is a ready Interpreter and
    symbols is the name→addr dict. Used by trace.py, repl.py etc. to
    avoid re-freezing when a cached binary exists and is still fresh
    relative to every .re source file (see _bin_is_stale()).
    """
    from interpreter import Interpreter
    if not _bin_is_stale():
        interp = Interpreter()
        interp.aether.thaw(_BIN)
        symbols = load_symbols()
        interp.update_relations(symbols)
        return interp, symbols
    ldr = freeze()
    return ldr.interp, ldr.symbols


def fresh_loader(bin_path: str = _BIN, sym_path: str = _SYM) -> "Loader":
    """Create a fresh Loader on top of the frozen base state.

    Reads reca.bin + reca.sym, computes the correct bump from K_CURSOR,
    and sets up load-time aspects so Reca macros work when load_file() is
    called on additional .re files.

    This is the single canonical way to load user .re fragments on top of
    the frozen graph — used by repl.py --run, test_macros.py, and diag.py.
    """
    from interpreter import Interpreter
    interp = Interpreter()
    interp.aether.thaw(bin_path)
    syms = {}
    if os.path.exists(sym_path):
        with open(sym_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line: continue
                addr_str, name = line.split("\t", 1)
                syms[name] = int(addr_str)
    interp.update_relations(syms)
    ldr = Loader(interp)
    ldr.symbols = dict(syms)
    # Compute correct bump from K_CURSOR (the live allocation pointer)
    a = interp.aether.aether
    kc = syms.get("K_CURSOR", 0)
    cur_ptr = a[kc] if kc else 0
    ldr._bump = a[cur_ptr] if cur_ptr else (max(syms.values()) + 256 if syms else 1024)
    ldr._water = ldr._bump
    ldr._lumen_prepass = {}
    ldr._setup_load_aspects()
    return ldr


def check() -> None:
    from symphony import Aether
    if not os.path.exists(_BIN):
        print(f"Error: {_BIN} not found", file=sys.stderr); sys.exit(1)
    info = Aether.read_header(_BIN)
    print(f"OK: {_BIN}")
    print(f"   format:    {info['format']} (v{info['version']})")
    print(f"   luces:     {info['luces_total']} live")
    print(f"   segments:  {info['segments']}")
    print(f"   file size: {info['file_size']} bytes ({info['file_size']//1024} KB)")


if __name__ == "__main__":
    if "--check" in sys.argv: check()
    else: freeze()
