"""
loader.py — Reca bootstrap loader + Aether persistence

BOOTSTRAP ONLY: builds Aether from .re text files during freeze.
After reca.bin is written, this file is never called at runtime.

LUX MODEL:
  Two lux kinds share the same u64-lux Aether:

  NOVA (instruction) luces — compact fixed-slot layout:
    slot 0  = word  (self-ref: aether[addr] = addr)
    slot 1  = op    (primitive ID — dispatch key)
    slot 2  = e1    (first operand)
    slot 3  = e2    (second operand)
    slot 4  = exit  (destination: store target for Move/ALU, jump target for Jump/JumpIf,
                     RA_LINK address for Voca)
    slot 5  = next  (SLOT_NEXT: 0 = implicit fall-through pc+NOVA_SIZE; !0 = explicit link)
    slot 6  = pad   (last slot of the base structure when there are no extra
                     lumina — 0, as always. When there ARE extra lumina, this
                     same slot IS their first entry instead: extra LINK
                     lumina as (rel_addr, tgt_addr) pairs, 0-terminated,
                     start right here rather than after a separate always-
                     empty slot. Only consulted when SLOT_NEXT==0; an
                     explicit link/warp is unrelated. See interpreter.py's
                     _nova_extra_skip.)

  Data luces — arbitrary lumen pairs:
    slot 0  = word  (value)
    slot 1+ = lumen pairs (rel_addr, tgt_addr), 0-terminated

  NOVA and Data luces are distinguished by Op (slot 1): nonzero = NOVA, zero = Data.
  Extra lumina on NOVA luces start at slot 7 (NOVA_SIZE), appended by LINK commands.

GLOBAL LUMEN PREPASS:
  Before any allocation, all .re files are scanned for:
    1. Explicit LINK src rel tgt  → lumen (rel, exit) on src
    2. Implicit Next              → sequential NOVA luces linked by (Next, nxt)
       (same _last_nova tracking as pass2, NOLINK/RREDI/etc reset flow)
  Result: {sym_name: [tgt_name, ...]} tells _alloc_nova/_alloc_data the
  correct lux count for each lux before allocation happens.
"""
import os
import re
import sys
import glob
from collections import namedtuple

from aether import _MASK64, SLOT_WORD, SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, \
    NOVA_SIZE, SLOT_NEXT, SLOT_PAD, BOOTSTRAP_FIRST_FREE, LUMEN_PAIR, DATA_LUX_MIN, \
    DATA_LUMEN_OFFSET


# ── Indent stack ─────────────────────────────────────────────────────────────
# The one implementation of indent-level tracking for every macro body
# (FOR/SWITCH/SAVE/NOVAE/CHAIN/DEF...), used by _parse_indent_blocks.
# Rule: indentation in raw spaces (tab = 4); a stack of known levels starts at
# the opener's own indent. A line whose indent matches the top is at that
# level; one matching an earlier level returns there (deeper levels are
# forgotten); exactly +4 opens a new level; anything else is inert (skipped,
# stack unchanged). A genuinely blank line closes everything -- the caller
# handles that, not this class. Unit-tested by tools/test_indent_stack.py.

class IndentStackError(ValueError):
    pass


class IndentStack:
    """Tracks nested indent levels for one open macro body (FOR/SWITCH/
    SAVE/NOVAE/DEF/...). One instance per open context; the caller is
    responsible for creating one when a macro header opens and discarding
    it when the context is closed (by a blank line, per the caller's own
    logic -- not this class)."""

    STEP = 4  # one Tab on a real keyboard; matches _ITEM_INDENT_UNIT in loader.py

    def __init__(self, opener_indent: int):
        self._levels = [opener_indent]  # level 0 = the header line's own indent

    def classify(self, indent: int):
        """Returns (action, level) where action is one of:
          'same'  -- matches the current deepest level (level >= 1);
                     level = its index
          'return'-- matches an earlier, shallower level (level >= 1);
                     deeper levels forgotten; level = the matched index
          'deeper'-- exactly STEP more than current deepest; new level
                     pushed; level = its (new) index
          'skip'  -- doesn't match anything, OR matches level 0 (the
                     opener's own indent) -- level 0 is confirmed inert:
                     a line landing back on the opener's own indent is
                     treated identically to a stray/garbage indent, not
                     as a meaningful return. Caller does not need a
                     special case for level==0; both 'skip' and a
                     level==0 match collapse to the same "ignore this
                     line" outcome, so this method normalizes them to
                     'skip' with level=None before returning.
        Level index 0 is the opener's own line; real body content starts
        at level >= 1.
        """
        top = self._levels[-1]

        if indent == top:
            level = len(self._levels) - 1
            return ('skip', None) if level == 0 else ('same', level)

        if indent > top:
            if indent == top + self.STEP:
                self._levels.append(indent)
                return ('deeper', len(self._levels) - 1)
            return ('skip', None)

        # indent < top: look for an exact match among earlier (shallower) levels
        for i in range(len(self._levels) - 1, -1, -1):
            if self._levels[i] == indent:
                # forget everything deeper than the matched level
                del self._levels[i + 1:]
                return ('skip', None) if i == 0 else ('return', i)
        return ('skip', None)

    def depth(self) -> int:
        """Current deepest known level index."""
        return len(self._levels) - 1



# ── Source files ─────────────────────────────────────────────────────────────
# THE one definition of "the project's source files" and their order. Every
# Python pass, LOAD_MAIN's file list and every tool take the list from here,
# so no two places can ever disagree about which files exist or in what order
# they are read.

def re_sources(root: str = None) -> list:
    """Every .re file under `root` (default: the project root), found
    recursively and sorted by full path. The full path is unique, so the order
    is total and stable, and it does not depend on file names being unique
    across directories (two libraries may both have a commands.re)."""
    root = root or _HERE
    return sorted(glob.glob(os.path.join(root, '**', '*.re'), recursive=True))


def seed_candidates(root: str = None) -> list:
    """Every aspects.re in the project, in re_sources order. The seed loads
    first because its names are literal op= values for everything else."""
    return [p for p in re_sources(root) if os.path.basename(p) == 'aspects.re']


def load_main_order(seed: str, root: str = None) -> list:
    """LOAD_MAIN's file order: every source except the seed, in re_sources
    order, then the seed last (Python has already wired it)."""
    seed_abs = os.path.abspath(seed)
    return [p for p in re_sources(root) if os.path.abspath(p) != seed_abs] + [seed]


# Aliases for internal use in loader.py
_WORD     = SLOT_WORD
_OP       = SLOT_OP
_E1       = SLOT_E1
_E2       = SLOT_E2
_EXIT     = SLOT_EXIT
_NEXT     = SLOT_NEXT
_PAD      = SLOT_PAD
_NOVA_SIZE = NOVA_SIZE
_DATA_OFF = DATA_LUMEN_OFFSET
_BOOTSTRAP_FIRST_FREE = BOOTSTRAP_FIRST_FREE

# NOVA opcodes that unconditionally end a basic block (reset _last_nova auto-Next).
# Jump/JumpReg/End: direct terminators via NOVA command.
# Redi: handled by RREDI macro (which also resets _last_nova), but listed here
#        so a bare "NOVA X Redi" also terminates the flow.
_TERMINATORS = frozenset({"Jump", "JumpReg", "End", "Redi"})

# NOVA/NOVAE's own already-meaningful slot keys -- anything else in _slots
# on one of these lines is a real, arbitrary [Name][N]= extra-lumen
# relation (In1=/Op1=/Next1=/... or a genuinely new name), not a fixed
# field. Name itself never appears here: _split_args_slots already folds
# Name= back into args[0] before slots is built. Reset stays in args too
# (see NOVAE's own Reset= handling), not slots, for the same reason.
_NOVA_RESERVED_SLOTS = frozenset({"Op", "El1", "El2", "Exit", "At", "Next",
                                   "Op1", "Op2", "Op3", "Cmd", "Vis"})

# Commands that DO appear as name-minting in the source scan but must NOT be
# treated as "args[0] is a minted name": NEW/SET mint a name but are handled by
# their own branches; LINK/LOAD take a reference as args[0], not a definition.
# Subtracted from the discovered naming set wherever "does this command mint
# args[0]" is asked (Wave 1 sizing and the block-scope pre-scan).
_NON_NAMING_PRIMS = frozenset({'NEW', 'SET', 'LINK', 'LOAD'})

# Reserved built-in keys, recognised as Key=Value slots even before any .re file
# uses them (see _discover_key_vocabulary). Vis= = name visibility (Local/Public).
_BUILTIN_KEYS = frozenset({'Vis'})

# Macro dispatch: when loader sees unknown CMD, it looks up {CMD} in symbols.
# No registry needed — convention-based. Reca defines the logic, Python just calls.



# Indent-context modes (ctx['mode']), each opened by its own leader and
# handled in _parse_indent_blocks:
#   'switch' — body "<val> <dest>" / "<v1> <v2> > <dest>" -> SWITCH record
#   'save'   — body is the block -> SAVE record; its handler assembles
#              register-save + body + register-restore (_save_expand_records)
#   'def'   — named subroutine: entry lux takes the header name; RREDI NAME_RET
#              on close, addressable inside as {RET}
#   'for'    — body with {N}=index / {X}=element / override keys -> FOR record
#
# NEXO is deliberately not an opener: its indented body is raw LLVM-IR template
# text, not Reca commands, so NEXO_START (macros.re) reads its own body at the
# Reca level via the same token stream everything else already goes through.

# Special-opener tokens: first token of any line that opens a new indented
# context (pushes onto _ctx_stack or has its own dedicated dispatch path).
# Lines starting with these MUST NOT be swallowed by rel>0 raw passthrough
# inside CHAIN bodies -- they need to go through the main dispatch
# loop so they're correctly recognized and expanded regardless of where
# they appear in the source.
# Which commands open an indented block, and the parse-mode each uses, is not
# a hand-kept list here: it is discovered from the corpus's own `Open=<mode>`
# declarations (paired with `Cmd=<WORD>`), the same single-source approach as
# _discover_key_vocabulary. See _discover_block_openers / _special_openers
# below, and the _open_<mode> handlers just above _parse_indent_blocks. The
# same `Open=` key is what the self-hosted parser (parser.re) will read, so
# "what opens a block" has one answer for both implementations.

# The fixed number of indent-chars an "item" sits deeper than its own
# CHAIN/SAVE/FOR/SWITCH/NOVAE opener, project-wide, with zero exceptions
# (confirmed empirically). body_base is computed from this, not detected
# from whatever the block's first body line happens to use -- see the
# comment where it's set, in _parse_indent_blocks, for why that distinction
# matters.
_ITEM_INDENT_UNIT: int = 4


_strip_comments_cache: dict = {}  # (filepath, mtime) -> stripped result


def _cut_trailing_comment(line: str) -> str:
    """Remove a trailing /comment from a code line.

    A trailing comment starts at a lone slash (not a pair) that follows
    whitespace, outside any quoted string. Everything from there to the end
    of the line goes.

    Purely lexical: it does not know a single command, and must not. The
    only things it tracks are quotes (so `SET COMMENT_OPEN "//"` and
    `SET PATH "/aria/io.re"` survive) and the lone-vs-pair distinction (so
    `///` stays inert). Verified against every dangerous shape in the
    project before being turned on.

    Why it belongs here and not in the dispatcher: this is the same
    lexical question the rest of Wave 0 answers -- what is text and what is
    a comment. Leaving it to the dispatcher meant every wave re-read those
    bytes and threw them away by failing to parse them, which is where
    roughly 45% of all byte reads went.
    """
    out = []
    i = 0
    n = len(line)
    in_str = False
    while i < n:
        ch = line[i]
        if in_str:
            out.append(ch)
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if (ch == '/' and i > 0 and line[i-1] in ' \t'
                and (i + 1 >= n or line[i+1] != '/')):
            return ''.join(out).rstrip()
        out.append(ch)
        i += 1
    return ''.join(out).rstrip()


_key_vocab_cache: frozenset | None = None
_block_openers_cache: dict | None = None


def _discover_key_vocabulary() -> frozenset:
    """The real key= vocabulary, found by scanning every .re file's own
    tokens for the Key=Value shape — not a hand-typed list.

    If some file writes Body=X anywhere, Body IS a key; nothing else has
    to declare that separately. This is the same reasoning as
    _discover_known_commands (a command list mechanically extracted from
    what Wave 2 actually branches on, not hand-maintained) applied one
    level down, to the key= names inside a command's own argument list.

    Before this, the vocabulary was a hand-typed Python tuple, kept in
    step by hand with the Reca-side key recognizer (lexer.re's LOAD_ELEM,
    then a MATCH byte-tree) that does the identical job on the other side. They
    drifted — seven keys (Count/Width/In/Of/Value/Body/Rel) existed here
    but not there, so every self-hosted RVOCA/NOVA line whose second
    token used Body=/Op= (100% of them) silently misclassified. Scanning
    the actual corpus instead of hand-listing it means neither side can
    drift from what the files actually contain, because there is nothing
    left to keep in sync — both sides look at the same evidence.

    Cached: the corpus doesn't change mid-run.
    """
    global _key_vocab_cache
    if _key_vocab_cache is not None:
        return _key_vocab_cache
    # A real key= name has one shape everywhere in the corpus: one capital
    # letter, then zero or more lowercase letters/digits -- El1, Op, Body,
    # Count. Registers/scratch names (RA_MA0, SK_TMP) are the other thing
    # that legitimately sits left of an unquoted '=' in this corpus (in a
    # comment's own worked example, not a real key=value token), and they
    # never take this shape: ALL-CAPS with underscores. This is the
    # corpus's own established capitalisation convention, not a guessed
    # word list -- it is what already separates the two categories on
    # every line a human wrote, whether or not that line is live code.
    _KEY_SHAPE = re.compile(r'^[A-Z][a-z0-9]*$')
    keys: set = set()
    for path in re_sources():
        for _lineno, item in _read_re_file(path, expand_autogen=False):
            if isinstance(item, tuple):
                # A direct-build record (cmd, args, slots): its operand keys
                # live in the slots dict, not in any text token. Harvest the
                # real key-shaped ones so a command that carries a keyed operand
                # only in record form (PACK's Full=, MATCH's Except=) is
                # discovered exactly like a text-written key.
                _slots = item[2] if len(item) >= 3 else {}
                for k in _slots:
                    if _KEY_SHAPE.match(k):
                        keys.add(k)
                continue
            tokens = _tokenize_quoted(item)
            for a in tokens[1:]:
                if '=' in a and not a.startswith('"'):
                    k = a.split('=', 1)[0]
                    if _KEY_SHAPE.match(k):
                        keys.add(k)
    # Built-in language keys are recognised even before any file uses them.
    # Operand keys (El1/Body/Count) are conventions and rightly discovered from
    # usage; a core keyword like Vis= (name visibility) is part of the language
    # itself, so it cannot depend on already appearing somewhere to be parsed as
    # a key. Seeding it here (not a hand-list of every key -- just the reserved
    # ones) also propagates to the Reca lexer, which derives its recognizer from
    # this same vocabulary.
    _key_vocab_cache = keys | _BUILTIN_KEYS
    return _key_vocab_cache


# Matches an `Open=<mode>` declaration token (with its leading space) so it can
# be stripped from a line before the node builds -- see _read_re_file. Leading
# indentation is untouched: the pattern requires whitespace BEFORE Open=, which
# is never a line's own indent (Open= only ever follows a command + Cmd=).
_OPEN_DIRECTIVE_RE = re.compile(r'\s+Open=\S+')


def _discover_block_openers() -> dict:
    """{command_word: parse_mode} for every block-opening command, discovered
    from the corpus's own `Cmd=<WORD> Open=<mode>` declarations -- the same
    single-source, no-hand-list approach as _discover_key_vocabulary. `Open=`
    marks a command as opening an indented body and names the parse-mode; the
    Python parser dispatches on it (see _parse_indent_blocks, which looks up
    the `_open_<mode>` handler by that name), and the self-hosted parser
    (parser.re) will read the same key -- so what opens a block has one answer
    for both sides, written where the command itself is declared.

    Scans raw text directly (not via _read_re_file) so it never re-enters
    _parse_indent_blocks, which itself consults this map -- the reason it reads
    files itself rather than reusing the record view. Cached: the corpus does
    not change mid-run.
    """
    global _block_openers_cache
    if _block_openers_cache is not None:
        return _block_openers_cache
    openers: dict = {}
    for path in re_sources():
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                raw = f.readlines()
        except OSError:
            continue
        for _lineno, text in _strip_comments(raw):
            word = mode = None
            for a in _tokenize_quoted(text):
                if a.startswith('Cmd='):
                    word = a.split('=', 1)[1]
                elif a.startswith('Open='):
                    mode = a.split('=', 1)[1]
            if word and mode:
                openers[word] = mode
    _block_openers_cache = openers
    return openers


def _special_openers() -> frozenset:
    """The set of block-opening command words -- literally the keys of the
    _discover_block_openers() map, so "what is an opener" and "what the parser
    opens" are one answer, not a second hand-kept copy."""
    return frozenset(_discover_block_openers())


_DJB2_MASK64 = (1 << 64) - 1


def _reca_djb2(s: str) -> int:
    """The project's djb2, byte-for-byte identical to what the interpreter
    computes at runtime (HASH_STEP = Mul(signed)*33 then Add, both masked to
    64 bits; seed 5381). Verified equal to the live interpreter hash for real
    keys. Used to pre-compute the KEY_HTAB entries statically (see
    AUTOGEN_KEY_TABLE) so the table Reca's own HT_LOOKUP reads at runtime is
    filled at build time -- the same precompute BS_HTAB's bootstrap does.
    Returns the full 64-bit accumulator; callers mask to low 32 (the stored
    hash) with & 0xFFFFFFFF, exactly as BS_LOOKUP_AT does via MASK_LOW32."""
    h = 5381
    for ch in s:
        sh = h if h < (1 << 63) else h - (1 << 64)   # _s64: Mul's signed operand
        h = ((sh * 33) & _DJB2_MASK64)               # Mul, masked to 64 bits
        h = (h + ord(ch)) & _DJB2_MASK64             # Add, masked to 64 bits
    return h


def _discover_known_commands(w1_src: str, w2_src: str) -> frozenset:
    """The real, authoritative command set: whatever Wave 1 + Wave 2 branch
    on, extracted mechanically from their own source. Not a hand-written
    list — one was tried and got it wrong (included STORE/BR, which are
    LLVM-IR template text living inside NEXO's own body, not commands; a
    line starting with either is prose to the real dispatcher, which falls
    through every branch and does nothing, same as any other unrecognised
    text).
    """
    import re
    cmds = set()
    for src in (w1_src, w2_src):
        cmds |= set(re.findall(r'cmd == "(\w+)"', src))
        for grp in re.findall(r'cmd in \(([^)]*)\)', src):
            cmds |= {x.strip().strip('"') for x in grp.split(',')}
    return frozenset(cmds)


_known_commands_cache: frozenset | None = None


def _known_commands(loader_for_scan) -> frozenset:
    """Union of what Wave 2 branches on and what macros.re's own calling
    convention names (_discover_nova_naming_commands) — the two do not
    overlap fully. Wave 2 handles primitives (NEW, SET, NOVA, LINK, ...);
    discovery finds macros (EMIT, RVOCA, RCALL_AT, ...) that Wave 2 never
    mentions by name because they run through the generic Reca-macro path.
    Using only one half of this union dropped EMIT/EMITI/RCALL_AT as junk.
    """
    global _known_commands_cache
    if _known_commands_cache is None:
        import inspect
        w2 = inspect.getsource(Loader._wave2_alloc_line)
        w3 = inspect.getsource(Loader._wave2_wire_line)
        _known_commands_cache = (_discover_known_commands(w2, w3)
                                  | frozenset(loader_for_scan._discover_nova_naming_commands())
                                  | frozenset(_COMMANDS))
    return _known_commands_cache


_alloc_line_cmds_cache: frozenset | None = None


def _alloc_line_own_cmds() -> frozenset:
    """Commands _wave2_alloc_line has its own dedicated handling for,
    discovered by scanning that function's own source (same technique as
    _discover_known_commands, applied to just this one function) instead
    of a hand-maintained exclusion list.

    Replaces the old `cmd not in _BOOTSTRAP_CMDS and cmd != "NOVAE"` gate
    in front of the generic macro-argument pre-allocation block below:
    that gate was two things bolted together (a hand-maintained frozenset
    covering some commands, plus one hardcoded name check for NOVAE alone)
    standing in for a single real question -- "does this function have its
    own code for this cmd, further down?" -- that the function's own
    source already answers mechanically. NOVAE needed its own carve-out
    only because it wasn't in the frozenset; discovering the set from
    source makes that carve-out unnecessary rather than one exception
    among others. `_BOOTSTRAP_CMDS` itself is gone now -- every place that
    used it (this gate, the modifier-tuple check in _wave1_lcount_line, and
    _freeze_prepare's pre-registration loop) is mechanically discovered
    instead; see this function and `_bootstrap_only_cmds` below.
    """
    global _alloc_line_cmds_cache
    if _alloc_line_cmds_cache is None:
        import inspect
        # "Own" = has allocation of its own, so the generic macro-arg
        # pre-alloc must skip it: an inline branch discovered from source, OR a
        # handler that OVERRIDES declare (SWITCH/PACK/MATCH mint their own
        # case/tag/node names; the expanders recurse into sub-records). A
        # handler that does NOT override declare (ALLOC_TO/CLEAR/JEQ/...) is
        # deliberately absent -- it wants the universal generic alloc, which
        # runs precisely because it is not "own". Detected by the presence of
        # an override, not a flag or a per-command list.
        _own_handlers = frozenset(
            c for c, h in _COMMANDS.items() if h.declare is not _Cmd.declare)
        _alloc_line_cmds_cache = (_discover_known_commands(
            inspect.getsource(Loader._wave2_alloc_line), "")
            | _own_handlers)
    return _alloc_line_cmds_cache


def _tokenize_quoted(text: str) -> list:
    """Split on whitespace, but a double-quoted span is one token even if
    it contains spaces -- `line.split()` breaks `SET X "a b c"` into
    ['SET', 'X', '"a', 'b', 'c"'] instead of ['SET', 'X', '"a b c"'].
    This is what every re-parse of `cmd_part` (see _wire_word_value,
    _parse_new_modifiers) exists to work around; doing it once, here, in
    the tokenizer itself, means records never carry that bug forward.
    """
    toks = []
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        # One token is a maximal run of non-whitespace, EXCEPT that
        # whitespace inside a "..." span belongs to the token. The quote
        # need not be the token's first byte: `Value="a b c"` is one token
        # just as `"a b c"` is. The old version only recognised a quote in
        # the FIRST position, so `Value="a b"` split at the inner space --
        # every keyed string value with a space (SF_LIMITER_BARE=
        # "Bare Lux: " and ~15 others) was silently truncated to its first
        # word. Toggling on every quote keeps the whole span together
        # regardless of where the quote sits.
        j = i
        in_quote = False
        while j < n and (in_quote or not text[j].isspace()):
            if text[j] == '"':
                in_quote = not in_quote
            j += 1
        toks.append(text[i:j])
        i = j
    return toks


def _parse_record(text: str, known: frozenset) -> tuple | None:
    """One clean line -> (cmd, args, slots), or None if it is not a command.

    This is Wave 0's third cut. The first two removed `//` blocks and `/`
    lines; this one removes what is left that cannot be executed — prose
    like `── Standard file descriptors ──` and `====...====`, which today
    survives all the way to the dispatcher and is silently ignored there.
    Measured: 477 such lines, against 6341 real ones.

    `slots` holds only the lux's own slot names (El1/El2/Exit/Next/At).
    Anything else with an `=` in it is an ordinary argument — quoted
    strings included, which is why the `"` check comes first.

    `Name=X` is the keyword form of a naming command's own label. It is
    normalised back into args[0] here, so every wave handler downstream
    keeps reading the name from the same place regardless of which form
    the source used -- which is what makes accepting the new form a
    parse-level change only, touching no handler.
    """
    return _record_from_tokens(_tokenize_quoted(text), known)


def _record_from_tokens(p, known: frozenset) -> tuple | None:
    """The token half of _parse_record: an already-tokenized line (list of
    token strings) -> (cmd, args, slots), or None if it is not a command.
    Split out so the block-body handlers, which now carry their bodies as
    token lists (not text), build records without a text round-trip -- the
    same model Reca uses (its record[2] holds tokenized lines). _parse_record
    is now just _tokenize_quoted + this."""
    if not p:
        return None
    cmd = p[0].upper()
    if cmd not in known:
        return None
    args, slots = _split_args_slots(p[1:])
    return (cmd, args, slots)

def _split_args_slots(tokens) -> tuple:
    """Split a command's tokens (everything after the command word) into
    positional `args` and lux-slot `slots`.

    The one implementation of this rule. A token shaped `Key=Value` whose
    Key is a real lux slot goes to `slots`; everything else stays a
    positional argument -- quoted strings included, which is why the `"`
    check comes first (`SET="a b"` is a value, not a slot).

    `Name=X` is the keyword form of a naming command's own label; it is
    normalised back into args[0] so callers read the name from one place
    regardless of which form the source used.

    Shared by _parse_record (the wave path) and _discover_nova_naming_commands
    (the macros.re scan). Those two carried separate copies of this loop and
    had to be kept in step by hand -- exactly the kind of duplicate that
    drifts, and the reason the scan is not simply _parse_record: the scan
    must accept commands that are not yet known, since finding them is its
    whole job, so only the split is common, not the known-command gate.
    """
    known_keys = _discover_key_vocabulary()
    args, slots = [], {}
    for a in tokens:
        if '=' in a and not a.startswith('"'):
            k, v = a.split('=', 1)
            if k in known_keys:
                slots[k] = v
                continue
        args.append(a)
    name = slots.get('Name')
    if name is not None:
        args.insert(0, name)
    return args, slots


def _expand_autogen_markers(lines: list) -> list:
    """Expand the AUTOGEN_KEY_* marker lines into the data-driven key
    recognizer they stand for, all generated from _discover_key_vocabulary()
    -- the corpus's own real vocabulary, found by scanning, not a hand-typed
    list. See LOAD_ELEM's own docstring (lexer.re) for the runtime side.

    Key recognition is a hash table (AUTOGEN_KEY_TABLE), not a byte-tree:
    the argument reader hashes a token's key part and one HT_LOOKUP yields
    its code, then one generic store routes through KEY_DEST[code]. This
    replaced the old MATCH byte-tree plus per-key KEY_CODE/JEQ/store codegen
    (the AUTOGEN_MATCH_CANDIDATES / AUTOGEN_KEY_HANDLERS / AUTOGEN_KEY_JEQS /
    AUTOGEN_KEY_STORES markers), which are gone -- one table where there were
    N hand-shaped handler chains.

    There is no AUTOGEN_COMMAND_CANDIDATES: command dispatch
    (LOAD_DISPATCH_CORE/BS_LOOKUP) runs once per source line during a
    one-time load pass, and BS_HTAB's command entries are already populated
    for free as a side effect of each WAX_CMD line's own ordinary processing
    (BS_HT_REGISTER, intern.re) -- a hash table already held live.

    AUTOGEN_KEY_TABLE <prefix> -- the pre-computed static hash table plus
    its <prefix>_DEST[code] -> &SK_ARG_<key> routing (details at the branch).
    AUTOGEN_KEY_REGS -- one 2-wide SK_ARG_<key> value+kind slot per key.
    AUTOGEN_KEY_CLEARS <prefix> -- one CLEAR per SK_ARG_<key>, per read.

    A marker line is replaced in place by N generated lines; every other
    line passes through unchanged.
    """
    _AUTOGEN_HEADS = ('AUTOGEN_KEY_REGS', 'AUTOGEN_KEY_CLEARS',
                      'AUTOGEN_KEY_TABLE')
    changed = False
    for lineno, text in lines:
        parts = text.split()
        if parts and parts[0] in _AUTOGEN_HEADS:
            changed = True
            break
    if not changed:
        return lines

    # Destination register for each key, so LOAD_READARG_KV's routing is
    # generated from the discovered vocabulary instead of a hand-kept list
    # (which silently dropped Body/Value/Count/In/Rel/Width -- see
    # _discover_key_vocabulary). Every KEYED argument lands in its own
    # SK_ELEM_<key> -- the by-key model, mirroring Python's slots[key]. The
    # only key that does not is `Name`, which folds into RA_MA0, the one
    # normalised identity slot the "first keyless token == Name" rule also
    # fills. The already-existing At/Next/Reset/Op1..3 registers are named
    # explicitly (their consumers read those exact upper-case names); every
    # other key -- El1/El2/Exit included -- routes to SK_ELEM_<key> verbatim,
    # minted with no edit here the moment it appears in the corpus. (El1/El2/
    # Exit used to bridge into RA_MA2/3/4 for a positional builder to read;
    # that bridge was dead -- the only NOVA-operand consumer, EXP_CMD_NOVA,
    # reads its own tokens instead of the pre-read, and is itself being
    # aligned onto this reader. Positional args still use RA_MA1.. via the
    # SK_POS_SLOT path, untouched.)
    # Every keyed argument lands in one uniform namespace, SK_ARG_<key>
    # (mirroring Python's slots[key]); the only exception is Name, which
    # folds into RA_MA0 -- the single identity slot the "first keyless token
    # == Name" rule also fills. SK_ARG_ is a fresh namespace, so a key like
    # Exit/Value never collides with the reader's own SK_ELEM_EXIT/
    # SK_ELEM_VALUE scratch temps. (The old At/Next/Reset/Op1..3 keys used to
    # route to hand-named SK_ELEM_ registers, but those turned out to be
    # write-only -- nothing read them, since their only consumer, the NOVA
    # handler, reads its own tokens -- so folding them into SK_ARG_ cost no
    # consumer migration.)
    _key_dest_map = {'Name': 'RA_MA0'}
    def _kdest(k):
        return _key_dest_map.get(k, f'SK_ARG_{k}')
    out = []
    for lineno, text in lines:
        parts = text.split()
        head = parts[0] if parts else ''
        if head == 'AUTOGEN_KEY_REGS':
            # One scratch register per key whose destination is an SK_ELEM_
            # (i.e. every keyed arg except Name, which uses RA_MA0). MA-routed
            # keys need no register -- RA_MA* already exist.
            for k in sorted(_discover_key_vocabulary()):
                d = _kdest(k)
                if not d.startswith('RA_MA'):
                    # 2-wide slot: value at [0], its kind (1=int literal,
                    # 0=name/string) adjacent at [1]. The generic store writes
                    # both (Write [dest], Write [dest+1]); the one kind consumer
                    # (SET's Value) reads it at SK_ARG_Value+1. KEY_DEST[code]
                    # points at the value cell (see AUTOGEN_KEY_TABLE).
                    out.append((lineno, f'NEW {d} Count=2'))
        elif head == 'AUTOGEN_KEY_CLEARS':
            # Clear every SK_ELEM_ key register at the start of each read, the
            # way the hand-written CLEAR block did for At/Next/Reset/Op1..3.
            prefix = parts[1]
            for k in sorted(_discover_key_vocabulary()):
                d = _kdest(k)
                if not d.startswith('RA_MA'):
                    out.append((lineno, f'CLEAR {prefix}_{k} El1={d}'))
        elif head == 'AUTOGEN_KEY_TABLE':
            # Data-driven key recognizer: a real hash table (same generic
            # HT_LOOKUP the command/symbol tables use) whose entries are
            # PRE-COMPUTED here and written as STATIC data -- the same
            # precompute BS_HTAB's own bootstrap does, made possible because
            # _reca_djb2 is byte-identical to the interpreter's runtime hash
            # (verified) and SET At= writes a chosen cell at build time.
            #
            # Replaces the MATCH byte-tree (recognition) and the per-key
            # KEY_CODE/JEQ/store codegen (dispatch) with: HT_LOOKUP -> code,
            # then one generic store via <prefix>_DEST[code]. Full-name
            # compare is kept (HT_NAME_CMP, by construction) -- not hash-only.
            #
            #   <prefix>_HTAB      the table (HT_LOOKUP format: word0 =
            #                      hash32<<32 | code, word1 = &name entry;
            #                      slot addr = base + slot*2, per htable.re)
            #   <prefix>_NAME_<k>  the key's own name bytes: [len, b0, b1, ...]
            #                      (HT_NAME_STORE/CMP format), its own symbol so
            #                      word1 is a plain address, no symbol+offset.
            #   <prefix>_DEST[code] &SK_ARG_<key> -- code -> destination slot.
            prefix = parts[1]
            keys = sorted(_discover_key_vocabulary())
            n = len(keys)
            size = 1
            while size < n * 4:      # load factor <= 0.25 -> few/no probes
                size <<= 1
            mask = size - 1
            out.append((lineno, f'NEW {prefix}_HTAB Count={size} Width=2'))
            out.append((lineno, f'NEW {prefix}_DEST Count={n}'))
            out.append((lineno, f'NEW {prefix}_HT_BASE'))
            out.append((lineno, f'NEW {prefix}_HT_SIZE'))
            out.append((lineno, f'NEW {prefix}_HT_MASK'))
            # base = the HTAB pointer lux itself (addr(HTAB)); HT_LOOKUP then
            # reads bucket i at base + i*2, so bucket 0's word0 falls on the
            # pointer cell. Slot 0 is therefore RESERVED below (never written),
            # leaving the pointer's own self-ref word (a[HTAB] = region addr,
            # nonzero) in place as bucket 0's word0: a live entry there would
            # break the pointer's self-ref (lint) and, more importantly, a key
            # can never hash-home to a cell an entry would corrupt. Because that
            # word is nonzero, HT_LOOKUP treats slot 0 as an occupied-mismatch
            # and probes past it -- exactly matching the reserve here.
            out.append((lineno, f'SET {prefix}_HT_BASE Value={prefix}_HTAB'))
            out.append((lineno, f'SET {prefix}_HT_SIZE Value={size}'))
            out.append((lineno, f'SET {prefix}_HT_MASK Value={mask}'))
            used: dict = {0: None}   # reserve slot 0 (the pointer cell) -- see above
            for code, k in enumerate(keys, start=1):
                # name entry: [len, bytes...] in its own block
                out.append((lineno, f'NEW {prefix}_NAME_{k} Count={len(k)}'))
                out.append((lineno, f'SET {prefix}_NAME_{k} At=0 Value={len(k)}'))
                for i, ch in enumerate(k):
                    out.append((lineno, f'SET {prefix}_NAME_{k} At={i + 1} Value={ord(ch)}'))
                # linear-probe placement (same probe order HT_LOOKUP walks)
                h32 = _reca_djb2(k) & 0xFFFFFFFF
                slot = h32 & mask
                while slot in used:
                    slot = (slot + 1) & mask
                used[slot] = k
                word0 = (h32 << 32) | code
                # Bucket i: word0 at base + i*2, word1 (&name) at +1, base =
                # addr(HTAB), so the At= offsets (relative to HTAB) are exactly
                # 2*i and 1+2*i. NO +2 header: HT_LOOKUP (htable.re) reads
                # base + slot*2 flat, and the former base+2 skip was removed
                # there (the "eliminate HT +2-header convention" cleanup). This
                # static writer kept the stale +2 (At=2+2*slot / 3+2*slot), so
                # every entry sat one slot high -- a key was recognised only
                # when its home slot happened to be occupied by a neighbour,
                # forcing a probe (e.g. El2 found via El1's misplaced entry),
                # and missed otherwise. That silently dropped Op/El1/Exit on
                # every self-hosted NOVA line -> the whole named graph came out
                # husk. (slot 0 is reserved above, so 2*slot never hits the
                # HTAB pointer cell.)
                out.append((lineno, f'SET {prefix}_HTAB At={2 * slot} Value={word0}'))
                out.append((lineno, f'SET {prefix}_HTAB At={1 + 2 * slot} Value={prefix}_NAME_{k}'))
                out.append((lineno, f'SET {prefix}_DEST At={code} Value=SK_ARG_{k}'))
        else:
            out.append((lineno, text))
    return out


def _read_and_parse(filepath: str, loader_for_scan) -> tuple:
    """Read one file once, returning both its text lines and its records.

    Building records from _build_records alone would call _read_re_file a
    second time, which calls _parse_indent_blocks a second time -- the exact
    duplicate-expansion pattern this whole effort removes elsewhere. This
    reads once and derives both views from that one read.

    Returns (lines, records) where lines is _read_re_file's own
    [(lineno, text), ...] (kept for Wave 2/3, which still read text — see
    PLAN_TIDE.md step 2) and records is [(lineno, record_or_None), ...]
    from the same lines, via _parse_record.
    """
    known = _known_commands(loader_for_scan)
    lines = _read_re_file(filepath, loader_for_scan)
    # An item is normally text to parse; a block-opener that direct-builds
    # (SWITCH -- see _parse_indent_blocks's _close_ctx) instead hands over a ready
    # (cmd, args, slots) tuple, which is already a record and passes through.
    records = [(lineno, item if isinstance(item, tuple) else _parse_record(item, known))
               for lineno, item in lines]
    return lines, records


def _strip_comments(raw_lines: list) -> list:
    """Comment stripping — one step inside Wave 0, not the whole of it.

    Wave 0 (see _wave0_parse) is comment-strip + indent-expand + static
    dispatch + save, in one pass, run once per file. This function is only
    the first of those four. It used to be named `_wave0_line`, which
    claimed to be the whole wave and was not — it doesn't expand indent
    syntax, doesn't dispatch, doesn't save anything; _read_re_file calls
    this and then _parse_indent_blocks separately, and _wave0_parse (via
    _read_and_parse) is the actual single place all four happen together.

    Returns [(lineno, text), ...] -- one entry per source line, always, so
    line numbers survive stripping.

    `text` keeps the line's own leading indentation and drops everything
    else: trailing comment, trailing blanks, and the whole line if it is a
    comment. Indentation stays in the text because indentation is now the
    only thing that ends a block, and the self-hosted stripper (comments.re)
    has nowhere else to put it -- one field, one shape, both sides.

    Nothing else is returned. Two fields used to be: the untouched source
    line, and an "is this line indented" flag. Both belonged to the old rule
    where a blank line closed a block: the raw line was what told a comment
    apart from a genuinely empty one (they close differently under that
    rule), and the flag was a leftover that no caller ever read. Under
    indent-closing neither has anything to decide.
    A dropped line yields '' rather than vanishing: line numbers and
    blank-line boundaries have to survive, because CHAIN/SWITCH/FOR/NOVAE
    bodies are delimited by indentation and reset by blank lines.

    The rule (see COMMENTS.md):
    - A line that is exactly "//" (whitespace aside) toggles block state.
      That is the ONLY thing that is a marker.
    - Inside a block, every line is dropped.
    - Outside, a line whose first non-whitespace char is '/' is dropped.
      This also covers a "//text" line: it starts with a slash, so it goes,
      and no marker is recognised inside it.
    - A trailing /comment on a code line is cut.
    - Everything else passes through.

    A pair appearing in prose, in a quoted string, in a path, or mid-code is
    just text. There is nothing to disambiguate, which is why there is no
    slash-run check, no discard-rest-of-line, and no cross-line string
    tracking here. The old inline rule needed all three, and each was a real
    bug.

    This is a duplicate of comments.re's STRIP_COMMENTS, which is the
    canonical implementation. It exists only because bootstrap cannot run
    Reca before Reca is loaded. Do not let the two drift -- any change here
    must be made there and re-verified over every .re file.
    """
    result = []
    in_block = False
    for lineno, raw in enumerate(raw_lines, 1):
        line = raw.rstrip('\n')
        s = line.strip()

        if s == '//':
            in_block = not in_block
            result.append((lineno, ''))
            continue
        if in_block or s.startswith('/'):
            result.append((lineno, ''))
            continue
        cut = _cut_trailing_comment(s)
        if not cut:
            result.append((lineno, ''))
            continue
        indent = line[:len(line) - len(line.lstrip())]
        result.append((lineno, indent + cut))
    return result



_REG_PREFIXES = ('RA_', 'SC_', 'SAL_', 'S_')


def _file_tag(filepath: str) -> str:
    """Short, stable, generated-name-safe id for a file: basename without
    extension, non-alnum/underscore collapsed to '_'. One definition, shared
    by _read_re_file and the MATCH handler so both derive the same tag."""
    base = os.path.splitext(os.path.basename(filepath))[0]
    return ''.join(c if (c.isalnum() or c == '_') else '_' for c in base)


def _ascii_const_for(ch: str) -> str:
    """Map one character to its ascii.re constant name. Lowercase letters use
    the project's 'l'-suffix convention (ASCII_el, not ASCII_e) uniformly,
    including a/z. Digits and uppercase already have bare names."""
    if ch.isalpha() and ch.islower():
        return f'ASCII_{ch}l'
    if ch.isdigit():
        return f'ASCII_{ch}'
    return f'ASCII_{ch.upper()}'


def _match_flat_names(items) -> list:
    """Every leaf name a MATCH group resolves (a nested group contributes its
    members). Shared by the parent-registration at block close and by
    _match_expand's own flatten, so the two cannot drift."""
    flat = []
    for it in items:
        if it['kind'] == 'leaf':
            flat.append(it['name'])
        else:
            flat.extend(it['members'])
    return flat


def _match_expand(items, group_name, except_label, file_tag, ref_lineno):
    """Build one MATCH group's dispatcher: minimal Read+JEQ comparisons over
    bytes at RA_MATCH_BASE+depth. Returns (records, flat_names) -- records is
    the (lineno, (cmd,args,slots)) node-specs implementing group_name's own
    dispatcher (entry point named group_name, RVOCA-able on its own);
    flat_names is every leaf name this group resolves.

    Module-level (not nested in _parse_indent_blocks) so the Wave-2 MATCH handler and
    the Wave-1 sizing pass can both compute the same dispatcher from the group's
    items at load time -- MATCH builds through the handler like every other
    command, nothing pre-computed at expand time.

    Fallthrough safety: every sibling JEQ at one depth is emitted as one
    unbroken chain BEFORE any subtree, so an unmatched JEQ falls through to the
    next sibling, never into a subtree; subtrees follow, each reached only by an
    explicit true-jump and each preceded by NOLINK so the auto-chain never wires
    one subtree's tail into the next's head by adjacency. The chain's own
    natural continuation into the Except jump is wanted and left un-severed.

    Group hand-off: once every remaining candidate resolves to the same action
    (one leaf, or several names all in one nested group) comparison stops and
    control jumps straight there WITHOUT advancing RA_MATCH_BASE, so a group's
    code is identical whether reached by nesting or RVOCA'd directly."""
    counter = [0]

    def gensym(suffix):
        counter[0] += 1
        return f'__match_{file_tag}_{ref_lineno}_{group_name}_{counter[0]}_{suffix}'

    # Flatten to (name, action) pairs. action = ('leaf', label) or
    # ('group', entry_label).
    flat_names = _match_flat_names(items)
    entries = []
    for it in items:
        if it['kind'] == 'leaf':
            entries.append((it['name'], ('leaf', it['label'])))
        else:
            for m in it['members']:
                entries.append((m, ('group', it['entry'])))

    def build(node_entries, depth, entry_label):
        if not node_entries:
            return []

        # Once every remaining candidate resolves to the SAME action, stop
        # comparing and jump straight there (see the hand-off note above).
        actions = {action for _name, action in node_entries}
        if len(actions) == 1:
            kind, target = next(iter(actions))
            return [(ref_lineno, ('NOVA', [entry_label],
                                  {'Op': 'Jump', 'Exit': target}))]

        # A name exactly `depth` bytes long is fully consumed; RA_LOAD_TLEN
        # (the real token length) tells it apart from longer siblings that
        # share every byte so far -- a byte read at `depth` cannot, since the
        # buffer past a short token's end holds stale bytes.
        exhausted = [(name, action) for name, action in node_entries if len(name) == depth]
        longer = [(name, action) for name, action in node_entries if len(name) > depth]
        if len(exhausted) > 1:
            raise LoadError(
                f"{file_tag}:{ref_lineno}: MATCH group '{group_name}': "
                f"duplicate key(s) at depth {depth}: "
                f"{[n for n, _ in exhausted]!r}.")
        if exhausted:
            exh_name, (exh_kind, exh_target) = exhausted[0]
            out = [(ref_lineno, ('JEQ', [entry_label],
                    {'El1': 'RA_LOAD_TLEN', 'El2': f'C_{depth}', 'Exit': exh_target}))]
            if not longer:
                if except_label:
                    out.append((ref_lineno, ('NOVA', [gensym("emiss")],
                                {'Op': 'Jump', 'Exit': except_label})))
                return out
            out.extend(build(longer, depth, gensym('cont')))
            return out
        groups: dict = {}
        order = []
        for name, action in node_entries:
            ch = name[depth]
            if ch not in groups:
                groups[ch] = []
                order.append(ch)
            groups[ch].append((name, action))

        if len(groups) == 1:
            # Every remaining candidate shares this byte -- it distinguishes
            # nothing, so no comparison is emitted; advance to the next byte.
            return build(node_entries, depth + 1, entry_label)

        out = []
        addr_label = entry_label
        out.append((ref_lineno, ('NOVA', [addr_label],
                    {'Op': 'Add', 'El1': 'RA_MATCH_BASE', 'El2': f'C_{depth}', 'Exit': 'RA_MATCH_ADDR'})))
        out.append((ref_lineno, ('NOVA', [gensym("rd")],
                    {'Op': 'Read', 'El1': 'RA_MATCH_ADDR', 'Exit': 'RA_MATCH_BYTE'})))
        child_labels = []
        for i, ch in enumerate(order):
            child_labels.append(gensym(f'b{i}'))
        for i, ch in enumerate(order):
            out.append((ref_lineno, ('JEQ', [gensym(f"j{i}")],
                        {'El1': 'RA_MATCH_BYTE', 'El2': _ascii_const_for(ch), 'Exit': child_labels[i]})))
        # No NOLINK here: the chain's natural continuation is the wanted edge
        # to Except (or nowhere). Subtrees below each get their own NOLINK.
        if except_label:
            out.append((ref_lineno, ('NOVA', [gensym("miss")],
                        {'Op': 'Jump', 'Exit': except_label})))
        for i, ch in enumerate(order):
            out.append((ref_lineno, ('NOLINK', [], {})))
            out.extend(build(groups[ch], depth + 1, child_labels[i]))
        return out

    lines = build(entries, 0, group_name)
    return lines, flat_names


def _for_parse_header(tokens):
    """Parse a FOR header's token list into a flat slot list:
    [(position, name_or_None), ...], 0-indexed. A purely-numeric token is a
    count block (N consecutive anonymous slots); any other token is one named
    slot. Every slot consumes one position in a single running count."""
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
    specificity: 0=name, 1=position/range. Raises ValueError if the token
    resolves to nothing -- loud failure, not a silent no-op."""
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


def _for_parse_override_line(toks, slots, order_idx):
    """Parse one override-block line (a TOKEN list) into a list of
    (position, specificity, kv_dict, order_idx) entries -- one per slot the
    line's selector(s) resolve to. A line with no selector before '>' is a
    default: applies to every slot at the lowest specificity. '>' is its own
    token (spaced in the corpus); selectors are the tokens before it, the
    key=value pairs the tokens after."""
    if '>' not in toks:
        raise ValueError(f"FOR override line missing '>': {toks!r}")
    gt = toks.index('>')
    sel_toks = toks[:gt]
    kv = {}
    for pair in toks[gt + 1:]:
        if '=' not in pair:
            raise ValueError(f"FOR override: bad key=value token {pair!r}")
        k, v = pair.split('=', 1)
        kv[k] = v
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


def _for_expand_records(slots, body_lines, override_lines, known):
    """Expand a FOR block into ready records: for each slot, substitute
    {N}/{X}/override-keys into each body line and parse the result. The one
    override rule -- most specific selector wins (name > position/range >
    default), tie-broken by last-in-text -- unchanged. A placeholder with no
    resolved value is left literal, so a forgotten value surfaces as a
    traceable token, not a silent empty.

    Module-level and record-returning (not text): the Wave-1 sizing pass and
    the Wave-2 build handler both call it at load time, when the key
    vocabulary is already warm -- so parsing here never re-enters the
    vocabulary scan (which is why FOR no longer needs a scan-time flag). A
    line that parses to nothing (prose/unknown) yields None, skipped by both
    handlers exactly as an inert line always was."""
    all_entries = []
    for idx, (_lineno, ov_toks) in enumerate(override_lines):
        all_entries.extend(_for_parse_override_line(ov_toks, slots, idx))

    out = []
    for pos, name in slots:
        subst = {'N': str(pos)}
        if name is not None:
            subst['X'] = name
        keys_here = {k for p, _s, kv, _o in all_entries if p == pos for k in kv}
        for key in keys_here:
            candidates = [(spec, oidx, kv[key]) for p, spec, kv, oidx in all_entries
                          if p == pos and key in kv]
            candidates.sort(key=lambda c: (c[0], -c[1]))
            subst[key] = candidates[0][2]
        for lineno, body_toks in body_lines:
            new_toks = []
            for tok in body_toks:
                for key, val in subst.items():
                    tok = tok.replace('{' + key + '}', val)
                new_toks.append(tok)
            out.append((lineno, _record_from_tokens(new_toks, known)))
    return out


def _def_expand_records(name, body, file_tag, known):
    """Expand a DEF block into ready records: walk its pre-structured body
    (token-line leaves + already-built nested-block records -- nesting resolved
    at collection, no re-parse here), give the block's NAME to the entry,
    resolve {RET} to NAME_RET in each leaf's tokens, and append the implicit
    `RREDI NAME_RET`.

    Module-level and record-returning, like _for_expand_records: the Wave-1
    sizer and the Wave-2 build handler both call it at load time. A nested block
    is passed through as-is -- the wave re-expands it via its own per-command
    handler, and a nested DEF's own {RET} binds to ITS name there, so {RET}
    stays scoped without a text pre-pass.

    Entry naming: the first leaf record takes NAME as its own label when it is
    anonymous (no positional name). A leaf that already names itself, and a
    nested block (no single lux to carry a name), are left alone -- the same two
    shapes the old inline injection skipped.

    A body item is (lineno, X): X a token list (leaf) or a (cmd, args, slots)
    tuple (nested block)."""
    ret = f"{name}_RET"
    out = []
    entry_done = False
    last_ln = body[-1][0] if body else 0
    for item_ln, x in body:
        last_ln = item_ln
        if isinstance(x, tuple):
            rec = x  # nested block record; the wave re-expands it
        else:
            rec = _record_from_tokens([t.replace('{RET}', ret) for t in x], known)
        if not entry_done:
            entry_done = True
            if rec is not None and not isinstance(x, tuple):
                _cmd, _args, _slots = rec
                if not _args:
                    rec = (_cmd, [name], _slots)
        out.append((item_ln, rec))
    out.append((last_ln, ('RREDI', [ret], {})))
    return out


def _save_expand_records(regs, body, entry, norestore, file_tag, ln, known):
    """A SAVE block's own expansion rule: assemble the register-save setup, the
    re-parsed body, and (unless norestore) the register-restore -- all as
    ordinary NEW/NOVA sub-records the wave handlers already dispatch. SAVE is
    now a plain block like FOR/DEF (collect body, emit one record, expand in
    the handler); this replaces the old special-casing where setup was emitted
    inline by _open_save, the body flowed inline, and the restore was deferred
    through _pending_restore as a separate SAVE_RESTORE record type. The setup
    and restore NOVAs are exactly what every wave used to rebuild for itself
    (identically, three times) -- folded here into one place.

    Module-level and record-returning, like _for_expand_records /
    _def_expand_records. Entry naming and the __sv_ gensym are byte-for-byte
    what the old per-wave SAVE/SAVE_RESTORE handlers produced (n = tag_line,
    entry on the first Move, anonymous __sv_ names otherwise)."""
    n = f"{file_tag}_{ln}"
    out = [(ln, ('NEW', [f'S_{reg}'], {})) for reg in regs]
    for i, reg in enumerate(regs):
        nm = entry if (i == 0 and entry) else f'__sv_{n}_s{i}'
        out.append((ln, ('NOVA', [nm], {'Op': 'Move', 'El1': reg, 'Exit': f'S_{reg}'})))
    for item_ln, x in body:
        # x is a token list (leaf) or an already-built (cmd,args,slots) tuple
        # (nested block, resolved at collection -- the wave re-expands it). SAVE
        # bodies carry no placeholders, so leaves parse straight through.
        rec = x if isinstance(x, tuple) else _record_from_tokens(x, known)
        out.append((item_ln, rec))
    if not norestore:
        for i, reg in enumerate(regs):
            out.append((ln, ('NOVA', [f'__sv_{n}_r{i}'],
                             {'Op': 'Move', 'El1': f'S_{reg}', 'Exit': reg})))
    return out


# ── Per-command build handlers (Wave 1/2/3 dispatch) ──────────────────────
# One handler per command word, registered in _COMMANDS. The three wave loops
# -- _wave1_lcount_line (size), _wave2_alloc_line (declare), _wave2_wire_line
# (wire) -- dispatch here uniformly by command name: the whole of a command's
# build logic in ONE place, reached by ONE lookup, with no per-wave if-chain
# and no per-wave discovery convention. Mirrors Reca, where a command is one
# named handler the waves reach through the symbol table (BS_LOOKUP), the same
# handler run each wave. A handler overrides only the waves it takes part in;
# _CmdDefault (registered last) covers the generic case.
_COMMANDS = {}

def _command(*words):
    """Register a handler class under one or more command words."""
    def _reg(cls):
        for w in words:
            _COMMANDS[w] = cls
        return cls
    return _reg

class _Cmd:
    """Base command handler. A subclass overrides only the waves it joins.
    Every method takes the live loader, the record, and that wave's own
    context: size returns Wave-1's five sizing facts (link_pairs, nova_add,
    region_updates, span_update, extra_pairs); declare and wire mutate the
    loader in place and return nothing.

    A handler only ever contributes what is SPECIFIC to its command -- the
    folded facts of a body it expands (FOR/DEF/SAVE/...), or the nova_add of a
    construct it builds directly (SWITCH/MATCH/PACK/ALLOC_TO). It never carries
    the cross-cutting logic (In=/region/span/generic macro-arg alloc): the
    waves run that universally, for every command, and it simply finds nothing
    to do when a command has none of those keys. So no handler opts in to a
    shared facility and none has to call one -- the general logic wraps around
    all commands by default, and a handler's own facts merge with it."""
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return [], (), (), [], ()
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        pass
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        pass


def _fold_sizes(loader, subrecs, naming_commands):
    """Fold the five Wave-1 sizing facts of a list of (lineno, subrecord)
    into one -- the union a block/compound contributes via its expansion.
    The shared tail of every compound's own size(); was copy-pasted per
    branch in _wave1_lcount_line."""
    lp, na, ru, su, ep = [], [], [], [], []
    for _ln, s in subrecs:
        a_lp, a_na, a_ru, a_su, a_ep = loader._wave1_lcount_line(s, naming_commands)
        lp += a_lp; na += list(a_na); ru += list(a_ru); su += a_su; ep += list(a_ep)
    return (lp, tuple(na), ru, su, ep)


def _arg0_name_size(rec):
    """Wave-1 facts for a macro whose args[0] IS its own minted NOVA name --
    the only sizing fact it contributes: pre-register args[0], unless the line
    is templated (a FOR-macro body, resolved later). Exactly what the generic
    naming-command branch produced for these before they became handlers.
    Shared by the simple direct-build macros (CLEAR/NOP/JEQ/JZ/HASH_STEP)."""
    _c, args, slots = rec
    if not args:
        return [], (), (), [], ()
    name = args[0]
    if ('{' in name or any('{' in p for p in args[1:])
            or any('{' in str(v) for v in slots.values())):
        return [], (), (), [], ()
    return [], (name,), (), [], ()


@_command('FOR')
class _CmdFOR(_Cmd):
    # FOR loop: substitute {N}/{X}/overrides into the body tokens and expand to
    # N iterations of records (_for_expand_records), then size/declare/wire each
    # like any ordinary line. A FOR header line with no body (opener passthrough)
    # contributes nothing.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return [], (), (), [], ()
        _bl, _ov = _for_body_ov(slots)
        return _fold_sizes(loader, _for_expand_records(
            slots['__slots__'], _bl, _ov, _known_commands(loader)), naming_commands)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        _bl, _ov = _for_body_ov(slots)
        for _ln, r in _for_expand_records(slots['__slots__'], _bl, _ov, _known_commands(loader)):
            loader._wave2_alloc_line(r, filepath, _ln, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        _bl, _ov = _for_body_ov(slots)
        for _ln, r in _for_expand_records(slots['__slots__'], _bl, _ov, _known_commands(loader)):
            loader._wave2_wire_line(r, filepath, _ln)


@_command('DEF')
class _CmdDEF(_Cmd):
    # DEF subroutine: name the entry, resolve {RET}, append RREDI NAME_RET
    # (_def_expand_records), then size/declare/wire each resulting record.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if '__body__' not in slots:
            return [], (), (), [], ()
        return _fold_sizes(loader, _def_expand_records(
            args[0], slots['__body__'], slots['__tag__'], _known_commands(loader)),
            naming_commands)
    # A DEF body is a scope: its internal names bind locally (default), while
    # the entry (args[0], declared by this header in the enclosing scope) and
    # any Vis=Public name stay global. The scope_id is the entry name -- unique
    # and identical across waves, so declare and wire share one scope with the
    # same addresses. The local set is pre-scanned from the fully-expanded body
    # (_block_local_names), so every reference resolves by knowing its home.
    # size() is deliberately NOT scoped: a local NOVA still needs NOVA-sizing,
    # and that hint rides the global _nova_names the fold feeds.
    @staticmethod
    def _scope(loader, args, recs):
        nc = loader._discover_nova_naming_commands() - _NON_NAMING_PRIMS
        loader._scope_enter(("scope", args[0]),
                            loader._block_local_names(recs, args[0], nc))
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if '__body__' not in slots:
            return
        recs = _def_expand_records(args[0], slots['__body__'],
                                    slots['__tag__'], _known_commands(loader))
        _CmdDEF._scope(loader, args, recs)
        try:
            for _ln, r in recs:
                loader._wave2_alloc_line(r, filepath, _ln, idx)
        finally:
            loader._scope_leave()
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if '__body__' not in slots:
            return
        recs = _def_expand_records(args[0], slots['__body__'],
                                    slots['__tag__'], _known_commands(loader))
        _CmdDEF._scope(loader, args, recs)
        try:
            for _ln, r in recs:
                loader._wave2_wire_line(r, filepath, _ln)
        finally:
            loader._scope_leave()


@_command('SAVE')
class _CmdSAVE(_Cmd):
    # SAVE block: _save_expand_records assembles register-save setup + body +
    # register-restore as ordinary records; size/declare/wire each.
    @staticmethod
    def _recs(loader, slots, args):
        return _save_expand_records(args, slots['__body__'], slots.get('__entry__'),
                                    slots.get('__norestore__', False),
                                    slots['__tag__'], slots['__ln__'], _known_commands(loader))
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if '__body__' not in slots:
            return [], (), (), [], ()
        return _fold_sizes(loader, _CmdSAVE._recs(loader, slots, args), naming_commands)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if '__body__' not in slots:
            return
        for _ln, s in _CmdSAVE._recs(loader, slots, args):
            loader._wave2_alloc_line(s, filepath, _ln, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if '__body__' not in slots:
            return
        for _ln, s in _CmdSAVE._recs(loader, slots, args):
            loader._wave2_wire_line(s, filepath, _ln)


@_command('WRITE_OUT')
class _CmdWRITE_OUT(_Cmd):
    # WRITE_OUT n val -> Write val to LD_NI_OUT, then bump the pointer.
    @staticmethod
    def _subs(args):
        n, val = args[0], args[1]
        return [('NOVA', [n], {'Op': 'Write', 'El1': 'LD_NI_OUT', 'El2': val}),
                ('NOVA', [f'{n}_i'], {'Op': 'Add', 'El1': 'LD_NI_OUT', 'El2': 'C_1', 'Exit': 'LD_NI_OUT'})]
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _fold_sizes(loader, [(0, s) for s in _CmdWRITE_OUT._subs(rec[1])], naming_commands)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        for s in _CmdWRITE_OUT._subs(rec[1]):
            loader._wave2_alloc_line(s, filepath, lineno, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        for s in _CmdWRITE_OUT._subs(rec[1]):
            loader._wave2_wire_line(s, filepath, lineno)


@_command('READ_BODY')
class _CmdREAD_BODY(_Cmd):
    # READ_BODY n dst -> Read from LD_NI_PTR into dst, then bump the pointer.
    @staticmethod
    def _subs(args):
        n, dst = args[0], args[1]
        return [('NOVA', [n], {'Op': 'Read', 'El1': 'LD_NI_PTR', 'Exit': dst}),
                ('NOVA', [f'{n}_i'], {'Op': 'Add', 'El1': 'LD_NI_PTR', 'El2': 'C_1', 'Exit': 'LD_NI_PTR'})]
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _fold_sizes(loader, [(0, s) for s in _CmdREAD_BODY._subs(rec[1])], naming_commands)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        for s in _CmdREAD_BODY._subs(rec[1]):
            loader._wave2_alloc_line(s, filepath, lineno, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        for s in _CmdREAD_BODY._subs(rec[1]):
            loader._wave2_wire_line(s, filepath, lineno)


@_command('ALLOC_RAW')
class _CmdALLOC_RAW(_Cmd):
    # ALLOC_RAW [name] size target -> Move count + Voca ALLOC_RAW + Move result,
    # named __ar_<tag>_<ln> (or the given name for the first node).
    @staticmethod
    def _subs(slots, args):
        first_name, size_sym, target = args[0], args[1], args[2]
        n = f"{slots['__tag__']}_{slots['__ln__']}"
        nova_name = first_name or f'__ar_{n}'
        return [('NOVA', [nova_name], {'Op': 'Move', 'El1': size_sym, 'Exit': 'RA_ALLOC_COUNT'}),
                ('RVOCA', [f'__ar_{n}_j', 'ALLOC_RAW_SUB'], {}),
                ('NOVA', [f'__ar_{n}_k'], {'Op': 'Move', 'El1': 'RA_ALLOC_RESULT', 'Exit': target})]
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        return _fold_sizes(loader, [(0, s) for s in _CmdALLOC_RAW._subs(slots, args)], naming_commands)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        for s in _CmdALLOC_RAW._subs(slots, args):
            loader._wave2_alloc_line(s, filepath, lineno, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        for s in _CmdALLOC_RAW._subs(slots, args):
            loader._wave2_wire_line(s, filepath, lineno)


@_command('SWITCH')
class _CmdSWITCH(_Cmd):
    # Direct-build SWITCH: one case == one JEQ pair, built through the shared
    # _build_jeq a hand-written JEQ uses -- no synthesised JEQ text. Cases come
    # from _switch_cases (parses the raw body once, caches on the record).
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return [], (), (), [], ()
        # Each case's primary name, exactly the set the N separate JEQ text
        # records registered. The `_J` half stays Wave-2-allocated (a plain
        # JEQ's does too -- only ALLOC_TO pre-registers its `_J`/`_K`).
        return [], tuple(c[0] for c in _switch_cases(slots)), (), [], ()
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        for name, _reg, _val, _dest in _switch_cases(slots):
            if name in loader.symbols:
                continue
            addr = (loader._alloc_nova(name) if name in loader._nova_names
                    else loader._alloc_data(name))
            loader.symbols[name] = addr
            loader.interp.aether.aether[addr + _WORD] = addr
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        # Cases built in order, so _last_nova threads each JumpIf into the next
        # Equal, identical to N consecutive JEQ records.
        for name, reg, val, dest in _switch_cases(slots):
            loader._build_jeq(name, reg, val, dest)


@_command('PACK_BYTE', 'UNPACK_BYTE')
class _CmdPACK(_Cmd):
    # PACK_BYTE/UNPACK_BYTE tag word shift byte: 3 arithmetic NOVAs
    # (tag + tag_1 + tag_2) built through the shared _nova_lux, plus, if an
    # on-full/on-exhausted label is given, one JEQ pair (tag_J) via _build_jeq.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if len(args) < 4:
            return [], (), (), [], ()
        tag = args[0]
        names = [tag, f'{tag}_1', f'{tag}_2']
        if slots.get('Full') or slots.get('Exhausted'):
            names.append(f'{tag}_J')
        return [], tuple(names), (), [], ()
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if not args:
            return
        tag = args[0]
        # Only the tag + two arithmetic nodes name luces (the JEQ half, if any,
        # is minted in the wire pass by _build_jeq); the register args are
        # declared elsewhere.
        for name in (tag, f'{tag}_1', f'{tag}_2'):
            if name in loader.symbols:
                continue
            addr = loader._alloc_nova(name)
            loader.symbols[name] = addr
            loader.interp.aether.aether[addr + _WORD] = addr
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        cmd, args, slots = rec
        if len(args) < 4:
            return
        tag, word, shift, byte = args[0], args[1], args[2], args[3]
        if cmd == 'PACK_BYTE':
            loader._nova_lux(tag,        'Left', e1=byte, e2=shift, exit='RA_PACK_TMP')
            loader._nova_lux(f'{tag}_1', 'Or',   e1=word, e2='RA_PACK_TMP', exit=word)
            loader._nova_lux(f'{tag}_2', 'Add',  e1=shift, e2='C_8', exit=shift)
        else:
            loader._nova_lux(tag,        'Right', e1=word, e2=shift, exit=byte)
            loader._nova_lux(f'{tag}_1', 'And',   e1=byte, e2='C_255', exit=byte)
            loader._nova_lux(f'{tag}_2', 'Add',   e1=shift, e2='C_8', exit=shift)
        label = slots.get('Full') or slots.get('Exhausted')
        if label:
            loader._build_jeq(f'{tag}_J', shift, 'C_64', label)


@_command('MATCH')
class _CmdMATCH(_Cmd):
    # MATCH: compute the dispatcher from the group's items (reading Except like
    # any operand) and build each node-spec through the primitive wave, in order
    # -- the auto-link chain and NOLINK severing thread through as when these
    # were spliced loose. Items come from _match_items (parses body once, cached).
    @staticmethod
    def _gen(rec):
        _c, args, slots = rec
        gen, _ = _match_expand(_match_items(slots), args[0], slots.get('Except'),
                               slots['__tag__'], slots['__ln__'])
        return gen
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return [], (), (), [], ()
        # Register every NOVA/JEQ node the dispatcher builds, so a forward
        # reference to any of them is sized as a NOVA. `_J` halves stay
        # Wave-2-allocated, as for any JEQ.
        na = tuple(g[1][0] for _ln, g in _CmdMATCH._gen(rec)
                   if g and g[0] in ('NOVA', 'JEQ') and g[1])
        return [], na, (), [], ()
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        for _ln, grec in _CmdMATCH._gen(rec):
            loader._wave2_alloc_line(grec, filepath, _ln, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, _a, slots = rec
        if '__body__' not in slots:
            return
        for _ln, grec in _CmdMATCH._gen(rec):
            loader._wave2_wire_line(grec, filepath, _ln)


@_command('ALLOC_TO')
class _CmdALLOC_TO(_Cmd):
    # ALLOC_TO name Exit=dest Count=n: 3 chained NOVAs (name Move count ->
    # RA_ALLOC_COUNT; name_J Voca ALLOC -> RA_LINK; name_K Move result -> dest).
    # A Python-side fast path for the macros.re ALLOC_TO, used early (via SAVE,
    # BS_INTERN's lazy-alloc, ...) before LOAD_MAIN reaches that macro's own def.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if len(args) < 1 or not slots.get('Exit') or not slots.get('Count'):
            return [], (), (), [], ()
        name = args[0]
        if '{' in name or '{' in slots['Exit'] or '{' in slots['Count']:
            return [], (), (), [], ()
        return [], (name, name + '_J', name + '_K'), (), [], ()
    # No declare override: `name` is allocated by the universal macro-arg
    # pre-alloc (per _nova_names, exactly as before); name_J/name_K are minted
    # lazily by _nova_lux in the wire pass, as for any JEQ-shaped macro.
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if not args or not slots.get('Exit') or not slots.get('Count'):
            return
        name, dest, count = args[0], slots['Exit'], slots['Count']
        loader._nova_lux(name,        'Move', e1=count,              exit='RA_ALLOC_COUNT')
        loader._nova_lux(name + '_J', 'Voca', e1='ALLOC',            exit='RA_LINK')
        k = loader._nova_lux(name + '_K', 'Move', e1='RA_ALLOC_RESULT', exit=dest)
        loader._last_nova = k


# The simple direct-build macros below share one shape: args[0] IS the minted
# NOVA name (sized by _arg0_name_size), the args are allocated by the universal
# macro-arg pre-alloc (no declare override), and wire builds the luces through
# the same _nova_lux/_build_jeq a hand-written NOVA/JEQ uses. They are hardcoded
# Python fast paths (each also has a .re macro) because they run before
# LOAD_MAIN reaches those definitions -- same bootstrap reason as ALLOC_TO.

@_command('CLEAR')
class _CmdCLEAR(_Cmd):
    # CLEAR name El1=target -> NOVA name Move El1=C_0 Exit=target
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if args and slots.get('El1'):
            loader._nova_lux(args[0], 'Move', e1='C_0', exit=slots['El1'])


@_command('NOP')
class _CmdNOP(_Cmd):
    # NOP name -> NOVA name Move El1=C_0 Exit=C_0
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if len(args) >= 1:
            loader._nova_lux(args[0], 'Move', e1='C_0', exit='C_0')


@_command('JEQ')
class _CmdJEQ(_Cmd):
    # JEQ name El1=reg1 El2=reg2 Exit=dest -> Equal(name) + JumpIf(name_J).
    # name_J is minted lazily by _build_jeq in the wire pass.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if args and slots.get('El1') and slots.get('El2') and slots.get('Exit'):
            loader._build_jeq(args[0], slots['El1'], slots['El2'], slots['Exit'])


@_command('JZ')
class _CmdJZ(_Cmd):
    # JZ name El1=reg Exit=dest -> Equal(name, reg, C_0) + JumpIf(name_J).
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if args and slots.get('El1') and slots.get('Exit'):
            loader._build_jeq(args[0], slots['El1'], 'C_0', slots['Exit'])


@_command('HASH_STEP')
class _CmdHASH_STEP(_Cmd):
    # HASH_STEP name hash_reg byte_reg tmp_reg [to_reg]: the djb2 update step --
    #   name   : Mul(hash_reg, C_33) -> tmp_reg
    #   name_J : Add(tmp_reg, byte_reg) -> to_reg (default hash_reg)
    # name_J is minted here via _nova_lux (lazy alloc), like any JEQ half.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if len(args) < 4:
            return
        name, hash_reg, byte_reg, tmp_reg = args[0], args[1], args[2], args[3]
        to_reg = args[4] if len(args) >= 5 else hash_reg
        loader._nova_lux(name,        'Mul', e1=hash_reg, e2='C_33', exit=tmp_reg)
        j = loader._nova_lux(name + '_J', 'Add', e1=tmp_reg, e2=byte_reg, exit=to_reg)
        loader._last_nova = j


# ── Primitive commands ────────────────────────────────────────────────────
# The irreducible core -- NEW/SET/LINK/NOVA/NOVAE/NOLINK/RVOCA/RREDI -- now
# lives in _COMMANDS too, so EVERY command (primitive or compound) is handled
# in exactly one place: its own handler. Nothing branches on `cmd == "..."`
# inline in the wave functions any more; those keep only the cross-cutting
# facilities that run for all commands (Cmd= word-write, In=/region, generic
# macro-arg pre-alloc). This matches the Reca side, which already dispatches
# every one of these through one path (BS_LOOKUP -> Cmd= handler). The
# low-level mechanisms a handler builds with (_alloc/_define/_wire_word_value/
# _add_lumen/_nova_lux) stay ordinary methods -- they are not commands.

@_command('NOLINK')
class _CmdNOLINK(_Cmd):
    # NOLINK: reset the autolink chain so the next NOVA does not link back to
    # the previous one. (Python resets _last_nova=0 via _end_chain; wax.re's
    # EXP_CMD_NOLINK clears only RA_MC_PREV -- a known, documented divergence.)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        loader._end_chain()


@_command('LINK')
class _CmdLINK(_Cmd):
    # LINK src Rel=R Exit=T: add one typed lumen (src -R-> T). Wave 1 records
    # the (src, Exit) pair so the source lux is sized to hold its lumina;
    # Wave 2 resolves all three and writes the lumen.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if args and slots.get('Exit'):
            return [(args[0], slots['Exit'])], (), (), [], ()
        return [], (), (), [], ()
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        if not args or not slots.get("Rel") or not slots.get("Exit"):
            return
        src = loader._resolve(args[0])
        rel = loader._resolve(slots["Rel"])
        tgt = loader._resolve(slots["Exit"])
        loader._add_lumen(src, rel, tgt)


@_command('RVOCA', 'RREDI')
class _CmdVocaRedi(_Cmd):
    # RVOCA name Body=B: a call node (Voca B, exit=RA_LINK).
    # RREDI name:        a return node (Redi RA_LINK), and resets the chain.
    # args[0] is the node's own minted NOVA name (needs full NOVA_SIZE from
    # its first mention -- via size below, the same fact the generic
    # naming-command branch produced before this was a handler).
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        return _arg0_name_size(rec)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if len(args) < 1:
            return
        name = args[0]
        _a = loader._lookup(name)
        if _a is None or _a not in loader._nova_addrs_set:
            # Not yet allocated, or was a data lux stub — (re-)allocate as NOVA.
            loader._define_nova(name)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        cmd, args, slots = rec
        if len(args) < 1:
            return
        name = args[0]
        body = slots.get("Body")
        # Template names from FOR macro bodies — skip during Wave 2
        if '{' in name or (body and '{' in body):
            return
        lux_addr = loader._own_addr_if_declared(name)
        if lux_addr is None:
            # Not pre-allocated — allocate now
            lux_addr = loader._alloc_nova(name)
            loader._bind(name, lux_addr)
        a = loader.interp.aether.aether
        a[lux_addr + _WORD]  = lux_addr   # self-ref
        a[lux_addr + _NEXT]  = 0           # in case this address is reused
        a[lux_addr + _NOVA_SIZE - 1] = 0    # pad (last slot before NOVA_SIZE)
        # RVOCA: e1=call target (Body=), exit=RA_LINK (return address).
        # RREDI: e1=RA_LINK (return via link register), no exit.
        # op and every operand name are resolved by _emit_nova itself, same
        # lazy-stub-on-forward-reference behavior every other caller gets.
        loader._emit_nova(
            None, "Voca" if cmd == "RVOCA" else "Redi",
            e1=body if cmd == "RVOCA" else "RA_LINK",
            exit="RA_LINK" if cmd == "RVOCA" else None,
            addr_override=lux_addr,
        )
        if cmd == "RREDI":
            loader._last_nova = 0  # RREDI resets chain (like NOLINK after)


@_command('SET')
class _CmdSET(_Cmd):
    # SET [Name=]X [At=o] [Value=V | Of=Y]: declare X (a blank name, exactly
    # what NEW wants) in the alloc wave; write its word -- the value -- in the
    # wire wave, where the "where does the value go" knowledge lives. Bare
    # `SET X` is a self-ref (word(X)=addr(X)); Value=/Of=/positional forms all
    # funnel into _wire_word_value, which decides number/text/reference.
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if len(args) >= 2:
            loader._declare(args[0])
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        name = slots.get("Name") or (args[0] if args else None)
        if name is None:
            return
        value_args = args[1:] if args and args[0] == name else list(args)
        of = slots.get("Of")
        value = slots.get("Value")
        if of is not None:
            value_args = ["Of", of]
        elif value is not None:
            value_args = [value]
        elif not value_args:
            value_args = [name]   # bare SET X: self-ref, word(X) = addr(X)
        loader._wire_word_value(
            loader._resolve(name) + loader._set_offset(args, slots),
            [name] + list(value_args))


@_command('NEW')
class _CmdNEW(_Cmd):
    # NEW [Name=]X: declare a blank name X. NEW X Count=n Width=w: X is a
    # POINTER (cell 0) to a fresh region of Count*Width cells that follows it,
    # allocated as one block (absorbs the old SPAN), placed in X's own combo
    # block when X carries an OWN/SAVE/LOCK region. All allocation is in this
    # wave; wire is a no-op. The region MEMBERSHIP (In=/K_/RA_) is the
    # cross-cutting region facility in _wave1_lcount_line, shared with every
    # declaration -- not NEW's own logic.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if ('Count' in slots or 'Width' in slots) and args:
            # Reserve Count*Width+1 for the region (a symbolic size is
            # resolved later, not sizable here) -- same job as SPAN did.
            try:
                span = 1 + int(slots.get('Count', 1)) * int(slots.get('Width', 1))
            except (TypeError, ValueError):
                return [], (), (), [], ()
            return [], (), (), [(args[0], span)], ()
        return [], (), (), [], ()
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        if "Count" in slots or "Width" in slots:
            name = slots.get("Name") or (args[0] if args else None)
            if not name or loader._has(name):
                return
            def _num(key, dflt):
                raw = slots.get(key)
                if raw is None:
                    return dflt
                v = loader._resolve_count_width(raw, filepath, lineno)
                return v if (v is not None and v >= 1) else dflt
            count = _num("Count", 1)
            width = _num("Width", 1)
            # Pointer (cell 0) + region + one slack cell, as ONE allocation,
            # in name's own OWN/SAVE/LOCK block if it has one.
            key = loader._combo_key(name) if name else frozenset()
            size = 1 + count * width
            ptr = loader._bump_alloc_combo(key, size) if key else loader._alloc(size)
            loader._bind(name, ptr)
            region = ptr + 1
            loader._write(ptr, region)
            loader._region_start[name] = region
            return
        if len(args) >= 1:
            loader._declare(args[0])
    # wire: no-op -- allocation already done above (was `if cmd == "NEW": return`).


@_command('NOVA')
class _CmdNOVA(_Cmd):
    # NOVA [name] Op=O [El1=..][El2=..][Exit=..]: one instruction lux, allocated
    # at load time (named or anonymous) and wired directly. Its own inline
    # extra-lumen keys (In1=/Op1/... , anything not a reserved slot) are sized
    # here and wired in wire; an anonymous NOVA keys them by (filepath, idx).
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        extra = []
        if args:
            name = args[0]
            op = slots.get('Op', '')
            for _k, _v in slots.items():
                if _k not in _NOVA_RESERVED_SLOTS:
                    extra.append((name, _k, _v))
            if op == 'Key' and 'Op1' in slots:
                extra.append((name, 'Op1', slots['Op1']))
            nova_add = () if ('{' in name or '{' in op) else (name,)
            return [], nova_add, (), [], tuple(extra)
        # Anonymous NOVA: no name to key extras by -- use anon_key.
        for _k, _v in slots.items():
            if _k not in _NOVA_RESERVED_SLOTS:
                extra.append((anon_key, _k, _v))
        if slots.get('Op') == 'Key' and 'Op1' in slots:
            extra.append((anon_key, 'Op1', slots['Op1']))
        return [], (), (), [], tuple(extra)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        _c, args, slots = rec
        a = loader.interp.aether.aether
        if not args:
            # Anonymous: always a fresh alloc; keys its extras by (filepath, idx)
            # -- the same synthetic prepass key its size() used. Stash the addr
            # for wire (same line, called right after).
            addr = loader._alloc_nova('', prepass_key=(filepath, idx))
            a[addr + _WORD] = addr
            loader._last_anon_nova_addr = addr
            return
        name = args[0]
        existing = loader._lookup(name)
        if existing is not None:
            # A 1-lux data stub (word=addr but not a real NOVA) is re-allocated
            # as a proper NOVA; an existing NOVA is reused.
            if existing not in loader._nova_addrs_set:
                addr = loader._alloc_nova(name)
                loader._bind(name, addr)
                a[addr + _WORD] = addr
            else:
                addr = existing
                a[addr + _WORD] = addr
        else:
            addr = loader._alloc_nova(name)
            loader._bind(name, addr)
            a[addr + _WORD] = addr
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        _c, args, slots = rec
        op = slots.get("Op")
        if not args:
            # Anonymous: use the address declare stashed for this exact line.
            addr = loader._last_anon_nova_addr
            loader._last_anon_nova_addr = 0
            if not addr or not op:
                return
            is_term = op in _TERMINATORS
            loader._emit_nova(
                None, op, e1=slots.get("El1"), e2=slots.get("El2"),
                exit=slots.get("Exit"), auto_next=True, addr_override=addr)
            loader._wire_extra_lumens(addr, slots)
            loader._wire_key_op1(addr, op, slots)
            if is_term:
                loader._last_nova = 0
            return
        name = args[0]
        # Template names (contain '{') are FOR-macro bodies expanded at runtime.
        if not op or '{' in name or '{' in op:
            return
        is_term = op in _TERMINATORS
        addr = loader._emit_nova(
            name, op, e1=slots.get("El1"), e2=slots.get("El2"),
            exit=slots.get("Exit"), auto_next=True)
        loader._wire_extra_lumens(addr, slots)
        loader._wire_key_op1(addr, op, slots)
        if is_term:
            loader._last_nova = 0


@_command('NOVAE')
class _CmdNOVAE(_Cmd):
    # NOVAE label Op=O ...: synthesize a NOVA at *interpreter runtime* (the
    # builder-sequence form of NOVA). Sizing -- the label, plus At='s
    # extra-lumen keys -- is here. The alloc and the runtime-builder emission
    # are large mechanisms, so declare/wire delegate to Loader._novae_declare /
    # _novae_wire; dispatch is still uniform through this one handler.
    @staticmethod
    def size(loader, rec, naming_commands, anon_key):
        _c, args, slots = rec
        if not args:
            return [], (), (), [], ()
        name = args[0]
        op = slots.get('Op', '')
        extra = []
        if slots.get('At'):
            # At=X: X already has a load-time address, so this line's own
            # extra keys are sized under X (not this NOVAE label).
            at_name = slots['At']
            for _k, _v in slots.items():
                if _k not in _NOVA_RESERVED_SLOTS:
                    extra.append((at_name, _k, _v))
        nova_add = () if ('{' in name or '{' in op) else (name,)
        return [], nova_add, (), [], tuple(extra)
    @staticmethod
    def declare(loader, rec, filepath, lineno, idx):
        loader._novae_declare(rec, filepath, lineno, idx)
    @staticmethod
    def wire(loader, rec, filepath, lineno):
        loader._novae_wire(rec, filepath, lineno)


# ── Block-opener handlers ────────────────────────────────────────────────
# One handler per parse-mode. _parse_indent_blocks looks the handler up by the
# `Open=<mode>` value declared in .re (see _discover_block_openers), calling
# `_open_<mode>`, so a new opener is: one `Cmd=WORD Open=mode` line in .re plus
# an `_open_<mode>` function here -- no list of openers to keep in step.
#
# Each returns an _OpenResult: `ctx` is the context to push (None = open no
# block), `emit` is records to append now (only FOR uses it, to pass its own
# header line through as ordinary text). `took_norestore` says this handler
# consumed the pending NORESTORE flag (only SAVE does). The behaviours a
# malformed opener can have -- open, skip, pass through as text, raise -- are
# all expressed through this one shape, so the loop stays uniform.
#
# (There used to be a `restore` field too, records deferred to the block's
# close -- SAVE's restore half. SAVE now collects its body like every other
# block and _save_expand_records assembles setup + body + restore in one go,
# so nothing is deferred and the field is gone, along with _pending_restore.)
_OpenResult = namedtuple('_OpenResult', ['ctx', 'emit', 'took_norestore'])
_NO_OPEN = _OpenResult(None, (), False)


def _open_switch(toks, stripped, lineno, cur_indent, file_tag, norestore):
    # SWITCH reg [NAME] -- NAME (if any) is ignored: direct-built luces carry
    # no textual name. Malformed (no reg) is a silent no-op.
    if len(toks) < 2:
        return _NO_OPEN
    reg = toks[1]
    block_name = toks[2] if len(toks) >= 3 else None
    ctx = {'mode': 'switch', 'reg': reg, 'block_name': block_name,
           'anon': block_name is None, 'body': [],
           'opener_indent': cur_indent, 'stack': IndentStack(cur_indent)}
    return _OpenResult(ctx, (), False)


def _open_match(toks, stripped, lineno, cur_indent, file_tag, norestore):
    # MATCH NAME [Except=LABEL] -- opens a byte-string dispatch group; NAME is
    # its own globally-addressable entry point.
    if len(toks) < 2:
        raise ValueError(f"MATCH at line {lineno}: needs a name")
    block_name = toks[1]
    except_label = None
    for t in toks[2:]:
        if t.startswith('Except='):
            except_label = t.split('=', 1)[1]
    ctx = {'mode': 'match', 'name': block_name, 'except_label': except_label,
           'body': [], 'opener_indent': cur_indent, 'stack': IndentStack(cur_indent)}
    return _OpenResult(ctx, (), False)


def _open_def(toks, stripped, lineno, cur_indent, file_tag, norestore):
    # DEF NAME -- collect the raw body; _def_expand_records does the rest at
    # load time (name the entry, resolve {RET}, append RREDI).
    if len(toks) < 2:
        raise ValueError(f"DEF at line {lineno}: needs a name")
    ctx = {'mode': 'def', 'block_name': toks[1], 'body': [],
           'opener_indent': cur_indent, 'stack': IndentStack(cur_indent)}
    return _OpenResult(ctx, (), False)


def _open_save(toks, stripped, lineno, cur_indent, file_tag, norestore):
    # SAVE [entry] reg... -- a plain block now, like FOR/DEF: collect the body,
    # and _save_expand_records assembles setup + body + restore at load time.
    # RA_LINK is stripped unconditionally (the call stack already preserves it).
    # Malformed with no tokens passes through as ordinary text; with tokens but
    # no savable regs it opens nothing (and raises only if a name was given,
    # since that name has no lux to land on). The opener's own lineno and the
    # NORESTORE flag are carried on the context to the record it emits at close.
    if len(toks) < 2:
        return _OpenResult(None, ((lineno, stripped),), False)
    reg_tokens = toks[1:]
    entry_name = None
    if reg_tokens and not any(reg_tokens[0].startswith(p) for p in _REG_PREFIXES):
        entry_name, reg_tokens = reg_tokens[0], reg_tokens[1:]
    regs = [r for r in reg_tokens if r != 'RA_LINK']
    if not regs:
        if entry_name:
            raise ValueError(
                f"line {lineno}: 'SAVE {entry_name}' has no registers "
                f"left to save (RA_LINK doesn't count -- the call "
                f"stack already covers it). Name the body's own "
                f"first real instruction '{entry_name}' directly "
                f"instead of wrapping it in SAVE.")
        return _OpenResult(None, (), True)
    ctx = {'mode': 'save', 'regs': regs, 'entry': entry_name or '',
           'norestore': norestore, 'ln': lineno, 'body': [],
           'opener_indent': cur_indent, 'stack': IndentStack(cur_indent)}
    return _OpenResult(ctx, (), True)


def _open_for(toks, stripped, lineno, cur_indent, file_tag, norestore):
    # FOR elem0 elem1 ... -- unlike the other openers, FOR's header line also
    # passes through as ordinary text (emitted here), then opens the loop.
    # Malformed FOR (no elements) is just that ordinary line, opening nothing.
    emit = ((lineno, stripped),)
    if len(toks) < 2:
        return _OpenResult(None, emit, False)
    slots = _for_parse_header(toks[1:])
    ctx = {'mode': 'for', 'slots': slots, 'body': [],
           'opener_indent': cur_indent, 'stack': IndentStack(cur_indent)}
    return _OpenResult(ctx, emit, False)


def _switch_cases(slots):
    """A SWITCH record's own expansion rule: parse its raw body (__body__) into
    (name, reg, val, dest) case tuples. Called by whichever wave needs the cases
    (sizing, declare, wire). This logic used to live inline in the shared
    _parse_indent_blocks collector; moved here so the collector only gathers
    body lines (uniform for every block) and every SWITCH-specific decision --
    the `>` multi-value form, the anon vs named gensym -- lives in one place.
    Result is cached back onto the record so repeat wave passes don't re-parse.

    Naming is byte-for-byte what the old inline code produced: a named block
    numbers its cases `<block_name>_<idx>`, an anonymous one
    `__sw_<file_tag>_<lineno>_<idx>`, idx a single counter running across the
    whole block's cases; lineno is each body LINE's own number.
    """
    if '__switch__' in slots:
        return slots['__switch__']
    reg = slots.get('__reg__')
    block_name = slots.get('__bn__')
    file_tag = slots.get('__tag__', '')
    cases = []
    idx = 0
    for lineno, toks_s in slots.get('__body__', ()):
        if '>' in toks_s:
            gt = toks_s.index('>')
            vals = toks_s[:gt]
            dest = toks_s[gt + 1] if gt + 1 < len(toks_s) else ''
        else:
            if len(toks_s) < 2:
                continue
            vals, dest = [toks_s[0]], toks_s[1]
        if not dest:
            continue
        for val in vals:
            if block_name is not None:
                jeq_name = f'{block_name}_{idx}'
            else:
                jeq_name = f'__sw_{file_tag}_{lineno}_{idx}'
            cases.append((jeq_name, reg, val, dest))
            idx += 1
    slots['__switch__'] = cases
    return cases


def _for_body_ov(slots):
    """A FOR record's own expansion rule (first half): split its uniform body
    (__body__, each entry (lineno, text, rel)) into body lines (rel == 0) and
    override lines (rel > 0). This rel-based split used to live in the shared
    collector; moved here so the collector only gathers body entries (uniform
    for every block) and every FOR-specific decision lives in one place.
    _for_expand_records consumes the two lists; result cached on the record."""
    if '__bl__' in slots:
        return slots['__bl__'], slots['__ov__']
    body_lines, override_lines = [], []
    for lineno, toks, rel in slots.get('__body__', ()):
        (override_lines if rel > 0 else body_lines).append((lineno, toks))
    slots['__bl__'] = body_lines
    slots['__ov__'] = override_lines
    return body_lines, override_lines


def _match_items(slots):
    """A MATCH record's own expansion rule: turn its body (__body__) into the
    item list _match_expand consumes. A body item is (lineno, X): X a token list
    (leaf) -> {'kind':'leaf', name, label}, or a nested (cmd,args,slots) block
    record -> {'kind':'group', ...} with the names that group resolves (a nested
    MATCH, embedded at collection like Reca's record[2]; recursion computes its
    members). Kept in source order. Cached back onto the record. Moved out of
    the shared collector so the collector only gathers body items, uniform for
    every block; the leaf/group decision -- MATCH-specific -- lives here."""
    if '__items__' in slots:
        return slots['__items__']
    items = []
    for _lineno, x in slots.get('__body__', ()):
        if isinstance(x, tuple):
            # nested block record. A nested MATCH resolves to a group whose
            # members are the full set of names it resolves (recurse); any other
            # nested block has no leaf/label meaning inside a MATCH, so skip it.
            _cmd, _args, _mslots = x
            if _cmd == 'MATCH':
                gname = _args[0] if _args else ''
                items.append({'kind': 'group', 'name': gname, 'entry': gname,
                              'members': _match_flat_names(_match_items(_mslots))})
        elif len(x) >= 2:                 # leaf: token list `name label ...`
            items.append({'kind': 'leaf', 'name': x[0], 'label': x[1]})
    slots['__items__'] = items
    return items


def _parse_indent_blocks(stripped_lines: list, loader, file_tag: str = '') -> list:
    """Parse indent-structured .re source into a flat list of (lineno, item),
    one per surviving line, in source order.

    Despite the historical name this no longer rewrites block commands into
    synthesised text. Most items are still the plain text of an ordinary line
    (parsed to a record later, by _read_and_parse); a block opener
    (SWITCH/MATCH/DEF/SAVE/FOR) is consumed here and replaced by ONE ready
    (cmd, args, slots) tuple record carrying its body as token lists, which its
    Wave-1 sizer and Wave-2 build handler expand at load time. Nothing is edited
    in place; everything is passed through untouched or handed on as a
    structured record. ({RET} in a DEF body is resolved later, in
    _def_expand_records, scoped to that DEF's own return -- not here.)

    Uses a STACK of contexts for correct nesting. Each context records its
    opener_indent so closing is relative: a line with indent <= opener_indent
    closes the context (pops the stack).

    Indent levels within a context are RELATIVE to body_base:
      rel = current_indent_chars - body_base
      rel == 0  → new item at this context's level
      rel >  0  → sub-item (belongs to previous item)

    This means a nested opener works correctly inside SAVE/DEF blocks.

    file_tag: a short, stable per-file string used (with lineno and a local
    index) to build generated names deterministically -- see the SWITCH/
    ALLOC_RAW/SAVE branches below. Without it, generated names came from
    loader._switch_counter/_alloc_counter, shared, monotonic state whose
    value depends on how many times this function has run before, on this
    object, for any file. That was safe only because every call site
    happened to run in one fixed, unvarying order (three separate scanners
    plus the real load, each re-expanding the same text and each nudging
    the same counters) -- correct by accident of sequence, not by
    construction. (file, lineno, local index) is unique regardless of how
    many times or in what order a given line gets expanded, because it
    depends only on the line's own identity, not on prior calls.
    """
    result = []
    _ctx_stack: list = []
    _pending_norestore: bool = False

    def _count_indent(raw: str) -> int:
        n = 0
        for ch in raw:
            if ch == '\t':  n += 4
            elif ch == ' ': n += 1
            else: break
        return n

    def _close_ctx(ctx, lineno=0):
        # Build the one record this block emits (its whole body carried on it),
        # then place it: embedded into the parent's body if this block closed
        # while still nested in another context, else appended to result at the
        # top level. Every block's body->records expansion is its own helper
        # (_switch_cases / _match_items / _for_* / _def_/_save_expand_records),
        # run later by whichever wave needs it -- this collector only gathers.
        mode = ctx['mode']
        rec = None
        if mode == 'save':
            # regs + body + entry + NORESTORE flag + gensym seed (opener's
            # tag/lineno). _save_expand_records assembles setup + body + restore.
            m = {'__body__': ctx['body'], '__entry__': ctx['entry'],
                 '__norestore__': ctx['norestore'], '__tag__': file_tag,
                 '__ln__': ctx['ln']}
            rec = (ctx['ln'], ('SAVE', list(ctx['regs']), m))
        elif mode == 'switch':
            # An empty block (no cases) yields nothing, exactly as before.
            if ctx['body']:
                rec = (lineno, ('SWITCH', [], {
                    '__body__': ctx['body'], '__reg__': ctx['reg'],
                    '__bn__': ctx['block_name'], '__tag__': file_tag}))
        elif mode == 'for':
            rec = (lineno, ('FOR', [], {'__slots__': ctx['slots'],
                                        '__body__': ctx['body']}))
        elif mode == 'def':
            rec = (lineno, ('DEF', [ctx['block_name']],
                            {'__body__': ctx['body'],
                             '__tag__': file_tag, '__ln__': lineno}))
        elif mode == 'match':
            m_slots = {'__body__': ctx['body'], '__tag__': file_tag, '__ln__': lineno}
            if ctx['except_label']:
                m_slots['Except'] = ctx['except_label']
            rec = (lineno, ('MATCH', [ctx['name']], m_slots))
        if rec is None:
            return
        # Nesting as structure: a block that closes while still inside another
        # context becomes a body item of its parent, so the parent's own helper
        # walks it -- mirrors Reca, where a nested opener is a body record of its
        # parent (record[2]). A top-level block goes to result. (No block nests
        # in the corpus today, so this only ever takes the result branch here.)
        if _ctx_stack:
            _ctx_stack[-1]['body'].append(rec)
        else:
            result.append(rec)

    # ── Unified name-mode helpers (used by CHAIN, SWITCH) ─────────────
    def _parse_iris_mode(tokens, macro_idx):
        """Return block_name, or None for manual mode (explicit names in body).

        block_name=None → manual (explicit names in body)
        block_name=NAME → auto-named (IRIS_N)
        """
        if len(tokens) <= macro_idx:
            return None
        return tokens[macro_idx]

    for lineno, _text in stripped_lines:
        cur_indent = _count_indent(_text)
        stripped = _text.strip()

        # A line with no content -- blank, or a comment that _strip_comments
        # reduced to nothing -- takes no part in structure at all. It does
        # not open a level, does not close one, is not counted. Skipped
        # before anything else looks at it.
        #
        # This is the whole rule now. Blocks used to close on a genuine blank
        # line and never on a dedent, which is why a comment line had to be
        # told apart from an empty one (a comment inside a body would
        # otherwise have ended the body early -- that was a real bug) and why
        # 182 blank lines were once inserted project-wide to terminate bodies
        # that had been relying on dedent. Both of those disappear here: the
        # distinction has nothing left to decide, and a forgotten blank line
        # can no longer silently reshape a block.
        if not stripped:
            continue

        # Close every level this line has stepped back out of. A level ends
        # at the first line whose indent is no deeper than the line that
        # opened it -- and that line is then processed normally by whatever
        # context (if any) still encloses it, which is what lets a body
        # return to its parent's level and carry on in the same pass.
        while _ctx_stack and cur_indent <= _ctx_stack[-1]['opener_indent']:
            _close_ctx(_ctx_stack.pop(), lineno)

        # Contexts close ONLY on a genuine blank line (handled above), never
        # on dedent. A non-blank line whose indent doesn't reach the
        # context's body level is simply skipped -- neither a close signal
        # nor ordinary code. This requires every real macro body in the
        # project to end with an actual blank line before whatever code
        # follows it; see migrate_blank_close.py, which was run once to
        # insert that blank line everywhere a body previously relied on
        # dedent-to-close (182 sites project-wide, confirmed empirically).
        if _ctx_stack:
            ctx = _ctx_stack[-1]
            mode = ctx['mode']

            # Central indent-level tracking: see IndentStack above for the
            # rule (verified in isolation with test_indent_stack.py before
            # this integration). 'skip' covers both a stray/garbage indent
            # AND a line that lands back on the opener's own indent
            # (level 0) -- both are inert here, matching the earlier
            # single-level scheme's "stray line below body_base" case.
            # 'level' otherwise starts at 1 for the first real body line
            # (what the old scheme called rel==0) and increases for each
            # deeper sub-level (old scheme's rel>0). rel's numeric value
            # was never used anywhere in this project beyond the ==0/>0
            # distinction (confirmed project-wide), so replacing it with
            # a discrete level number changes nothing existing while also
            # correctly supporting returning to an already-seen shallower
            # level after going deeper -- something the old single
            # body_base could not express at all.
            action, level = ctx['stack'].classify(cur_indent)
            if action == 'skip':
                continue
            rel = level - 1  # 0=item (old rel==0), >0=sub-item (old rel>0)

            # Uniform collect -- one rule for every block, no per-mode branch:
            #   * a body line that is itself a block opener falls through to the
            #     opener dispatch below, which opens a sub-context; its record is
            #     embedded into THIS body at close (see _close_ctx). Nesting is
            #     resolved as structure, exactly like Reca's record[2] (a nested
            #     opener becomes a body record of its parent).
            #   * every other line is collected as its TOKEN list -- no text kept
            #     and no re-parse; each block's helper reads the tokens. FOR also
            #     keeps rel so it can split body (rel==0) from override (rel>0).
            _btoks = _tokenize_quoted(stripped)
            _bfirst = _btoks[0].upper() if _btoks else ''
            if _discover_block_openers().get(_bfirst) is None:
                ctx['body'].append((lineno, _btoks, rel) if mode == 'for'
                                   else (lineno, _btoks))
                continue
            # opener body line: fall through to open the nested sub-context

        # ── Non-context line (or save/func passthrough) ───────────────────────
        toks = stripped.split()
        first = toks[0].upper() if toks else ''

        # Block openers: dispatched by the `Open=<mode>` declared for this
        # command in .re (see _discover_block_openers), to the `_open_<mode>`
        # handler. No hardcoded list of openers -- the corpus's own Open= keys
        # are the set. Each handler returns what to emit now, what to defer to
        # close, and the context to push (if any); FOR uniquely also emits its
        # own header line as ordinary text, exactly as the passthrough below
        # would for any other line.
        _opener_mode = _discover_block_openers().get(first)
        if _opener_mode is not None:
            _fn = globals().get(f'_open_{_opener_mode}')
            if _fn is None:
                raise ValueError(f"command {first!r} declares Open={_opener_mode} "
                                 f"but loader has no _open_{_opener_mode} handler")
            _res = _fn(toks, stripped, lineno, cur_indent, file_tag, _pending_norestore)
            if _res.took_norestore:
                _pending_norestore = False
            result.extend(_res.emit)
            if _res.ctx is not None:
                _ctx_stack.append(_res.ctx)
            continue

        if first == 'ALLOC_RAW' and len(toks) >= 3:
            # ALLOC_RAW [name] size target -- direct-build: one record carrying
            # the operands plus its source location (for the deterministic
            # __ar_<file>_<line> node names). The Wave-2 handler decomposes it
            # into the Move / ALLOC_RAW-Voca / Move triple at load time.
            # With a 4th token the first node takes that explicit name (a real
            # jump target) instead of an anonymous __ar_N.
            if len(toks) >= 4:
                first_name, size_sym, target = toks[1], toks[2], toks[3]
            else:
                first_name, size_sym, target = '', toks[1], toks[2]
            result.append((lineno, ('ALLOC_RAW', [first_name, size_sym, target],
                                    {'__tag__': file_tag, '__ln__': lineno})))
            continue

        if first == 'NORESTORE':
            _pending_norestore = True; continue

        if first in ('PACK_BYTE', 'UNPACK_BYTE'):
            # PACK_BYTE tag word shift byte [Full=on_full_label]
            # UNPACK_BYTE tag word shift dest [Exhausted=on_exhausted_label]
            #
            # tag is positional, always first, like HASH_STEP's own label --
            # not Name=: with three more bare register operands right after
            # it, exclusion (the one keyless token is the name) cannot tell
            # which of four keyless tokens is the label, so a fixed position
            # is what settles it, the same way HASH_STEP already does. Used
            # to be optional (Name=tag, defaulting to an auto-generated
            # __pb_{file}_{line} tag when omitted) -- all three real call
            # sites always supplied one anyway, so making it a mandatory
            # positional slot drops the special case for free.
            #
            # The pure arithmetic of packing/unpacking one byte into/out of
            # an 8-bytes-per-word accumulator -- nothing about where the
            # word came from or what happens once it's full/exhausted
            # (allocate a new lux and chain, vs advance a pointer into an
            # already-sized block, are both legitimate, different-cost
            # strategies for different callers with different constraints
            # -- see BUGS.md/HISTORY.md for the session that found this
            # arithmetic reimplemented three times, once with a real bug
            # (multiplication standing in for a shift, not equivalent once
            # carries are possible), while the allocation strategy around
            # it turned out to have a real reason to differ per caller and
            # was deliberately left alone).
            #
            # Expands inline (plain NOVA/JEQ text, flows through the normal
            # wave1/wave2 pipeline exactly like hand-written code) rather
            # than as a subroutine call: this sits on at least one hot path
            # (BS_PACK_TOKBUF, every token during tide replay), where an
            # RVOCA/RREDI pair per byte would be a real, measurable cost a
            # shared macro must not add.
            if len(toks) < 5:
                raise ValueError(f"{first} at line {lineno}: needs tag, word, shift, byte/dest registers")
            tag = toks[1]
            word_reg, shift_reg, byte_reg = toks[2], toks[3], toks[4]
            # Direct-build: one record carrying the operands. The on-full /
            # on-exhausted label stays a KEYED operand (Full=/Exhausted=) in the
            # record's slots -- so it is an ordinary operand the Wave-2 handler
            # reads and the key scan discovers, not a directive consumed here.
            slots = {}
            for t in toks[5:]:
                if t.startswith('Full=') or t.startswith('Exhausted='):
                    k, v = t.split('=', 1)
                    slots[k] = v
            result.append((lineno, (first, [tag, word_reg, shift_reg, byte_reg], slots)))
            continue

        # WRITE_OUT NAME VAL / READ_BODY NAME DST -- direct-build: one record
        # carrying the operands; the Wave-2 handler builds the pair of NOVAs at
        # load time from args (name-produced nodes: NAME and NAME_i), nothing
        # decomposed here.
        if first == 'WRITE_OUT' and len(toks) >= 3:
            result.append((lineno, ('WRITE_OUT', [toks[1], toks[2]], {})))
            continue

        if first == 'READ_BODY' and len(toks) >= 3:
            result.append((lineno, ('READ_BODY', [toks[1], toks[2]], {})))
            continue

        # Ordinary line: passes through as text, to be parsed to a record later
        # by _read_and_parse. (Block openers never reach here -- they are
        # dispatched above; FOR emits this same passthrough itself, then opens.)
        result.append((lineno, stripped))

    # EOF: close all remaining contexts
    while _ctx_stack:
        _close_ctx(_ctx_stack.pop(), 0)
    return result


def _read_re_file(filepath: str, loader=None, expand_autogen: bool = True) -> list:
    """Read a .re file and return [(lineno, line), ...].

    Pipeline: read raw lines → strip //...// comments → expand AUTOGEN_*
    markers → expand indent syntax.

    AUTOGEN_* marker expansion must run BEFORE _parse_indent_blocks, not
    after, so _parse_indent_blocks only ever sees ordinary, already-real
    lines and needs no AUTOGEN-awareness of its own. This mattered acutely
    back when the key recognizer was a MATCH byte-tree: a marker
    ("AUTOGEN_MATCH_CANDIDATES <prefix>", since removed with that byte-tree)
    sat inside a MATCH block's indented body, and running the passes in the
    wrong order made _parse_indent_blocks's MATCH-context parser read the
    still-unexpanded marker line as one literal, bogus candidate and finalize
    the whole MATCH group on the spot -- LOAD_ELK_ROUTE (lexer.re) then
    generated a single unconditional jump with all real keys silently gone.
    Today's markers (AUTOGEN_KEY_TABLE/REGS/CLEARS) expand to flat NEW/SET
    lines outside any block, so the failure mode is gone, but the ordering
    is kept as the correct pipeline shape.

    expand_autogen=False is for _discover_key_vocabulary/
    _discover_nova_naming_commands, which must read raw, unexpanded text:
    expanding AUTOGEN_* here would call back into whichever discovery
    function generates that expansion's own data, which is exactly the
    call in progress -- infinite recursion the first time either
    discovery function scans a file that carries one of these markers.
    Skipping expansion for their scan is harmless: neither discovery
    function is looking for anything an AUTOGEN marker's own expansion
    would produce.
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

    # RCALL/RRET: REMOVED (macros no longer exist, zero .re callers — see
    # macros.re's "RCALL/RRET: REMOVED" note). No expansion needed.
    expanded = list(stripped)
    if expand_autogen:
        expanded = _expand_autogen_markers(expanded)

    # Open= is a load-time DECLARATION directive (it marks a command as a block
    # opener -- see _discover_block_openers, which reads it from raw text), not
    # a runtime operand. Strip it here so it never reaches the node builder or
    # the key vocabulary: an opener's declaration node stays exactly what it was
    # without it, and Open never becomes a runtime key (so it also never touches
    # the AUTOGEN keyed-arg machinery). _discover_block_openers does its own raw
    # read and so still sees it.
    expanded = [(ln, _OPEN_DIRECTIVE_RE.sub('', text)) for ln, text in expanded]

    # file_tag: short, stable, generated-name-safe id, deterministic from the
    # path alone (see _file_tag).
    return _parse_indent_blocks(expanded, loader, _file_tag(filepath))


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

    INVARIANT (settled, checked more than once — do not re-litigate):
    `parts` is `line.split()` — safe ONLY for single-token values (numbers,
    bare symbol names, bare flags). It silently breaks any value containing
    a space (a quoted string like `SET="hello world"` becomes three broken
    tokens: `'"hello', 'world"'` split apart). `cmd_part` (this function's
    third return value, the raw un-split line) is what every caller that
    might see a space-containing value uses instead — see
    `_wire_word_value` (checks `'"' in cmd_part` before ever touching
    `parts[2]`) and `_parse_new_modifiers` (its own quote-aware tokenizer
    over `cmd_part`, never `parts`). This is a deliberate split of
    responsibility, not an oversight: `parts` for positional single-word
    fields, `cmd_part` for anything that can contain whitespace. Any new
    command handler that accepts a possibly-quoted value must follow the
    same rule.

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
        "_last_nova",
        "_end_lux_addr",
        "_data_addrs",
        "_nova_addrs",
        "_nova_addrs_set",
        "_lazy_ok",          # lazy resolve only inside the waves, never during bootstrap
        "_lazy_stubs",       # names minted by a lazy-resolve miss (see _resolve)
        "_last_anon_nova_addr",  # hand-off: anonymous NOVA's addr, alloc_line -> wire_line
        "_region_start",        # {span_name: element size in luces} — SPAN X Y N
        "_lumen_prepass",
        "_lumen_count",      # dict[src_addr, int] — O(1) lumen append
        "_nova_names",        # set of names that appear as RVOCA/RREDI/NOVA targets
                              # anywhere (global prepass) — these always need the
                              # full NOVA_SIZE layout, regardless of which command
                              # sees the name FIRST during allocation.
        "_nova_naming_commands",  # cache: set of macro names discovered (from
                              # macros.re itself) to use this same convention —
                              # see _discover_nova_naming_commands.
                              # immediate Wave 1 allocation: SPAN targets
                              # (a bare 1-lux data stub from elsewhere would
                              # silently pre-empt SPAN's own sizing) and ALIAS
                              # targets, standalone or via NEW's ALIAS= modifier
                              # (resolved in Wave 2 -- SEAL merged into
                              # _wave2_alloc_line, see its own docstring --
                              # once every name project-wide is known; nothing
                              # else may allocate a lux for these names first).
        "_name_regions",  # {name: {region_str, ...}} -- every In[N]=REGION a
                        # name ever carries (last declaration wins per
                        # region string, across the whole project -- see
                        # _wave1_lcount_line), as a plain set of whatever
                        # strings actually got used. No fixed vocabulary:
                        # "OWN"/"SAVE"/"LOCK" are just the three strings
                        # real declarations happen to use today, not a
                        # closed enum this dict enforces. Placement
                        # (_combo_key/_combo_blocks below) works for any
                        # region string uniformly; a region string having
                        # any further RUNTIME consequence (LOCK's write
                        # guard is the one example today) is a separate,
                        # explicit thing some other piece of code has to
                        # specifically recognize -- adding a name to this
                        # set never implies behavior beyond colocation.
                        #
                        # OWN (Pattern W, BUGS.md: per-thread physical
                        # locality) and SAVE (the name's word field must
                        # stay a stable, never-reused address -- K_CURSOR-
                        # style pointer target, RA_JEQ_FLAG_PTR-style flag
                        # target) are themselves just placement concerns,
                        # same as any future region string would be. LOCK
                        # is the one region string with a real runtime
                        # consequence beyond placement (see _lock_ranges):
                        # once written, the name's word may never be
                        # written again except via Key (SAVE protects the
                        # address from being *reused*; LOCK protects the
                        # *content* from being overwritten by anyone
                        # unauthorized, at runtime -- two different kinds
                        # of guarantee, established directly with the
                        # author: content-write-once needs its own axis,
                        # not a special case of address-uniqueness).
        "_region_slot_role",  # {name: {slot_num: region_str}} -- which
                        # region string a given In{N}= slot last set for a
                        # name, so a later In{N}=0 on the SAME numbered
                        # slot can clear only that one region, not all of
                        # them. Bare In= (no number) never enters this --
                        # its own "0" means "clear everything", always,
                        # regardless of what bare In= itself last set.
        "_combo_blocks",  # dict[frozenset({'own','save','lock',...}), [base, next, end]]
                          # One generic reservation mechanism for every
                          # combination of the flags above that actually
                          # occurs in the project -- built lazily, one
                          # entry per real combo (today: just a few, not
                          # all 2**3 mathematically possible ones). This
                          # replaces three, then would have become four+,
                          # hand-copied block-pair implementations
                          # (_reserve_own_block/_reserve_save_block/
                          # _reserve_ownsave_block were each ~15 lines of
                          # the exact same pattern) with one. A name's
                          # combo key is frozenset of whichever of
                          # own/save/lock apply to it; two names with the
                          # same key share the same physical block,
                          # regardless of which flags produced that key.
        "_bump",
        "_aether_size",   # cached len(aether) for _bump_alloc hot path
        "_water",
        # _switch_counter and _alloc_counter removed: generated names are
        # now deterministic from (file, line, index) instead of shared
        # incrementing state — see _parse_indent_blocks's docstring.
        "_lit_const_cache", # dict[int value, addr] — memoized C_<N> literal registers
                              # minted on demand by _resolve for bare numeric
                              # operands that aren't an existing C_<N> constant.
        "scopes",          # {scope_id: {name: addr}} — per-scope local name
                           # bindings, on top of the global `symbols` table.
                           # A scope_id is deterministic (a block's file_tag +
                           # lineno), so declare and wire re-enter the SAME
                           # scope and see the SAME addresses. Purely a
                           # COMPILE-TIME resolution structure: the runtime
                           # only ever sees resolved flat addresses ("no
                           # scopes at core" stays literally true). Empty
                           # while nothing has opened a scope -> behaves
                           # exactly like a single global table.
        "_scope_stack",    # [scope_id, ...] — the scopes currently open, a
                           # block handler pushing before it processes its body
                           # and popping after (in every wave). Top = current.
        "_scope_locals",   # {scope_id: frozenset(names)} — the fixed set of
                           # names that bind locally in each scope, from the
                           # block handler's pre-scan of its own body (Vis=Public
                           # and the block's own entry name excluded). Decides
                           # local-vs-global by KNOWING up front, so a forward
                           # reference lazy-stubs into the right home and is
                           # never repointed (the loader's no-repoint invariant).
    )

    def __init__(self, interp) -> None:
        self.interp        = interp
        self.symbols: dict = {}
        self._loaded_files = set()
        self._lux_registry = {}
        self._next_file_id = 0
        self._last_nova           = 0
        self._end_lux_addr       = 0
        self._data_addrs   = []   # ordered list of all lux addresses (data + instr)
        self._nova_addrs  = []   # ordered list of instruction lux addresses
        self._nova_addrs_set: set = set()
        self._lazy_ok = False            # see _resolve; flipped on once the waves start
        self._lazy_stubs: set = set()    # names a lazy-resolve miss created
        self._last_anon_nova_addr: int = 0
        self._region_start: dict = {}    # region name -> address of its contents
        self._lumen_prepass: dict = {}    # {sym_name: [tgt_name, ...]} from global scan
        self._lumen_count: dict = {}       # {src_addr: n_lumens_written} for O(1) append
        self._nova_names: set = set()       # names needing full NOVA layout (global scan)
        self._nova_naming_commands = None   # cache for _discover_nova_naming_commands()
        self._name_regions: dict = {}
        self._region_slot_role: dict = {}
        self._combo_blocks: dict = {}
        self._bump         = _BOOTSTRAP_FIRST_FREE
        self._aether_size  = len(self.interp.aether.aether)  # cached for _bump_alloc
        self._water        = _BOOTSTRAP_FIRST_FREE - 1
        self._lit_const_cache: dict = {}
        self.scopes: dict = {}          # scope_id -> {name: addr}; see __slots__
        self._scope_stack: list = []    # currently-open scope_ids, top = current
        self._scope_locals: dict = {}   # scope_id -> frozenset(local names)

    # ── Name binding & scope resolution ───────────────────────────────────────
    # One rule: a name binds in the CURRENT scope. Global is just the outermost
    # scope -- at top level (no block open) the current scope IS global, so a
    # top-level name is public and a body-of-block name is local, with no flag
    # for either common case. Vis=Public is the one opt-out (a name declared
    # inside a block that must reach the global table). This is entirely a
    # compile-time resolution layer; the runtime only ever sees the flat
    # addresses these produce.

    def _scope_of(self, name):
        """The innermost open scope that declares `name` as one of its locals,
        or None (meaning global). Membership comes from the block's pre-scan
        (see _scope_enter), known before the first reference -- so a name's
        home is decided by KNOWING, not guessing, and never repointed later."""
        for sid in reversed(self._scope_stack):
            if name in self._scope_locals.get(sid, ()):
                return sid
        return None

    def _lookup(self, name):
        """Resolve a name to its address: if a local of some open scope, that
        scope's map (None until allocated -> a forward ref lazy-stubs into the
        same scope); otherwise the global table. With no scope open this is
        exactly self.symbols.get(name)."""
        sid = self._scope_of(name)
        if sid is not None:
            return self.scopes.get(sid, {}).get(name)
        return self.symbols.get(name)

    def _has(self, name) -> bool:
        """True if name is already bound in its home (local scope or global)."""
        return self._lookup(name) is not None

    def _bind(self, name, addr) -> None:
        """Bind name->addr in its home: the innermost open scope that declares
        it local, else the global table. Which one is fixed by the pre-scanned
        local set, so a name always binds where its references already resolve.
        With no scope open this is exactly self.symbols[name]=addr."""
        sid = self._scope_of(name)
        if sid is not None:
            self.scopes.setdefault(sid, {})[name] = addr
        else:
            self.symbols[name] = addr

    def _scope_enter(self, scope_id, local_names) -> None:
        """Open a scope: `local_names` (from the block handler's pre-scan of its
        own body) is the fixed set of names that bind locally here. scope_id is
        deterministic (block file_tag+lineno) so declare and wire re-enter the
        same scope with the same set and the same addresses."""
        self._scope_stack.append(scope_id)
        self._scope_locals[scope_id] = frozenset(local_names)

    def _scope_leave(self) -> None:
        self._scope_stack.pop()

    # ── Allocation helpers ────────────────────────────────────────────────────

    def _alloc(self, n: int) -> int:
        """The one minimal allocation primitive: n contiguous luces from
        the general aether, bump-allocated in a single move. Returns start
        address.

        No key/routing parameter: OWN/SAVE/LOCK combo-block routing is a
        decision only the caller (NEW's own logic, via _combo_key) knows
        how to make, not something this primitive should branch on --
        a caller needing a combo block calls _bump_alloc_combo directly
        instead. (This used to be folded into a `key`-routing wrapper
        here; that wrapper mixed "which cursor" -- the caller's business
        -- into "bump a cursor" -- this function's only job -- so it's
        gone, not renamed.)

        This is the bootstrap allocator. Loader maintains its own cursor
        as a Python int. At freeze time the final values are written into
        K_CURSOR.word and K_WATERMARK.word so aria code (alloc.re) and
        the compiled binary (preamble.ll) can continue allocating from
        the same point.

        The bounds guard below is not business logic to push onto every
        caller -- it is a universal safety invariant true for literally
        every possible caller, and the project already has a documented
        case of the opposite approach failing (see ALLOC's own
        count=0 guard in alloc.re: hand-guarded at each risky call site,
        found and fixed twice, missed a third time, then centralized here
        once). Kept here deliberately, not stripped out.
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

    def _combo_key(self, name: str) -> frozenset:
        """Whatever region strings `name` carries (In[N]=REGION -- any
        string, no fixed vocabulary; see _name_regions), as a frozenset.
        Empty frozenset means none apply (ordinary general-aether
        allocation, no combo block involved at all). Region strings are
        stored lowercased (_wave1_lcount_line normalizes on the way in),
        matching the one place a region string has a further runtime
        consequence today -- _reserve_combo_blocks' own 'lock' in key
        check for _lock_ranges."""
        return frozenset(self._name_regions.get(name, ()))

    def _reserve_combo_blocks(self, span_sizes: dict | None = None):
        """Reserve one contiguous range per distinct combination of region
        strings that actually occurs among declared names -- built lazily,
        one block per real combo, not one per mathematically possible
        combination, and not restricted to any fixed set of region names
        (whatever strings _name_regions actually holds). Must run after
        the full-project prepass and before Wave 1 allocates a single
        name, so the very first allocation for each name already lands in
        its correct block -- no later relocation pass needed, same shape
        as every other "global prepass informs the first allocation"
        mechanism already in this file (_nova_names).

        `span_sizes` (from _wave1_lcount_line's SPAN detection) gives the
        real size for a SPAN'd name in one of these combos (e.g.
        SK_SEAL_NAMEPOOL) -- without it this would fall back to plain-
        data-lux sizing and silently truncate the block, a real bug
        found and fixed the same session SAVE was introduced.
        """
        span_sizes = span_sizes or {}
        by_combo: dict = {}
        for name in self._name_regions:
            key = self._combo_key(name)
            if key:
                by_combo.setdefault(key, []).append(name)
        for key, names in by_combo.items():
            if key in self._combo_blocks:
                continue
            total = 0
            for name in names:
                if name in span_sizes:
                    total += span_sizes[name]
                elif name in self._nova_names:
                    # NOVA_SIZE + 2*n_extra, no separate +1 -- matches
                    # _alloc_sized/check_overlap.py's formula (pad-slot
                    # reuse already absorbs the terminator cell; a stale
                    # "+1 whenever n_extra" copy of the pre-pad-reuse
                    # formula lived here, silently over-reserving by one
                    # cell for any combo'd NOVA name with extras -- found
                    # via RA_MA0, which is exactly such a name).
                    n_extra = len(self._lumen_prepass.get(name, []))
                    total += _NOVA_SIZE + 2 * n_extra
                else:
                    n = len(self._lumen_prepass.get(name, []))
                    total += 1 + 2 * n + 1
            base = self._alloc(total)
            self._combo_blocks[key] = [base, base, base + total]
        # LOCK enforcement: interpreter._lock_ranges is what _write/_key and
        # the generic exit-operand guard actually check (see interpreter.py);
        # nothing populated it before this. Rebuilt fully (not incrementally
        # appended) each call -- cheap, and correct regardless of how many
        # times this runs across multiple _wave1_lcount calls, unlike trying
        # to track "was this key's range already added" separately. Any
        # combo containing 'lock' contributes its range, whatever else it's
        # combined with (today: {save,lock}; a future {own,lock} or bare
        # {lock} combo is handled identically, not specially).
        self.interp._lock_ranges = [
            (blk[0], blk[2]) for key, blk in self._combo_blocks.items() if 'lock' in key
        ]

    def _bump_alloc_combo(self, key: frozenset, n: int) -> int:
        """Bump-allocate n contiguous luces from the reserved block for
        this exact OWN/SAVE/LOCK combination. Mirrors _bump_alloc exactly,
        against that combo's own cursor, so every name sharing this same
        combo ends up physically contiguous with every other one
        regardless of which file/line declares it.
        """
        blk = self._combo_blocks[key]
        ptr = blk[1]
        end = ptr + n
        if end > blk[2]:
            # Should never happen: the block was sized in Wave 1 (lcount),
            # before Wave 2 allocated anything, from the exact same scan
            # that this allocation now draws from. A mismatch means the
            # sizing scan and this allocation path disagree about which
            # names carry which flags -- a real bug to investigate, not a
            # case to silently paper over.
            raise LoadError(
                f"{sorted(key)} block overflow: needed {end}, reserved end {blk[2]} "
                f"(reserved base {blk[0]})")
        blk[1] = end
        if end - 1 > self._water:
            self._water = end - 1
        return ptr

    def _alloc_sized(self, name: str, base: int, force_terminator: bool,
                      prepass_key=None) -> int:
        """Shared sized-allocation helper for _alloc_data/_alloc_nova.

        `prepass_key`, when given, looks up _lumen_prepass under that key
        instead of `name` -- the anonymous-NOVA case, where `name` is
        always '' (nothing to key a shared name by) but the real prepass
        key is id(rec), unique per source occurrence (see
        _wave1_lcount_line's matching 'elif cmd == NOVA and not args'
        branch). `name` itself still drives combo-block routing below --
        an anonymous NOVA never carries OWN/SAVE/LOCK (nothing names it to
        tag), so that stays correct either way.
        Size = base + 2*n_lumens + (1 if force_terminator else 0).
        Lumen count comes from global prepass (_lumen_prepass). The two
        callers differ only in base slot count and in whether the trailing
        terminator/sentinel slot is unconditional (data: the base=1 word
        slot has no built-in pad of its own, so a real terminator cell is
        always needed, n=0 or not) or never separately needed at all
        (nova: slot 6, part of base, doubles as the terminator when
        n_lumens==0, or as the first extra lumen's own rel when n_lumens>0
        -- either way a real terminator cell always falls naturally at
        base-1+2*n_lumens, already covered by base+2*n_lumens with no
        further +1, unlike the old "NOVA_SIZE + 2*n + 1" scheme this
        replaced, which double-counted a terminator cell that pad's own
        reuse had already absorbed).

        Routes to a specific OWN/SAVE/LOCK combo block when name carries
        any of those flags, the general aether otherwise -- this
        function's own decision to make (see _combo_key/
        _reserve_combo_blocks), not something the allocation primitives
        themselves know about."""
        n = len(self._lumen_prepass.get(prepass_key if prepass_key is not None else name, []))
        size = base + 2 * n + (1 if force_terminator else 0)
        key = self._combo_key(name) if name else frozenset()
        addr = self._bump_alloc_combo(key, size) if key else self._alloc(size)
        self._data_addrs.append(addr)
        return addr

    def _alloc_data(self, name: str = '') -> int:
        """Allocate data lux [word, rel0, exit0, rel1, exit1, ..., 0].
        Each lumen = 2 luces (rel, exit), plus an unconditional trailing 0
        slot — the lumen-chain sentinel, explicitly allocated so that
        lumen-scanning code can safely read it without bounds checks.

        If name is anywhere the target of RVOCA/RREDI/NOVA (per the global
        _nova_names prepass), it needs the full NOVA_SIZE layout instead —
        delegate to _alloc_nova so the FIRST mention of the name (whatever
        command that happens to be: NEW, SEAL's X or Y, NEWSET, ...)
        already reserves enough room. Without this, a later RVOCA/RREDI/NOVA
        line for the same name would silently no-op against the existing
        (too-small) allocation — see _define's "already exists" guard — and
        the lux would overlap with whatever gets allocated right after it."""
        if name and name in self._nova_names:
            return self._alloc_nova(name)
        return self._alloc_sized(name, 1, force_terminator=True)

    def _declare(self, name: str) -> int:
        """Ensure `name` exists as an allocated, registered symbol --
        idempotent (a no-op if it's already there). This is NEW's entire
        job. It's also exactly as much of SET's job as happens at
        allocation time: SET's own distinguishing work (writing the
        value) happens later, in the wiring wave, where NEW has nothing
        analogous at all -- see `if cmd == "NEW": return` there. Shared
        here instead of two copies of the same three lines under
        different cmd checks.
        """
        a = self._lookup(name)
        if a is None:
            a = self._alloc_data(name)
            self._bind(name, a)
        return a

    def _alloc_nova(self, name: str = '', prepass_key=None) -> int:
        """Allocate a NOVA lux with compact fixed-slot layout.
        Slots 0-6: word, op, e1, e2, exit, next, pad (NOVA_SIZE=7 luces).
        Slot 5 (next) written by _link_novas in pass2.
        Slot 6+ (extra lumina) appended here from _lumen_prepass, keyed by
        `prepass_key` when given (the anonymous-NOVA case -- see
        _alloc_sized's matching note) or by `name` otherwise.

        If name carries any of OWN/SAVE/LOCK, allocate from that exact
        combination's reserved block instead of the general aether --
        see _alloc_sized's matching note.
        """
        addr = self._alloc_sized(name, _NOVA_SIZE, force_terminator=False,
                                  prepass_key=prepass_key)
        self._nova_addrs.append(addr)
        self._nova_addrs_set.add(addr)
        return addr


    def _add_lumen(self, src_addr: int, rel_addr: int, tgt_addr: int) -> None:
        """Append (rel_addr, tgt_addr) as next extra lumen of src_addr's lux.

        Not a separate primitive: an address computed by arithmetic (base +
        running count * stride), then two _write calls -- the same
        shape as _emit_nova's own "four _write calls, one per fixed
        slot". The running count (_lumen_count) is the one piece of state
        this needs; it plays the same role K_CURSOR plays for
        _alloc's own bump allocation, just kept in a Python-side
        dict instead of an
        Aether-resident register (Reca's own mirror of this, the lcount
        table read by LOAD_LCOUNT_GET, already lives in the graph -- see
        LOAD_ALLOC_SIZED's matching base/terminator split on the sizing
        side, which this seeding mirrors on the writing side).

        For NOVA luces: extra lumina start right at the pad slot itself
        (SLOT_PAD, offset 6) — pad doubles as the first entry when extras
        exist, rather than a separate always-empty slot before them (see
        interpreter.py's _nova_extra_skip and synth.re's SYNTH_WALK_NOVA,
        both offset=6 for the same reason). For Data luces: lumina append
        from slot 1 (DATA_LUMEN_OFFSET) onward, as always.

        The base offset is chosen directly from which set src_addr is in
        -- not from a pre-seeded count -- so this needs no seeding step at
        all, unlike the old NOVA_SIZE-based scheme it replaced (that one
        needed _emit_nova to seed count=3 up front, and builder-class
        macros whose target lux never went through _emit_nova could reach
        here unseeded and clobber op/e1 at [1]/[2]). Checking set
        membership on every call costs one dict lookup already being paid
        for _lumen_count itself, and is correct regardless of whether
        anything seeded this address before.
        """
        if not src_addr or not tgt_addr:
            return
        count = self._lumen_count.get(src_addr, 0)
        base = _PAD if src_addr in self._nova_addrs_set else _DATA_OFF
        i = base + count * LUMEN_PAIR
        self._write(src_addr + i,     rel_addr)
        self._write(src_addr + i + 1, tgt_addr)
        self._lumen_count[src_addr] = count + 1

    def _wire_extra_lumens(self, addr: int, slots: dict) -> None:
        """Write a plain NOVA line's own arbitrary [Name][N]= extra-lumen
        relations -- In1=/Op1=/Next1=/... or a genuinely new name, per
        _NOVA_RESERVED_SLOTS (the fixed Op/El1/El2/Exit/At/Next/Op1/Op2/
        Op3 keys are excluded, already wired by the caller).

        The rel key itself (e.g. "In1") is treated as a real identity
        lux, same convention as Op/El1/Exit/Entry/... (CONVENTIONS.md's
        NEWREF rule: an identity lux's own word must be its own address,
        or Equal -- which dereferences *both* operands -- silently
        compares 0==0 against every other never-self-ref'd name). Unlike
        those, nothing pre-declares an arbitrary new key via NEWREF
        ahead of time, so it's self-ref'd here, at first use. Idempotent
        -- harmless to repeat if this exact key was already self-ref'd
        by an earlier line.
        """
        for key, val in slots.items():
            if key in _NOVA_RESERVED_SLOTS:
                continue
            rel_addr = self._resolve(key)
            self._write(rel_addr, rel_addr)
            tgt_addr = self._resolve(val)
            self._add_lumen(addr, rel_addr, tgt_addr)

    def _wire_key_op1(self, addr: int, op, slots: dict) -> None:
        """Op=Key Op1=Move (etc.): Op1 names which write shape this
        specific Key instruction resolves to at runtime (interpreter.py's
        _resolve_key_shape, cached per-pc once resolved -- the actual
        write always re-runs, only the shape choice is memoized). A real
        extra-lumen relation on the instruction, same convention as any
        other [Name][N]= -- but Op1 is one of _NOVA_RESERVED_SLOTS (NOVAE
        uses the same key name for something else entirely: which
        resolution mode El1's own helper node uses there), so
        _wire_extra_lumens skips it on purpose and this wires it
        separately, only when Op is actually Key.
        """
        if op != "Key" or "Op1" not in slots:
            return
        rel_addr = self._resolve("Op1")
        self._write(rel_addr, rel_addr)
        tgt_addr = self._resolve(slots["Op1"])
        self._add_lumen(addr, rel_addr, tgt_addr)

    def _define(self, name: str, alloc_fn) -> int:
        """Create or return a lux for name. alloc_fn() → addr.

        No self-ref written here. NEW's real meaning is a blank, zeroed
        lux -- Wave 1's own direct handling of a NEW line already leaves
        it that way (calls _alloc_data directly, no self-ref write); this
        matches that, so a name's word content no longer silently depends
        on whether it happened to be lazily resolved (used before its own
        NEW/SET line is reached) or allocated the ordinary way. Self-ref
        only ever happens when a SET line for that name actually runs
        (bare `SET X` defaults to it, but the author still had to write
        the SET) -- never as a side effect of mere allocation, which is
        what a stub minted here would be. Found and fixed the same
        session this was written: SK_SES_SREG_1-4 (wax.re) relied on
        getting self-ref "by default", and silently didn't -- the two
        paths disagreed, and which one ran depended on accidental file
        order. _define_nova (below) still asserts its own self-ref
        independently, right after calling this -- the NOVA invariant
        (slot 0 = self-ref) is deliberate and unrelated to this change.
        """
        a = self._lookup(name)
        if a is not None:
            return a
        addr = alloc_fn()
        self._bind(name, addr)
        return addr


    def _define_data(self, name: str) -> int:
        return self._define(name, lambda: self._alloc_data(name))


    def _define_nova(self, name: str) -> int:
        addr = self._define(name, lambda: self._alloc_nova(name))
        self.interp.aether.aether[addr + _WORD] = addr  # NOVA invariant: slot 0 = self-ref
        return addr


    def _end_lux(self) -> int:
        """The single shared 'flow stops here' lux. Allocated on first need.

        Not chained into anything: it is a destination, never a step. Its op
        is End, which returns 0 and stops the run.

        Why this exists at all: SLOT_NEXT=0 means "my successor is my
        physical neighbour" (see _link_novas). It therefore could not also
        mean "I have no successor" -- those two are different facts, and
        writing both as an absent value is what let a block whose flow was
        meant to stop silently inherit whatever lux allocation happened to
        put next to it. Rather than give "stop" its own magic value in
        SLOT_NEXT (a third case in the interpreter's hot path, on every
        step forever), it gets an ordinary address like any other target:
        the interpreter is unchanged and pays nothing.
        """
        if self._end_lux_addr:
            return self._end_lux_addr
        addr = self._define_nova("LUX_END")
        self._emit_nova("LUX_END", "End", auto_next=False)
        self._last_nova = 0          # _emit_nova(auto_next=False) parks it there
        self._end_lux_addr = addr
        return addr


    def _end_chain(self) -> None:
        """Close the running auto-link chain: its tail's flow stops here.

        Every place that used to write `self._last_nova = 0` because the
        chain is BROKEN (NOLINK, end of file) calls this instead. The
        places that write it because the tail is a TERMINATOR (Jump,
        JumpReg, End, Redi) keep writing it directly and correctly: those
        opcodes compute their own successor and never read SLOT_NEXT, so
        stamping them would be writing into a slot nobody reads.
        """
        prev = self._last_nova
        self._last_nova = 0
        if not prev:
            return
        a = self.interp.aether.aether
        if a[prev + _NEXT] == 0:
            a[prev + _NEXT] = self._end_lux()


    def _link_novas(self, prev: int, nxt: int) -> None:
        """Write nxt into SLOT_NEXT (slot 5) of prev NOVA lux — only when not physically adjacent.

        If nxt == prev + NOVA_SIZE, the interpreter's fall-through (SLOT_NEXT=0 → pc+NOVA_SIZE)
        handles it automatically. Writing is only needed for explicit non-adjacent links.
        NOLINK resets _last_nova to 0 so this is never called for isolated blocks.
        """
        if not prev or not nxt:
            return
        if nxt == prev + _NOVA_SIZE:
            return  # adjacent: fall-through (SLOT_NEXT stays 0)
        self.interp.aether.aether[prev + _NEXT] = nxt


    def _chain_nova(self, addr: int) -> None:
        """Append addr to the running auto-link chain and make it the tail.

        The one place the chain advances. Three sites used to carry their
        own copy of this two-step -- _emit_nova, the RVOCA/RREDI path and the
        macro helper _nova_lux -- each writing SLOT_NEXT inline and then
        moving the cursor. They agreed, but only by hand: the rule about
        when a link is implicit (physical adjacency) lives in _link_novas,
        and an inline copy that forgets it silently produces a chain the
        interpreter walks differently.

        Callers that must break the chain instead (NOLINK, RREDI) set
        _last_nova to 0 directly -- that is a different act, not an append.
        """
        if self._last_nova:
            self._link_novas(self._last_nova, addr)
        self._last_nova = addr

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
            lux  = self._alloc(1)                      # stride=1, matches Reca
            a[lux] = word
            if first is None:
                first = lux
            prev_lux = lux
        # NUL terminator — separate slot with word=0
        lux = self._alloc(1)
        a[lux] = 0
        if first is None:
            first = lux
        return first


    def _run_pass_over(self, handler, label: str,
                       file_data: list, file_order=None) -> None:
        """Execute handler over every line of file_order (or file_data).

        Resets _last_nova between files so auto-link chains don't bleed.
        Called by _run_waves (via its local _run_pass helper) for both the
        allocate and wire passes, so this iteration logic lives in one place.
        """
        # Lazy resolve is legal from here on: we are inside a wave, where a
        # name not yet seen is an ordinary forward reference, not a bug. The
        # bootstrap that runs before any wave builds the base luces itself and
        # must keep failing loudly on a miss -- stubbing there would allocate
        # among the bootstrap constants and shift the whole layout.
        self._lazy_ok = True
        order = file_order if file_order is not None else file_data
        for fp, file_id, start_ptr, records in order:
            self._end_chain()
            for idx, (lineno, rec) in enumerate(records):
                try: handler(rec, fp, lineno, idx)
                except LoadError as e:
                    print(f"[{fp}:{lineno}] {label}: {e}", file=sys.stderr)
                except SystemExit: raise
                except Exception as e:
                    print(f"[{fp}:{lineno}] {label}: {type(e).__name__}: {e}",
                          file=sys.stderr)
            self._end_chain()

    def _run_waves(self, read_cache: dict, wire_files: list) -> None:
        """Wave 2 (build): package file_data, allocate, wire, then finalise.

        Wave 0 (parse) and Wave 1 (lcount sizing scan) have already run, in
        that order, before this is called (see auto_discover/_load_group).

        The first step captures each file's allocation start pointer — this
        must happen here, right after Wave 1 reserved its combo blocks and
        right before Wave 2 mints its first lux, so start_ptr/end_ptr bracket
        exactly this file's own allocation range for the lux registry.

        Wave 2 then builds the graph: allocate every name, then wire operands,
        lumina and Next pointers, macro files first so macro bodies exist
        before the files that invoke them.

        The two passes are NOT a cross-file barrier in the sizing sense —
        Wave 1 already provides every size, and lazy resolve can mint a
        correctly-sized lux for a name not yet declared. But the split is
        load-bearing for a different reason, found by trying the merge and
        measuring what broke: macros are Reca programs that execute during
        wiring, and they read a name's *value*, not merely its address. A
        lazily-minted stub has the right address and the right size, and
        zero content. Splitting the pass guarantees every declaration has
        run before any macro executes; merging does not, and a macro that
        reads a stub computes from zeroes.

        The result is a graph that passes every structural check and is
        semantically wrong: with the merge applied, iris --lint stays
        completely clean (no phantoms, no dead refs, no bounds violations)
        while self-hosting falls to 0/42, and 661 operand slots on
        macro-produced luces lose the name they should hold — EXP_SWITCH_T4.El1
        drops LOAD_READ_TOKEN, LOAD_CSS_SPANCK.El1 drops
        LOAD_CMD_SPAN_IMPL, and so on. Lazy stubs more than double, 187 to
        440, which is the same fact seen from the other side.

        Three cheaper explanations were measured and ruled out first: stub
        sizing is correct (zero names re-allocated after being stubbed),
        the auto-link chain does not straddle the halves (allocation never
        touches _last_nova), and file order is not the trigger (running the
        merged pass in default order fails identically). Merging therefore
        requires changing when macros run, not how names resolve.

        `wire_files` may be a subset of what Wave 0 read: auto_discover reads
        the seed file so its declarations are visible to lcount, but does not
        wire it here — `load_file` already ran the seed's own waves.

        Called by both auto_discover and _load_group so the wave protocol
        lives in exactly one place.
        """
        file_data = []
        for fp in wire_files:
            if fp in self._loaded_files:
                continue
            self._loaded_files.add(fp)
            file_id   = self._next_file_id; self._next_file_id += 1
            start_ptr = self._bump                    # after Wave 1 reserves, before Wave 2 allocs
            _lines, records = read_cache[fp]
            file_data.append((fp, file_id, start_ptr, records))
        if not file_data:
            return

        def _run_pass(handler, label, file_order=None):
            self._run_pass_over(handler, label, file_data, file_order)

        self.interp.update_relations(self.symbols)
        _run_pass(self._wave2_build_line, "Wave 2 (build)")

        for fp, file_id, start_ptr, lines in file_data:
            self._lux_registry[file_id] = {
                "name": os.path.basename(fp), "filepath": fp,
                "start_ptr": start_ptr, "end_ptr": self._bump,
            }
        self.interp.update_relations(self.symbols)
        # Register load aspects AFTER the final update_relations so they
        # are not overwritten by the dispatch rebuild.
        self._setup_load_aspects()

    def _discover_nova_naming_commands(self):
        """Scan macros.re itself to find every macro whose body, anywhere
        within its NEWREF...next-NEWREF span, writes its caller's first
        positional argument (RA_MA0) into RA_MC_LUX before a WRITE_NOVA_SLOTS-
        style self-reference write -- i.e. 'NOVA <label> Move El1=RA_MA0
        ... Exit=RA_MC_LUX'. This is the universal calling-convention
        signature for "this macro's first argument (parts[1] on the call
        site line) becomes the address of a new, self-referential NOVA
        entry, so it needs the full NOVA_SIZE=7 layout from its *first*
        mention, not whatever smaller layout a name picks up if first seen
        as a plain NEW/NEWREF/NEWSET argument".

        Discovering this automatically (rather than hand-maintaining a list
        of command names) means any future macro written the same way is
        covered with zero changes here. The previous hand-maintained list
        (RVOCA/RREDI/NOP/JZ/JEQ/CLEAR/ALLOC_TO) silently missed LR/LT/
        RCALL_AT/EMIT/EMITI/PUTBYTE/WALK_ONE/LINK_OP/WALK_NOVA/UNLINK_OP/LX/LH
        -- all of which follow this exact convention -- which is what broke
        RTB_LUX_BODY (named via LR, referenced earlier in yaku.re as a
        plain register before its own definition) -- see BUGS.md.

        A macro's body is *not* required to use RA_MA0 at its own entry
        label specifically (WALK_ONE's entry label uses RA_MA3 for an
        internal sub-step; RA_MA0/"name" itself is only wired at a later
        instruction within the same body) -- so this walks the whole span
        between one NEWREF and the next, not just the first line.

        Cached on first call (expand.re/synth.re don't change mid-run).
        """
        if self._nova_naming_commands is not None:
            return self._nova_naming_commands

        # Scans every .re file, wherever it lives: a command is declared by
        # its own line (Cmd=/WAX_CMD), so any file -- today commands.re, later
        # a library -- may declare one. This used to open a hardcoded
        # <root>/commands.re and silently skip when it was not there, which
        # dropped every command declaration once files moved into
        # subdirectories (load/commands.re).
        macro_paths = re_sources()
        commands = set()
        for macros_path in macro_paths:
            current_macro = None   # a WAX_CMD block never spans files
            # Reads via the low-level tokenizer directly, not _read_and_parse:
            # this function is itself one of the two sources _known_commands
            # unions together (see its docstring), so going through
            # _read_and_parse (which calls _known_commands) here would
            # recurse. Only WAX_CMD/NOVA/NOVAE matter, and all three are
            # unconditional bootstrap primitives -- no command-recognition
            # dispatch is needed to find them.
            for _lineno, text in _read_re_file(macros_path, self, expand_autogen=False):
                if not isinstance(text, str): continue  # direct-build record, no text form
                p = _tokenize_quoted(text)
                if not p:
                    continue
                cmd = p[0].upper()
                args, slots = _split_args_slots(p[1:])
                if cmd == 'WAX_CMD':
                    # Declared, not inferred. Both the command's own name and
                    # its body are luces by construction, and the command
                    # list is exactly the set of these lines -- no scanning
                    # for a marker that happened to sit nearby.
                    nm = slots.get('Name') or (args[0] if args else None)
                    bd = args[1] if len(args) > 1 else None
                    if nm:
                        commands.add(nm)
                        self._nova_names.add(nm)
                        current_macro = nm
                    if bd:
                        self._nova_names.add(bd)
                    continue
                # Syn=X: same registration WAX_CMD did (X is a command word
                # and a NOVA-kind lux), but declared as a slot on the
                # handler's own line instead of a standalone WAX_CMD line.
                # The handler is this line's own name; its NOVA-kind-ness is
                # already covered by its own NOVA/NOVAE declaration.
                _syn = slots.get('Cmd')
                if _syn:
                    commands.add(_syn)
                    self._nova_names.add(_syn)
                    # The handler is this line's own label. For a NOVA/NOVAE
                    # builder it is already NOVA-kind via its own declaration,
                    # but an RVOCA-labelled handler (EXP_SWITCH_START, etc.)
                    # needs marking too -- exactly what the old standalone
                    # `WAX_CMD word handler` line's own `bd` add did.
                    _hdlr = slots.get('Name') or (args[0] if args else None)
                    if _hdlr:
                        self._nova_names.add(_hdlr)
                # Old pattern: NOVA <label> Op=Move El1=RA_MA0 ... Exit=RA_MC_LUX
                if (current_macro and cmd == 'NOVA' and len(args) >= 1
                        and slots.get('Op') == 'Move' and slots.get('El1') == 'RA_MA0'
                        and slots.get('Exit') == 'RA_MC_LUX'):
                    commands.add(current_macro)
                # New pattern: NOVAE <label> ... At=RA_MA0
                # NOVAE-converted macros use At=RA_MA0 to mark the naming arg
                if (current_macro and cmd == 'NOVAE' and len(args) >= 1
                        and slots.get('At') == 'RA_MA0'):
                    commands.add(current_macro)

        # Second source: the builtins implemented directly in the wiring
        # handler. They follow the identical convention -- first positional
        # argument becomes a named, self-referential NOVA -- but live in
        # Python, not in macros.re, so the scan above cannot see them.
        #
        # HASH_STEP is the case that exposed this: `HASH_STEP BS_RT_H33 ...`
        # names a NOVA from args[0] via _nova_lux, but Wave 1 never marked
        # BS_RT_H33 as NOVA-kind, so it was minted as a data lux and then
        # re-allocated when the builtin ran -- 58 times per build, with the
        # symbol repointed each time. That is harmless only because nothing
        # writes into a lux before the wiring pass; it would not be harmless
        # if declaration and wiring ever shared a pass.
        #
        # Discovered rather than listed, for the same reason the macros.re
        # scan is: _discover_known_commands already reads these handlers'
        # source to learn command names, so reading them for this too keeps
        # one mechanism instead of a list that silently goes stale.
        import inspect
        import re as _re
        src = inspect.getsource(Loader._wave2_wire_line)
        guards = list(_re.finditer(
            r'if cmd (?:==\s*"([A-Z_0-9]+)"|in \(([^)]*)\))', src))
        for i, g in enumerate(guards):
            end = guards[i + 1].start() if i + 1 < len(guards) else len(src)
            body = src[g.end():end]
            # Only when the *first* argument is what becomes the NOVA --
            # a builtin minting some internal lux of its own does not make
            # its caller's args[0] NOVA-kind.
            if not _re.search(r'_(?:nova_lux|emit_nova)\(\s*(?:name\b|args\[0\])',
                              body):
                continue
            if g.group(1):
                commands.add(g.group(1))
            else:
                commands.update(_re.findall(r'"([A-Z_0-9]+)"', g.group(2)))

        self._nova_naming_commands = commands
        return commands


    def _wave0_parse(self, read_seq: list) -> dict:
        """Wave 0: read each file once, strip comments, statically dispatch
        each line into a record, and save the result. The only wave that
        touches a file or a byte of raw text — everything after this reads
        the records it produced, never the source again.

        Returns {filepath: (lines, records)}. `lines` is kept only because
        Wave 1/2 still address file_data by (filepath, file_id, start_ptr,
        records) tuples built from it; the records themselves are Wave 0's
        actual, sole product.
        """
        return {fp: _read_and_parse(fp, self) for fp in read_seq}

    def _wave1_lcount(self, read_cache: dict, read_seq: list) -> None:
        """Wave 1 (lcount): the global sizing scan.

        Reads Wave 0's already-built records (never a file) and accumulates,
        across every file, everything an allocation needs to know about a
        name *before* it is minted: LINK lumina counts, which names need a
        full NOVA layout, SPAN sizes, and the OWN/SAVE/LOCK modifier flags.
        Six answers, one pass -- see _wave1_lcount_line.

        This is a true cross-file barrier: a name's final size is only known
        once every file has been scanned, so Wave 2 (allocation) must not
        begin until this returns for the whole file set.
        """
        # naming_commands feeds the generic Wave-1 sizing rule below
        # ("elif cmd in naming_commands" -> args[0] is a fresh NOVA, size it as
        # one). NEW/SET/LINK/LOAD are excluded -- NOT because they are special
        # "bootstrap primitives" (they are derivable, floor = aspects + NOVA),
        # but because their args[0] is NOT a fresh NOVA this command mints:
        # NEW -> data cell, SET/LINK -> an already-declared name, LOAD -> a
        # filename. `NEW RA_TMP` must not size RA_TMP as a 7-slot NOVA.
        #
        # This whole list-of-kinds is an artifact of Python PRE-ALLOCATING every
        # name in Wave-1, OUTSIDE the command's own handler -- so it has to
        # guess each name's kind ahead of the handler that would decide it. The
        # self-hosted Reca loader has no such list: each handler allocates its
        # own node when it runs (PS_CMD_NEW allocs a data lux, EXP_CMD_NOVA a
        # NOVA), so "kind" is decided at the mint, never guessed. This exclusion
        # dissolves once Python allocation moves into the handlers too
        # (one-pass in-handler build); the lumen-count prepass stays (both
        # sides need it), the kind list does not. Kept as a correct stopgap
        # until then. (Was implicit before: these lived in parser.re, unscanned.)
        naming_commands = self._discover_nova_naming_commands() - _NON_NAMING_PRIMS
        lumen_prepass: dict = {}
        nova_names: set = set()
        region_status: dict = {}   # {(name, region_str): bool}, this call only, in order
        span_sizes: dict = {}
        for fp in read_seq:
            _lines, records = read_cache[fp]
            for _idx, (_lineno, rec) in enumerate(records):
                link_pairs, nova_add, region_updates, span_updates, extra_pairs = self._wave1_lcount_line(rec, naming_commands, anon_key=(fp, _idx))
                for _lp in link_pairs:
                    lumen_prepass.setdefault(_lp[0], []).append(_lp[1])
                for _name, _rel_key, _val in extra_pairs:
                    lumen_prepass.setdefault(_name, []).append(_val)
                if nova_add:
                    nova_names.update(nova_add)
                for _name, _region, _val in region_updates:
                    if _region is None:
                        # Bare In=0: clear every region this name has
                        # picked up so far, within this call -- later
                        # updates for the same name (further down the
                        # file, or in a later file) still apply normally
                        # afterward, in the order they're seen.
                        for _k in [k for k in region_status if k[0] == _name]:
                            del region_status[_k]
                        continue
                    region_status[(_name, _region)] = _val
                for _su in span_updates:
                    span_sizes[_su[0]] = _su[1]
        for k, vs in lumen_prepass.items():
            self._lumen_prepass.setdefault(k, []).extend(vs)
        self._nova_names |= nova_names
        for (_name, _region), _val in region_status.items():
            if _val:
                self._name_regions.setdefault(_name, set()).add(_region)
            # False (an explicit numbered In{N}=0 clear): matches the
            # pre-existing own/save/lock fold behavior this generalizes --
            # never removes a region already recorded in self._name_regions
            # by an earlier, separate _wave1_lcount call (auto_discover's
            # seed pass vs. the main pass, say). Real project usage is one
            # single big scan; cross-call retraction is an edge case this
            # doesn't handle, same as it never did before.
        self._reserve_combo_blocks(span_sizes)

    def auto_discover(self, root_dir: str, exclude_seed: str = "aspects.re") -> None:
        """Discover all .re files and load them through the three waves.

        Wave 0 (all files): read, strip comments, parse each line to a record.
        Wave 1 (all files): global sizing scan — lumina counts, NOVA/SPAN
                            sizes, OWN/SAVE/LOCK flags. The cross-file barrier.
        Wave 2 (all files): allocate every name, then wire everything.

        No SCC/Tarjan. File order is irrelevant: Wave 1 sizes every name
        across all files before Wave 2 mints one, and lazy resolve turns any
        remaining forward reference into a correctly-sized stub.
        """

        seed_path = os.path.abspath(os.path.join(root_dir, exclude_seed))
        all_files = []
        for path in re_sources(root_dir):
            abspath = os.path.abspath(path)
            if abspath == seed_path or abspath in self._loaded_files:
                continue
            all_files.append(abspath)
        if not all_files:
            return

        read_seq = ([seed_path] if os.path.exists(seed_path) else []) + all_files
        read_cache = self._wave0_parse(read_seq)         # Wave 0: text -> records
        self._wave1_lcount(read_cache, read_seq)         # Wave 1: global sizing scan
        self._run_waves(read_cache, all_files)           # Wave 2: allocate + wire

    def _load_group(self, filepaths):
        """Load a list of files through the three waves (see auto_discover)."""
        canonical_files = []
        for filepath in sorted(filepaths):
            if not os.path.exists(filepath):
                continue
            if os.path.abspath(filepath) in self._loaded_files:
                continue
            canonical_files.append(filepath)
        read_cache = self._wave0_parse(canonical_files)     # Wave 0: text -> records
        self._wave1_lcount(read_cache, canonical_files)     # Wave 1: global sizing scan
        self._run_waves(read_cache, canonical_files)        # Wave 2: allocate + wire

    def load_file(self, filepath):
        if not os.path.exists(filepath):
            return False
        canonical = os.path.abspath(filepath)
        if canonical in self._loaded_files:
            return True
        self._load_group([filepath])
        return True

    def _wave1_lcount_line(self, rec, naming_commands, anon_key=None):
        """Wave 1 (lcount): everything the global prepass needs from one
        record, in one look — LINK pairs (for lumen sizing), names needing
        full NOVA layout, OWN/FREE status, and SAVE/UNSAVE status.

        This used to be three separate scans (_scan_file, _scan_nova_names,
        _scan_own_names), each independently re-reading and re-parsing
        every file. They answer different questions but read the same
        text, so one pass answers all three. Reca has always called this
        stage lcount; this gives Python the same name for the same job,
        not three unnamed scans buried in a loop.

        `anon_key` -- (filepath, index-in-that-file's-records-list) for
        this exact record, supplied by the caller (_wave1_lcount, which
        has both from its own enumerate loop). Only consumed for an
        anonymous NOVA's own extra keys, where there is no name to key
        them by; _wave2_alloc_line reads the same (filepath, idx) pair
        back for the matching prepass_key. Not id(rec) -- a Python object
        address is this loader's own implementation accident, meaningless
        to a future self-hosted wax.re Wave 1, whereas "which position in
        this file" is a plain integer either side can compute the same
        way (see _wave2_alloc_line's own note for why this also survives
        Wave 2 wiring a smaller file set than Wave 1 scanned).

        Returns (link_pair_or_None, nova_names_to_add, region_updates,
        span_size_update_or_None, extra_pairs). region_updates is
        [(name, region_str_or_None, bool), ...] -- any region string, no
        fixed vocabulary (see _name_regions); region_str=None + bool=False
        is the bare `In=0` "clear everything seen so far for this name"
        sentinel, handled specially by _wave1_lcount's own fold. extra_pairs
        is [(key, rel_key, value), ...] for NOVA/NOVAE's own arbitrary
        [Name][N]= extra-lumen relations. `key` is the name for a named
        NOVA/NOVAE line, or anon_key for an anonymous NOVA line -- see the
        'elif cmd == NOVA and not args' branch below.
        """
        if rec is None:
            return [], (), (), [], ()
        cmd, args, _slots = rec

        # A registered handler contributes what only IT knows -- the folded
        # facts of a body it expands (FOR/DEF/SAVE/...), or the nova_add of a
        # construct it builds directly (SWITCH/MATCH/PACK/ALLOC_TO). It is NOT
        # the whole answer: everything below is universal cross-cutting logic
        # that runs for EVERY command and simply finds nothing to do when the
        # keys it looks for are absent. So a handler never opts in to In=/span,
        # and never has to call it -- the general logic wraps around all
        # commands by default, and the handler's own facts merge with it.
        _h_lp, _h_na, _h_ru, _h_su, _h_ep = ([], (), [], [], ())
        if cmd in _COMMANDS:
            _h_lp, _h_na, _h_ru, _h_su, _h_ep = _COMMANDS[cmd].size(
                self, rec, naming_commands, anon_key)

        # link_pairs and span_updates are LISTS: one record normally yields at
        # most one of each, but a record that expands into many (FOR) yields
        # the union of its body's -- so the caller loops both. Seeded from the
        # handler (LINK's own pair now comes from _CmdLINK.size).
        link_pairs = list(_h_lp)

        extra_pairs = list(_h_ep)
        nova_add = _h_na or ()   # a handler declares its own minted names
        # (NOVA/NOVAE size their own label + extra-lumen keys via their
        # _COMMANDS handlers -> _CmdNOVA.size / _CmdNOVAE.size, folded in as
        # _h_na/_h_ep above. The generic naming-command branch below stays
        # for discovered runtime macros that have no Python handler.)
        if cmd not in _COMMANDS and cmd in naming_commands and len(args) >= 1:
            name = args[0]
            # Extra operands may be positional (args[1:], commands not yet
            # migrated to Key=Value -- e.g. HASH_STEP) or keyed (_slots --
            # e.g. RVOCA/JEQ/JZ/CLEAR's Body=/El1=/El2=/Exit=). Check both:
            # a templated operand either way means this is a FOR-macro body
            # line, not a real name to register yet.
            templated = ('{' in name
                         or any('{' in p for p in args[1:])
                         or any('{' in str(v) for v in _slots.values()))
            if not templated:
                nova_add = (name,)

        region_updates = list(_h_ru)   # seeded from the handler's body facts
        # (NEW Count=/Width= span sizing now comes from _CmdNEW.size, folded in
        # via _h_su below -- it is NEW's own fact, not a cross-cutting one.)
        if cmd in _alloc_line_own_cmds() and len(args) >= 1:
            # `NEW X In1=OWN` / `X In1=SAVE In2=LOCK` (X declared via NEW,
            # SPAN, or SET): the region(s) a name is placed in, named
            # rather than flagged, and not restricted to any fixed
            # vocabulary -- "OWN"/"SAVE"/"LOCK" are just the strings real
            # declarations use today (_name_regions/_combo_key work for
            # any string; a region having a further RUNTIME consequence,
            # like LOCK's write guard, is a separate thing some other
            # piece of code specifically recognizes, not something this
            # parsing loop gatekeeps). OWN only really makes sense for NEW
            # (Pattern W is about per-thread registers, not spans or
            # values), but SAVE and LOCK apply to either -- SK_SEAL_NAMEPOOL
            # is SPAN'd, and LOCK's whole point (established directly with
            # the author) is that the *value-assigning* command (SET) is
            # where content actually becomes real, so that's where "and
            # never let this change again" naturally belongs -- not on
            # NEW, which only ever gives a blank, zeroed lux (see
            # _define's own note).
            #
            # Widened from a hand-picked (NEW, WAX_NEW, SET, WAX_SET,
            # WAX_CMD) tuple to _alloc_line_own_cmds() -- the same
            # source-discovered set already used by the Wave 2 gate this
            # mirrors. It also covers NOVA/NOVAE/RREDI/RVOCA, none of which
            # currently write an In= tail (verified against every .re
            # source), so this changes no behavior for them today.
            #
            # Numbered (In1=/In2=/...): a name can belong to more than one
            # region at once (OWN+SAVE is real; a plain dict-shaped `In=`
            # can only hold one value, so a second In= on the same line
            # would silently overwrite the first). Every _slots key whose
            # base is `In` (bare, or followed only by digits) is its own
            # independent region assignment -- same [Name][N]= convention
            # used for extra-lumen relations elsewhere, but these never
            # become lumens themselves: for a Data lux (the only thing
            # that reaches this branch), region membership is expressed by
            # WHERE the lux physically lives (which combo-block
            # _alloc_sized routed it to), not by anything written into its
            # own body. (NOVA doesn't get this for free -- it isn't
            # combo-routed at all -- so the same In[N]= on a NOVA/NOVAE
            # line is handled separately, as a real extra-lumen write.)
            #
            # `In{N}=0` on a NUMBERED slot clears only whatever region
            # that exact slot last set for this name (_region_slot_role
            # remembers it, so this line doesn't need to know what any
            # OTHER line did) -- not every region the name carries. Bare
            # `In=0` clears everything, unconditionally, regardless of
            # what bare In= itself may have set -- the two are
            # deliberately asymmetric: by convention, bare In= is reserved
            # for the "clear all" sentinel, never a real per-region
            # assignment worth being able to undo individually on its own
            # (see CONVENTIONS.md) -- own/save/lock below track only
            # whether THIS line touched those three specific, well-known
            # strings, purely to gate the K_/RA_ auto-inference right
            # after; they say nothing about what CAN be stored.
            own = None
            save = None
            lock = None
            name0 = args[0]
            for _key, region in _slots.items():
                if _key != 'In' and not (_key.startswith('In') and _key[2:].isdigit()):
                    continue
                slot_num = None if _key == 'In' else int(_key[2:])
                if region == '0':
                    if slot_num is None:
                        region_updates.append((name0, None, False))
                        self._region_slot_role.pop(name0, None)
                        own = save = lock = False
                    else:
                        prev = self._region_slot_role.get(name0, {}).pop(slot_num, None)
                        if prev is not None:
                            region_updates.append((name0, prev, False))
                            if prev == 'own':   own = False
                            elif prev == 'save': save = False
                            elif prev == 'lock': lock = False
                else:
                    region_l = region.lower()
                    region_updates.append((name0, region_l, True))
                    if slot_num is not None:
                        self._region_slot_role.setdefault(name0, {})[slot_num] = region_l
                    if region_l == 'own':    own = True
                    elif region_l == 'save': save = True
                    elif region_l == 'lock': lock = True
            if cmd == 'NEW' and name0.startswith('K_'):
                # K_-prefixed names are the project's own convention for
                # stable-pointer registers (K_CURSOR, K_WATERMARK,
                # K_TRACE_POS, K_STACK_TOP, K_FLUX_CURSOR, K_FLUX_FREELIST,
                # K_LIMITER_ADOPTED -- most double-deref: word=addr+1, real
                # value at addr+1). SAVE and LOCK both default on here --
                # "K_" names foundational, project-wide bookkeeping state by
                # convention; a name using this prefix is declaring itself
                # exactly that, the same way RA_ declares per-thread scratch.
                #
                # SAVE is harmless even where not strictly needed (a couple
                # of spare luces). LOCK is not harmless where wrong --
                # legitimate ongoing writes need Key (Op1=Move for the
                # non-double-deref shape) instead of Write, or they raise
                # LockViolation. That is not a reason
                # to leave K_ unlocked by default, though: the failure is
                # loud and immediate (a raised exception naming the exact
                # address, at the exact write that needs authorizing), not
                # silent corruption -- exactly the guardrail LOCK exists
                # to provide, and exactly why K_CURSOR/K_WATERMARK/
                # K_TRACE_POS/K_FLUX_CURSOR/K_FLUX_FREELIST's own updaters
                # (alloc.re/iris.re/yaku.re/arena.re) already had to be
                # written as Key, not Write. `In=0`/numbered `In{N}=0`
                # still override either one, per the rules above.
                if save is None:
                    save = True
                    region_updates.append((name0, 'save', True))
                if lock is None:
                    lock = True
                    region_updates.append((name0, 'lock', True))
            if own is None and cmd == 'NEW' and name0.startswith('RA_'):
                # RA_-prefixed names are the project's own convention for
                # ordinary scratch registers (Pattern W, BUGS.md). Measured
                # before adding this rule: every single RA_ name declared
                # via NEW already carried OWN by hand -- 239 out of 239,
                # zero exceptions -- so this makes explicit what the
                # project already does with perfect consistency, rather
                # than guessing at a pattern. Deliberately narrow to NEW
                # only, not SPAN/SEAL: the 10 RA_ names declared those other
                # ways are buffers (RA_FOR_IRIS_BUF, a 2048-luce text pool)
                # or aliases (RA_SSA_FR, `SEAL RA_SSA_FR RA_SSA_FR_DATA`),
                # where OWN's per-thread locality doesn't apply the same
                # way a single scalar register's does. `In=0` still works
                # for any RA_ name that genuinely needs to opt out.
                own = True
                region_updates.append((name0, 'own', True))

        span_updates = list(_h_su)   # NEW's span comes from _CmdNEW.size (_h_su)
        return (link_pairs, nova_add, region_updates, span_updates, extra_pairs)

    def _minted_names(self, rec, naming_commands):
        """The names a record declares (mints), as a list. A registered command
        reports them itself via size() (the handler is the authority -- covers
        derived names like _J/_K/case-names, and correctly empty for
        FOR/SWITCH/MATCH whose args[0] is a referenced register, not a def). An
        inline name-minting command mints its first argument. Everything else
        mints nothing. One source of truth, shared by --deps defs discovery and
        block-scope pre-scan."""
        cmd, parts, slots = rec
        if cmd in _COMMANDS:
            return list(_COMMANDS[cmd].size(self, rec, naming_commands, None)[1])
        # NEW/SET/NOVA/NOVAE now mint through their _COMMANDS handlers above;
        # this fallback covers only the discovered runtime naming macros that
        # have no Python handler. (ALIAS was retired as a pure subset of SET --
        # zero corpus uses -- so it is not in this set.)
        if parts and cmd in naming_commands:
            return [parts[0]]
        return []

    def _block_local_names(self, records, entry_name, naming_commands):
        """The fixed set of names a block body binds LOCALLY: every name its
        records mint, minus the block's own entry name (declared in the
        enclosing scope -> public) and minus any Vis=Public name (which escapes
        to the global table). Computed once from the fully-expanded body before
        the block is built, so every reference resolves local-vs-global by
        knowing, not guessing (see _scope_of)."""
        local = set()
        for _ln, rec in records:
            if not rec:
                continue
            _cmd, _parts, slots = rec
            if slots.get('Vis') == 'Public':
                continue
            local.update(self._minted_names(rec, naming_commands))
        local.discard(entry_name)
        return local

    def _scan_defs_refs(self, records, naming_commands):
        """Names a file declares (defs) and names it mentions (refs).

        Not a wave — a standalone diagnostic scan for iris.py's `--deps`
        mode (file dependency graph / SCC cycle detection), the one real
        consumer that needs "which file owns this name" and "what does this
        file depend on", as opposed to the sizing/allocation questions
        _wave1_lcount_line answers.

        `defs`: for a registered command, the names its own size() reports it
        mints (the handler is the authority -- so `SWITCH`/`FOR`/`MATCH`, whose
        args[0] is a referenced register rather than a minted name, contribute
        no false def, while `PACK`/`ALLOC_TO`/... contribute every derived name
        they build). For an inline command, the first argument of any name-
        minting command (the bootstrap set plus the getsource/.re-discovered
        naming commands).
        `refs`: every other name-shaped token on the line, including
        slot values (El1=/Exit=/...), excluding the NOVA/NOVAE opcode
        token (Move/Add/Read/... at args[1], not a name) and numeric
        literals.
        """
        defs, refs = set(), set()
        for _lineno, rec in records:
            if not rec:
                continue
            cmd, parts, slots = rec
            minted = self._minted_names(rec, naming_commands)
            defs.update(minted)
            rest = [t for t in parts if t not in minted]
            for tok in rest:
                if (tok and (tok[0].isupper() or tok[0] == '_')
                        and tok.replace('_', '').isalnum() and not tok.isdigit()):
                    refs.add(tok)
            for k, v in slots.items():
                if k in ('Name', 'Op') or k.startswith('__') or not isinstance(v, str):
                    # Name/Op: the def itself / the opcode word, not a name ref.
                    # __-prefixed keys (__body__/__slots__/__tag__/__ln__/...):
                    # a block command's internal carrier, holding token-lists,
                    # records, ints -- structure, never a name reference.
                    continue
                name = v.split(':')[-1]  # Idem:X -> X
                if (name and (name[0].isupper() or name[0] == '_')
                        and name.replace('_', '').isalnum()):
                    refs.add(name)
        return defs, refs

    # ── File scanning ─────────────────────────────────────────────────────────

    def _wave2_build_line(self, rec, filepath, lineno, idx):
        """Wave 2's single step: declare this line, then wire it.

        With the NOVA-naming discovery gap closed (no symbol is repointed
        mid-build), a reference resolved inside a handler's body lands on a
        stable stub address that the real declaration later fills in place.
        So declaration and wiring can share one pass over the source.

        `idx` is this record's position within its own file's records list
        (from _run_pass_over's enumerate) -- only _wave2_alloc_line needs
        it (an anonymous NOVA's prepass key, see its own note), wire_line
        has no current use for it."""
        self._wave2_alloc_line(rec, filepath, lineno, idx)
        self._wave2_wire_line(rec, filepath, lineno)

    def _wave2_alloc_line(self, rec, filepath, lineno, idx):
        """Wave 1: allocate. Everything that mints a lux happens here.

        This used to be two waves. The split existed as a barrier: the second
        half allocated only names the first half had not, and that "had not"
        was only trustworthy once the first half had swept every file.

        The barrier is gone: _resolve now allocates a correctly-sized stub on
        a miss (see its own note), so a name seen before its declaration is an
        ordinary forward reference rather than an error to be prevented. Reca
        never needed the split either — BS_INTERN_NOVA has always worked this
        way. The scanner that used to enforce the barrier
        (_scan_deferred_names) is deleted.

        The one real data dependency — `SPAN X SOME_CONST`, where the size is
        a name — is satisfied by source order: the project's only such line
        (`SPAN OB_START OB_LUCES`) sits two lines below its `SET OB_LUCES 32`.

        Takes an already-parsed record (cmd, args, slots) from Wave 0,
        not raw text — Wave 0 already did the tokenising (quote-aware,
        see _tokenize_quoted) and the command-recognition dispatch.
        `args` does not include the command itself (unlike the old
        `parts`, where `parts[0]` was the command) — every `parts[N]`
        below becomes `args[N-1]`.
        """
        if rec is None:
            return
        cmd, args, slots = rec
        a = self.interp.aether.aether

        # (Vis=Public is resolved by the block handler's pre-scan, which leaves
        # such names out of the scope's local set so they bind globally -- see
        # _scope_enter / _block_local_names. Nothing to read per-record here.)

        # Syn=X registers command X against this line's own name as handler
        # (replacing a standalone `WAX_CMD X <name>` line). Allocation half:
        # declare X, exactly what WAX_CMD's own alloc branch did for its
        # command word. The word-write (X.word = addr(name)) is the wiring
        # half -- see _wave2_wire_line. See _discover_nova_naming_commands.
        _syn = slots.get("Cmd")
        if _syn:
            self._declare(_syn)

        # By-command dispatch (after the cross-cutting Cmd= declare above): a
        # migrated command allocates through its _COMMANDS handler; the rest
        # fall through to the branches below (migrated batch by batch).
        if cmd in _COMMANDS:
            _COMMANDS[cmd].declare(self, rec, filepath, lineno, idx)
            # No early return: a handler that has no allocation of its own
            # (declare not overridden) still falls through to the universal
            # macro-arg pre-alloc below, exactly like any other macro. One that
            # DID override declare is in _alloc_line_own_cmds, so that same
            # generic block skips it. The NEW/RVOCA branches between here and
            # there never match a handler command.

        # (SWITCH/MATCH/PACK_BYTE/UNPACK_BYTE and the FOR/DEF/SAVE/WRITE_OUT/
        # READ_BODY/ALLOC_RAW block/compound commands allocate through their
        # _COMMANDS handlers above; ALLOC_TO/CLEAR/... use the generic alloc.)

        # SEAL removed -- it was a pure subset of SET (`SEAL X Y` wrote
        # word(X)=addr(Y), and `SEAL X` alone wrote word(X)=addr(X); SET's
        # own bare/`Value=`/`Of=` forms already cover both, the bare form's
        # self-ref default covering SEAL's one-argument shorthand exactly).
        # Verified zero real SEAL call sites remained in the project before
        # removal -- only its own test, migrated to SET Value=.

        # ALIAS, OWN and FREE removed. ALIAS was a second name for a
        # cell, which SET now says directly; OWN and FREE named a placement
        # that NEW's In= names better, and being a pair of words for one
        # question they also needed a negation each. All three had zero
        # uses left in the project.

        # (RVOCA/RREDI pre-allocate their named NOVA luces through their own
        # _COMMANDS handler above -- _CmdVocaRedi.declare.)

        # Macro call (this function has no dedicated code for it further
        # down): pre-allocate its identifier arguments as luces. Macros
        # like JEQ write full 7-lux NOVA into MA0, MA4, MA5 (all lux
        # names); pre-allocating avoids lux overlap. NOVA/NOVAE/NEW/SEAL/
        # RVOCA/RREDI/SET/WAX_* are excluded automatically here (see
        # _alloc_line_own_cmds) because they have their own handling,
        # not via a maintained list.
        #
        # This used to be two separate checks of the same condition, split
        # only by whether there were arguments to allocate. One check now;
        # "has arguments" is a guard inside it, not a second repetition of
        # "is this a macro".
        if cmd not in _alloc_line_own_cmds():
            if len(args) >= 1 and not (cmd.startswith('/') or cmd.startswith('"')
                    or cmd.startswith("'") or not cmd[0].isalpha()):
                for name in args:
                    if (name and not _is_integer(name) and '"' not in name
                            and not name.startswith('/')
                            and (name[0].isupper() or name[0] == '_')
                            and name.replace('_','').isalnum()
                            and not self._has(name)
                            ):
                        # Size it the way the global scan says, not always
                        # NOVA. Forcing NOVA here oversizes every plain
                        # register that happens to appear as a macro
                        # argument -- measured: 80 OWN-marked ones (RA_A1,
                        # RA_BYTE, ...), each reserved at 2 luces and handed
                        # 7, which overflows the OWN block.
                        addr = (self._alloc_nova(name) if name in self._nova_names
                                else self._alloc_data(name))
                        self._bind(name, addr)
                        self.interp.aether.aether[addr + _WORD] = addr
            return


        # (NEW Count=/Width= allocates its pointer+region block through its
        # own _COMMANDS handler above -- _CmdNEW.declare; plain NEW declares
        # its blank name there too.)

        # Primitive NOVA: allocate an instruction lux. Op is now always a
        # slot (Op=...), never positional, so args holds at most the name --
        # empty means anonymous, one element means named. No more len(args)
        # dance to tell "op-only" from "nothing at all" apart.
        # (NOVA allocates its instruction lux via its _COMMANDS handler ->
        # _CmdNOVA.declare.)

        # Non-bootstrap commands are Reca macros — no pass1 allocation needed.

    # ── Pass 1b: allocate instruction Lux ─────────────────────────────

        # NOVAE: pre-allocate the label and all generated suffix nodes as NOVA luces.
        # This mirrors what NOVA does in wave1a so that wave2 can wire them.
        # At=/Next= are already split into slots by Wave 0 -- read them
        # there, not by re-scanning args for a string prefix.
        # (NOVAE allocates via its _COMMANDS handler -> _novae_declare.)

        # Reaching here, cmd is a bootstrap primitive not yet handled above
        # (ALIAS/OWN/FREE/RVOCA/RREDI/the macro path/SPAN/NOVA/NOVAE all
        # returned earlier for their own commands): NEW or SET.

        # NEWSET was confirmed a pure, literal duplicate of SET (2026-07-09,
        # prompted by the author directly questioning why both existed) --
        # both allocated the name if missing and set its integer value
        # identically in both allocation and wiring; the "SEAL absorbed NEWREF
        # and SETREF" comment below already anticipated finishing this
        # exact cleanup for SET/NEWSET too. All 201 real "NEWSET name
        # value" lines project-wide were mechanically renamed to "SET name
        # value" (identical syntax, zero behavior change) and this
        # handler removed. See BUGS.md Pattern Z.
        # (SET declares its blank name through its own _COMMANDS handler above
        # -- _CmdSET.declare, sharing NEW's _declare; the value is written in
        # the wire wave -- _CmdSET.wire.)



    def _setup_load_aspects(self) -> None:
        """Register the 1 load-time native op: __LT_ALLOC_NOVA.

        __LT_ALLOC_NOVA is used as the op-code of luces that need to
        bump-allocate a NOVA block at load time. It is a true native op —
        Reca code sets a lux's op field to addr(__LT_ALLOC_NOVA) and the
        interpreter calls this Python handler.

        ALLOC is a Reca program (not a native op) — it runs normally
        through the interpreter. No registration needed.
        """
        sym      = self.symbols
        dispatch = self.interp._dispatch
        loader_ref = self

        lt_alloc_addr = sym.get("__LT_ALLOC_NOVA", 0)
        ra_ma_ret     = sym.get("RA_MA_RET", 0)
        ra_extra_cnt  = sym.get("RA_MC_EXTRA_COUNT", 0)
        if lt_alloc_addr:
            def _lt_alloc_nova(a1, a2, exit, nxt, aether):
                # RA_MC_EXTRA_COUNT, when a NOVAE line's own emitted Move set
                # it right before this call, means this particular
                # synthesized lux needs room for that many extra (rel,val)
                # pairs beyond the fixed 7 slots -- same NOVA_SIZE+2*n
                # formula _alloc_sized uses at load time (see synth.re's
                # matching declaration). Cleared unconditionally so the
                # next call (almost always a plain, extra-free NOVAE) never
                # inherits a stale count.
                extra = aether[ra_extra_cnt] if ra_extra_cnt else 0
                if extra:
                    aether[ra_extra_cnt] = 0
                addr = loader_ref._alloc(_NOVA_SIZE + 2 * extra)
                aether[addr + _WORD] = addr
                if ra_ma_ret: aether[ra_ma_ret] = addr
                return nxt
            dispatch[lt_alloc_addr] = _lt_alloc_nova

    def _resolve_count_width(self, token, filepath, lineno):
        """Resolve a NEW ... Count=/Width= value: an int literal, or a name
        already SET to one. Named after SPAN in an earlier version of this
        code, back when SPAN (not NEW's Count=/Width=) carried this fact --
        SPAN is gone from the language now (absorbed into NEW, see the
        Count/Width handler above), this helper just never got renamed."""
        try: return int(token)
        except ValueError: pass
        addr = self.symbols.get(token)
        if addr is None:
            raise LoadError(f"NEW Count/Width: '{token}' not defined")
        count = self.interp.aether.aether[addr + _WORD]
        if count <= 0:
            raise LoadError(f"NEW Count/Width: count must be > 0, got {count}")
        return count

    # ── Pass 2: wire everything ───────────────────────────────────────────────

    def _own_addr_if_declared(self, name):
        """Look up NAME's own address if already allocated — NOT a reference
        resolve. Used only when wiring a declaration to check whether *this
        exact name* was already allocated (e.g. by SPAN pre-allocation,
        or a duplicate encounter of the same lux), never to resolve a target
        this declaration points AT.

        Deliberately does not stub and does not go through `_resolve`: a
        miss here means "allocate this declaration now", not "someone
        referenced me before I existed". Conflating the two was exactly the
        shape of the address-0 bug class fixed project-wide (see
        docs/DECISIONS.md, "Wave 2 is one pass") — every OTHER `self.symbols` lookup in this
        class must go through `_resolve`, not this method. There are
        deliberately only two call sites: RVOCA/RREDI's own lux (below) and
        `_nova_lux`'s own lux. If you are about to add a third raw
        `self.symbols.get(...)` anywhere in Wave 2 wiring, stop — it almost
        certainly should be `_resolve`, not this.

        Scope-aware: checks the open scopes then the global table (via
        _lookup), so a local declaration's own already-allocated address is
        found while a scope is open, exactly as a global's is at top level.
        """
        return self._lookup(name)

    def _resolve(self, name) -> int:
        """Resolve a name or integer literal to an address.

        A bare numeric literal (e.g. "0", "47") must resolve to the ADDRESS
        of a register holding that value, never the literal value itself —
        every NOVA field is read by the interpreter through a dereference
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
        expansion in _parse_indent_blocks).

        Reuses the project's existing "C_<value>" named constant where one
        is already defined (the established convention, e.g. C_0, C_1,
        C_33); otherwise lazily allocates — and memoizes — a dedicated
        1-lux literal-constant register, so repeated uses of the same
        literal share one address rather than each minting a new lux.
        """
        a = self._lookup(name)
        if a is not None:
            return a
        try:
            v = int(name)
        except ValueError:
            # Lazy resolve: a name seen before its declaration gets a stub now,
            # at a correctly-sized NOVA lux, and the real declaration fills that
            # same address in later. The address never moves, so whatever
            # recorded it (a SEAL, an operand slot) stays correct.
            #
            # This mirrors Reca's BS_INTERN_NOVA, which has always worked this
            # way -- see its header: "a reference to the name, read before its
            # real `NOVA name op ...` declaration is reached (e.g. from another
            # file, given LOAD_MAIN's interleaved scheduling), doesn't get
            # handed a too-small 1-lux stub". NOVA_SIZE, not 1, for exactly that
            # reason: a later write past a 1-lux stub's bounds would clobber
            # whatever followed it.
            #
            # Raising here used to be the rule, and everything that sweeps the
            # project up front, or split allocation across waves, existed to
            # guarantee no miss ever happened. With this, a miss is ordinary,
            # and both of those are gone.
            if not self._lazy_ok:
                # Bootstrap builds the base luces itself and asks about names
                # that genuinely do not exist yet. A miss there is normal and
                # must stay fatal: stubbing it would allocate a label among the
                # bootstrap constants (measured: ILS_RET landed at 842, ahead
                # of C_0 at 2915) and shift the whole layout out from under
                # everything that follows.
                raise LoadError(f"Unknown name: '{name}'")
            if not name or not (name[0].isalpha() or name[0] == '_'):
                raise LoadError(f"Unknown name: '{name}'")
            # Size the stub the way ordinary allocation would: _nova_names is
            # the global scan's answer to "will this name end up a NOVA?", and
            # _alloc_data consults it for exactly this reason. Stubbing
            # everything as NOVA looks harmless but is not -- it oversizes every
            # plain register (RA_LOAD_BYTE and friends) by 3.5x, and for the
            # ~280 that are also OWN it overflows the OWN block, which was
            # reserved before this point using the same _nova_names answer.
            # Record the name as a lazily-created stub. Lazy resolve is a
            # capability, not a fallback: a name may legitimately be
            # referenced before it is declared, or built at runtime. But a
            # stub nothing ever fills in is a missing declaration wearing
            # the same shape as a forward reference, and without this record
            # the two are indistinguishable after the build. See iris's
            # `stubs` command, which reports the ones left empty.
            self._lazy_stubs.add(name)
            if name in self._nova_names:
                return self._define_nova(name)
            return self._define_data(name)
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

    def _set_offset(self, args: list, slots: dict) -> int:
        """How far into the named cell does this SET write? Default: 0.

        SET used to reach exactly one word -- the cell's own. That is why
        every structure built out of cells needed its own command to build
        it: NOVA to fill a five-slot lux, SPAN to reserve a run, SEAL and
        ALIAS to put a name into a particular word of another. Ten commands
        in the loader's bootstrap, ten handlers on each side of the project,
        all of them ultimately "put this value at this distance from that
        name".

        With an offset there is nothing left for them to be: a lux is five
        cells and five SETs, and the kernel never has to know what a lux is.
        Offsets are named already (SLOT_OP, SLOT_E1, SLOT_EXIT and friends),
        so the expansion reads as words rather than numbers.

        Written as `At=` to match the key=value form the rest of the loader
        already uses, and resolved through the ordinary name resolution, so
        `At=SLOT_EXIT` and `At=4` mean the same thing.
        """
        raw = slots.get("At")
        if raw is None:
            for tok in args[2:]:
                if tok.startswith("At="):
                    raw = tok.split("=", 1)[1]
                    break
        if raw is None:
            return 0
        try:
            return int(raw)
        except ValueError:
            return int(self.interp.aether.aether[self._resolve(raw) + _WORD])

    def _unescape_string(self, text: str) -> str:
        """Single left-to-right pass, mirroring wax.re's LOAD_PACK_STRING
        exactly (byte-by-byte, one 'escaped' flag) rather than Python's
        former sequential .replace() chain, which had a real bug: chained
        global replaces don't respect escape precedence, so a genuinely
        escaped backslash immediately followed by a literal 'n' or 't'
        (source: backslash, backslash, n) got wrongly read as an
        escape sequence by the *next* .replace() call, silently turning a
        literal backslash+n into a newline. Confirmed with a real example
        before this fix: "a\\nb" (escaped backslash then literal n) came
        out as "a" + LF + "b" instead of "a" + backslash + "n" + "b". Only
        \\n, \\t, \\\\ are recognised escapes here, matching LOAD_PACK_STRING;
        any other backslash-something is emitted literally, backslash
        included (same "Other: emit literally" branch as the Reca side).
        """
        out = []
        escaped = False
        for ch in text:
            if escaped:
                if ch == 'n':
                    out.append('\n')
                elif ch == 't':
                    out.append('\t')
                else:
                    out.append(ch)
                escaped = False
            elif ch == '\\':
                escaped = True
            else:
                out.append(ch)
        if escaped:
            # Trailing lone backslash (no char followed) -- emit it as-is
            # rather than silently dropping it.
            out.append('\\')
        return ''.join(out)

    def _wire_word_value(self, addr: int, args: list) -> None:
        """Write word(addr) from a value token — shared by SET and NEWSET Wave 2 handlers.

        Supports:
          "string literal"  → packed string first-chunk addr
          123               → integer literal
          SYMBOL_NAME       → word(symbol)  (symbolic copy)

        `args[1]` is the value token (args[0] is the name being written to,
        matching SET NAME VALUE). Wave 0's tokenizer already keeps a quoted
        span as one token including both quote marks, so no cmd_part
        re-scan is needed here -- args[1] itself is the whole quoted text
        when it starts with a quote.
        """
        val = args[1] if len(args) >= 2 else ''
        # Every branch below computes a value some different way (read
        # another cell's contents, pack a string, parse a literal, resolve
        # a bare name to its address) and ends the same way: one _write
        # call. The write is always the same operation; only how the value
        # gets computed varies.
        if val == "Of":
            # Of=Y -- the contents of Y, the reading SET used to give a bare
            # name. Now it has to say so, because bare means "where".
            self._write(addr, self.interp.aether.aether[self._resolve(args[2])])
            return
        if val.startswith('"'):
            text = val[1:-1] if val.endswith('"') and len(val) >= 2 else val[1:]
            text = self._unescape_string(text)
            self._write(addr, self._pack_string(text))
        else:
            try:
                self._write(addr, int(val) & _MASK64)
            except ValueError:
                # A bare name means WHERE it is -- its address.
                #
                # It has to mean "where" eventually -- that is the whole
                # reason SEAL exists, and every other position in the
                # language already reads a bare name that way. SET was the
                # single exception, and that exception is exactly why SEAL
                # existed: with no way to say "where" in a value position, a
                # whole command was spent saying it.
                #
                # Migration cost: nothing. Instrumenting this branch across a
                # full build found zero uses of the old contents reading --
                # measured through expansion, not by reading source files,
                # since SET records are also produced by macros. Of= keeps
                # the contents reading available for whoever needs it later.
                self._write(addr, self._resolve(val))

    def _write(self, addr: int, value: int) -> None:
        """The one minimal write primitive: write an already-resolved word
        to an address. Nothing else -- no name lookup, no type check, no
        branching. Resolving a name to a word (`_resolve`) is a separate,
        earlier step the caller does first; by the time a value reaches
        here it is just a word, which is exactly what lets this write
        anything at all uniformly instead of needing to know what kind of
        thing it was given.

        (There is no companion "declare-or-reuse" wrapper here
        deliberately -- the one real caller that used to go through one
        already does its own "already exists?" check before ever reaching
        an allocation, which made that wrapper's own internal check dead
        code at its only call site. "Check, then _alloc, then
        register" is just NEW's own logic, spelled out where NEW's logic
        already lives -- not a separate primitive to maintain.)
        """
        self.interp.aether.aether[addr] = value

    def _emit_nova(self, name: str, op: str, *, e1=None, e2=None,
                    exit=None, auto_next=True, addr_override: int = 0) -> int:
        """Wire a named NOVA lux with compact layout.
        Layout: [word=self, op, e1, e2, exit, next=0, ...extra_lumen..., 0]
        Values written to fixed slots — no rel slots. Next is at SLOT_NEXT (5).
        Returns the addr of this instruction.

        addr_override: for an anonymous NOVA (name=None) -- there is no
        name to _resolve, the caller (_wave2_wire_line) already has the
        real address from _wave2_alloc_line's own allocation.

        Four _write calls, one per fixed slot -- NOVA itself has no
        allocation logic of its own here (that already happened in
        Wave 1b, via _alloc) and no writing logic of its own either,
        just the four addresses to write to.
        """
        addr = addr_override if addr_override else self._resolve(name)
        self._write(addr + _OP,   self._resolve(op))
        self._write(addr + _E1,   self._resolve(e1)   if e1   is not None else 0)
        self._write(addr + _E2,   self._resolve(e2)   if e2   is not None else 0)
        self._write(addr + _EXIT, self._resolve(exit) if exit is not None else 0)
        # SLOT_NEXT (5) stays 0 for adjacent luces (fall-through). _link_novas writes
        # only for non-adjacent explicit links.
        # No _lumen_count seeding needed here: _add_lumen picks its own base
        # offset (pad slot vs DATA_LUMEN_OFFSET) from _nova_addrs_set
        # membership directly, not from a pre-seeded count -- see its own
        # docstring for why that's simpler than the seeding this replaced.
        if auto_next:
            self._chain_nova(addr)
        else:
            self._last_nova = addr
        return addr

    def _nova_lux(self, name, op_name, e1=None, e2=None, exit=None):
        """Alloc-or-find a NOVA lux, wire its self-ref/pad, emit its op,
        autolink. The one shared node-minting routine every macro-command
        build references (JEQ/JZ/CLEAR/NOP/HASH_STEP/ALLOC_TO, and via
        _build_jeq, SWITCH). Promoted from a _wave2_wire_line-local closure so
        callers outside that method reference it instead of re-implementing
        node minting."""
        a = self.interp.aether.aether
        addr = self._own_addr_if_declared(name)
        if addr is None or addr not in self._nova_addrs_set:
            addr = self._alloc_nova(name)
            self._bind(name, addr)
        a[addr + _WORD] = addr
        a[addr + _NEXT] = 0            # in case this address is reused
        a[addr + _NOVA_SIZE - 1] = 0    # pad
        return self._emit_nova(None, op_name, e1=e1, e2=e2, exit=exit,
                                addr_override=addr)

    def _build_jeq(self, name, e1, e2, dest):
        """Build one JEQ pair: Equal(e1,e2)->RA_JEQ_FLAG ; JumpIf(FLAG)->dest.
        The single JEQ-node truth. JEQ, JZ (e2='C_0') and SWITCH cases all
        reference this -- none re-implement it. Leaves _last_nova on the
        JumpIf (its own next slot is the fall-through point for autolink).
        Returns the JumpIf addr."""
        self._nova_lux(name, "Equal", e1=e1, e2=e2, exit="RA_JEQ_FLAG")
        ji = self._nova_lux(name + "_J", "JumpIf", e1="RA_JEQ_FLAG", exit=dest)
        self._last_nova = ji
        return ji

    def _wave2_wire_line(self, rec, filepath, lineno):
        """Wave 2: wire operands, lumina, Next pointers.

        Takes an already-parsed record from Wave 0 — see Wave 1's
        docstring. args does not include the command; every old
        parts[N] (N>=1) below becomes args[N-1].
        """
        if rec is None: return
        cmd, args, slots = rec
        a = self.interp.aether.aether

        # Syn=X: write X.word = addr(this line's own name) -- the command
        # word X now dispatches to this handler. Same word-write WAX_CMD did
        # (`cmd in ("SET","WAX_CMD")` below), but the fact is declared on the
        # handler's own line. Runs for any cmd, before the primitive branches,
        # since the handler line is itself an ordinary NOVA/NOVAE.
        _syn = slots.get("Cmd")
        if _syn:
            _handler = slots.get("Name") or (args[0] if args else None)
            if _handler:
                self._wire_word_value(self._resolve(_syn), [_syn, _handler])

        # By-command dispatch (after the cross-cutting Cmd= word-write above): a
        # migrated command wires through its _COMMANDS handler; the rest fall
        # through to the branches below (migrated batch by batch).
        if cmd in _COMMANDS:
            _COMMANDS[cmd].wire(self, rec, filepath, lineno)
            return

        # (SWITCH/MATCH/PACK_BYTE/UNPACK_BYTE/ALLOC_TO and the FOR/DEF/SAVE/
        # WRITE_OUT/READ_BODY/ALLOC_RAW block/compound commands now wire through
        # their _COMMANDS handlers above.)

        # ── Primitive commands ───────────────────────────────────────────────
        # (NEW wires nothing -- its allocation is done in the alloc wave, via
        # _CmdNEW; there is no _CmdNEW.wire override.)

        # (SET writes its word value through its own _COMMANDS handler above --
        # _CmdSET.wire. SET [Name=]X [At=o] [Value=V | Of=Y]: Value=number/text,
        # Value=Y/bare = reference/self-ref, all via _wire_word_value.)

        # (NOVA wires its instruction lux via its _COMMANDS handler ->
        # _CmdNOVA.wire.)

        # NOVAE: synthesize a NOVA at runtime.
        # NOVAE label Op [El1=X] [El2=Y] [Exit=Z]
        # Mirrors NOVA syntax. Expands to the standard builder sequence:
        #   __LT_ALLOC_NOVA + 5 Move nodes + Voca WRITE_NOVA_SLOTS
        # The label names the __LT_ALLOC_NOVA node (the visible entry point).
        # Operands are _REF-style: addresses of luces holding the values.
        # Absent El2/Exit default to C_0 (zero/null), matching CLEAR semantics.
        # (NOVAE wires via its _COMMANDS handler -> _novae_wire.)

        # (RVOCA/RREDI wire their call/return NOVA node through their own
        # _COMMANDS handler above -- _CmdVocaRedi.wire.)

        # (CLEAR/NOP/JEQ/JZ/HASH_STEP -- simple direct-build macros whose args[0]
        # is their minted name -- now wire through their _COMMANDS handlers above,
        # alongside ALLOC_TO. These are hardcoded Python fast paths, run before
        # LOAD_MAIN reaches their own .re macro definitions.)

        # (ALLOC_TO now wires through its _COMMANDS handler above.)

        # Unknown command: LOAD_MAIN's own wave handles remaining macros.
        return

    def _novae_declare(self, rec, filepath, lineno, idx):
        # NOVAE alloc: pre-allocate the label + generated suffix nodes as
        # NOVA luces (see _CmdNOVAE). Mechanism method; dispatch is via the handler.
        cmd, args, slots = rec
        if len(args) < 1:
            return
        label = args[0]
        has_at  = 'At' in slots
        has_nxt = 'Next' in slots
        # Extra keys (In1=/... , anything not a reserved slot) only add
        # suffix nodes on the no-At= path -- an At=X line's extras are
        # wired straight onto X itself (Wave 2 wire's own NOVAE
        # handling), no extra nodes of this NOVAE line's own needed.
        n_extra = 0 if has_at else sum(
            1 for k in slots if k not in _NOVA_RESERVED_SLOTS)
        # Order matches Wave 2 wire's own emission order exactly (not
        # just "every node this line needs") -- physically-adjacent
        # allocation is what lets _chain_nova leave SLOT_NEXT at its
        # free fall-through 0 instead of writing an explicit jump for
        # every one of these nodes.
        nodes = [label]
        if n_extra:
            nodes.append(label + "_alloc")
        if not has_at:
            nodes.append(label + "_lux")
        nodes += [label + sfx for sfx in ("_op", "_e1", "_e2", "_ex", "_w")]
        if has_nxt:
            nodes += [label + "_nl", label + "_nlw"]
        if n_extra:
            for i in range(n_extra):
                nodes += [f"{label}_x{i}r", f"{label}_x{i}ra", f"{label}_x{i}rw",
                          f"{label}_x{i}v", f"{label}_x{i}va", f"{label}_x{i}vw"]
        for node in nodes:
            if not self._has(node):
                addr = self._alloc_nova(node)
                self._bind(node, addr)
                self.interp.aether.aether[addr + _WORD] = addr

    def _novae_wire(self, rec, filepath, lineno):
        # NOVAE wire: synthesize a runtime NOVA (see _CmdNOVAE). Mechanism
        # method; dispatch is via the handler.
        cmd, args, slots = rec
        if len(args) < 1 or not slots.get('Op'):
            return
        label = args[0]
        op    = slots.get("Op")
        e1    = slots.get("El1")
        e2    = slots.get("El2")
        exit_ = slots.get("Exit")
        at    = slots.get("At")    # optional: reuse existing lux instead of allocating
        nxt   = slots.get("Next")   # optional: wire next-slot of synthesized NOVA to this addr
        op1   = slots.get("Op1")   # optional: op for El1's own synthesized helper (default Move)
        op2   = slots.get("Op2")   # optional: op for El2's own synthesized helper (default Move)
        op3   = slots.get("Op3")   # optional: op for Exit's own synthesized helper (default Move)
        # Reset= is the one key=value pair Wave 0 leaves in args rather
        # than slots (not one of the five lux-slot names) — read it the
        # same way _wave1_lcount_line reads OWN/FREE tokens.
        reset = None
        for tok in args[1:]:
            if tok.startswith('Reset='):
                reset = tok.split('=', 1)[1]

        # All operands passed as STRINGS to _emit_nova (which calls _resolve).
        # Passing integers would route through _resolve's C_N path, giving
        # the wrong address (e.g. _resolve(484) -> addr(C_484) != addr(Move)).

        # El1=/El2=/Exit= may pair with Op1=/Op2=/Op3= (e.g. "El1=RA_C0
        # Op1=Idem") to resolve that one operand via Idem (Aspect #25,
        # zero dereferences) instead of the default Move (one
        # dereference). Move is right for the common case -- a register
        # whose own, *current* value should become the synthesized
        # instruction's operand (El1=RA_MA1: "whatever RA_MA1 currently
        # points to"). Idem is for the other, genuinely different case
        # -- a fixed, compile-time-known constant should become the
        # operand (El1=RA_C0 Op1=Idem: "RA_C0's own address, always,
        # unconditionally" -- what a `_REF` holder, SEAL RA_C0_REF
        # RA_C0, used to be needed for; this makes the holder
        # unnecessary, the same as every other Idem migration this
        # session, see BUGS.md Pattern Z, REF_TO_IDEM.md). Op1/Op2/Op3
        # are ordinary keys read the same way El1/El2/Exit are (see
        # LOAD_ELEM's own docstring, wax.re, for the Reca-side mirror
        # of this same convention) -- not string surgery on the value
        # itself, which is what the old "Idem:" value-prefix was.
        def _novae_resolve_op(v, op):
            return (op or "Move"), v

        e1_op,   e1_val   = _novae_resolve_op(e1, op1)
        e2_op,   e2_val   = _novae_resolve_op(e2, op2)
        exit_op, exit_val = _novae_resolve_op(exit_, op3)

        # Arbitrary extra keys (In1=/Op1'-style-but-not-reserved/... --
        # anything not in _NOVA_RESERVED_SLOTS), same convention as a
        # plain NOVA's own extras. Two different targets depending on
        # At=:
        #   At=X: X already has a real, load-time address, so this is
        #     wired exactly like a plain NOVA's extras (_wire_extra_lumens,
        #     right below) -- X's own size already accounts for them,
        #     via the matching branch in _wave1_lcount_line.
        #   no At=: the target lux is allocated at *interpreter* runtime
        #     by __LT_ALLOC_NOVA, which has no load-time address to wire
        #     against yet -- appended as extra chain nodes after WRITE_NOVA_SLOTS,
        #     sized via RA_MC_EXTRA_COUNT (see _setup_load_aspects).
        extra_items = [(k, v) for k, v in slots.items() if k not in _NOVA_RESERVED_SLOTS]

        if at:
            # At=X: reuse existing lux at X — skip __LT_ALLOC_NOVA.
            # Move the at= address into RA_MC_LUX so WRITE_NOVA_SLOTS knows the target.
            self._emit_nova(label,           "Move",  e1=at,          exit="RA_MC_LUX",      auto_next=True)
            if extra_items:
                self._wire_extra_lumens(self._resolve(at), dict(extra_items))
        else:
            n_extra = len(extra_items)
            if n_extra:
                # Stage the extra count where __LT_ALLOC_NOVA's native
                # handler reads it, one instruction before the alloc
                # itself runs -- label stays the externally-known entry
                # point either way, only what it does at offset 0 differs.
                self._emit_nova(label,          "Move", e1="C_%d" % n_extra, exit="RA_MC_EXTRA_COUNT", auto_next=True)
                self._emit_nova(label + "_alloc", "__LT_ALLOC_NOVA",                                    auto_next=True)
            else:
                # Default: allocate a fresh, ordinary-sized NOVA slot at runtime.
                self._emit_nova(label,           "__LT_ALLOC_NOVA",                               auto_next=True)
            self._emit_nova(label + "_lux",  "Move",  e1="RA_MA_RET", exit="RA_MC_LUX",      auto_next=True)
        self._emit_nova(label + "_op",   "Move",  e1=op,               exit="RA_MC_OP",       auto_next=True)
        self._emit_nova(label + "_e1",   e1_op,   e1=e1_val or "C_0",  exit="RA_MC_E1",       auto_next=True)
        self._emit_nova(label + "_e2",   e2_op,   e1=e2_val or "C_0",  exit="RA_MC_E2",       auto_next=True)
        self._emit_nova(label + "_ex",   exit_op, e1=exit_val or "C_0", exit="RA_MC_DEST",    auto_next=True)
        wis = "WRITE_NOVA_SLOTS_RESET" if reset else "WRITE_NOVA_SLOTS"
        if nxt:
            # Next=N: after WRITE_NOVA_SLOTS, also wire the synthesized NOVA's
            # next-slot to N. Emits: Add src+C_5→RA_MC_SLOT; Write slot←N.
            # Uses RA_MC_LUX which still holds the synthesized NOVA's address.
            self._emit_nova(label + "_w",    "Voca",  e1=wis, exit="RA_LINK", auto_next=True)
            self._emit_nova(label + "_nl",   "Add",   e1="RA_MC_LUX", e2="C_5", exit="RA_MC_SLOT", auto_next=True)
            self._emit_nova(label + "_nlw",  "Write", e1="RA_MC_SLOT", e2=nxt,  auto_next=True)
        else:
            self._emit_nova(label + "_w",    "Voca",  e1=wis, exit="RA_LINK", auto_next=True)
        if not at and extra_items:
            # Same (rel,val) pairs a plain NOVA's _wire_extra_lumens
            # would write directly -- here written by a runtime chain
            # instead, because the target address (RA_MC_LUX) isn't
            # known until __LT_ALLOC_NOVA actually runs. Idem (zero
            # dereferences, Aspect #25) stages each side's own raw
            # address into RA_MC_E1 -- reused here since WRITE_NOVA_SLOTS
            # has already consumed its earlier, e1-operand-staging use of
            # it -- then Add+Write places that address into the target
            # cell, exactly matching _add_lumen's raw (rel_addr, tgt_addr)
            # writes. The rel key is force-self-ref'd at load time
            # (same as _wire_extra_lumens): the WRITE doesn't need it
            # (Idem never dereferences), but a later WALK_NOVA-style
            # Equal-based lookup against this same key name does.
            for i, (k, v) in enumerate(extra_items):
                rel_addr = self._resolve(k)
                self._write(rel_addr, rel_addr)
                off_rel = _NOVA_SIZE - 1 + 2 * i
                off_val = off_rel + 1
                self._emit_nova(f"{label}_x{i}r",  "Idem",  e1=k, exit="RA_MC_E1", auto_next=True)
                self._emit_nova(f"{label}_x{i}ra", "Add",   e1="RA_MC_LUX", e2=f"C_{off_rel}", exit="RA_MC_SLOT", auto_next=True)
                self._emit_nova(f"{label}_x{i}rw", "Write", e1="RA_MC_SLOT", e2="RA_MC_E1", auto_next=True)
                self._emit_nova(f"{label}_x{i}v",  "Idem",  e1=v, exit="RA_MC_E1", auto_next=True)
                self._emit_nova(f"{label}_x{i}va", "Add",   e1="RA_MC_LUX", e2=f"C_{off_val}", exit="RA_MC_SLOT", auto_next=True)
                self._emit_nova(f"{label}_x{i}vw", "Write", e1="RA_MC_SLOT", e2="RA_MC_E1", auto_next=True)

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
    sources = re_sources()
    sources += [
        os.path.join(_HERE, 'loader.py'),
        os.path.join(_HERE, 'interpreter.py'),
        os.path.join(_HERE, 'aether.py'),
    ]
    for s in sources:
        try:
            if os.path.getmtime(s) > bin_mtime:
                return True
        except OSError:
            continue
    return False


def _freeze_prepare() -> "Loader":
    """Phase 1 of freeze(): build the Python-side authoritative structure
    AND every piece of pre-LOAD_MAIN bootstrap (file list, K_CURSOR, stack
    registers, the protected-write dispatch guard) -- everything freeze()
    itself sets up before running LOAD_MAIN, stopping right before that
    one execution. Returns (loader, all_files, file_list_base) -- the
    ready-to-run Loader, plus the two pieces freeze()'s own post-run
    register restore needs but can't recompute from the Loader alone.

    Exists so LOAD_MAIN can be run exactly once from a genuinely fresh
    state by two different callers: freeze() itself (via
    _run_load_main_once, immediately after this), and iris_trace.py's
    --load-main (which needs its OWN single instrumented run, not a
    second run stacked on top of freeze()'s already-mutated state --
    calling `_freeze_prepare()` then doing its own traced loop, instead of
    `load_or_freeze()` -> freeze() [runs LOAD_MAIN once, opaquely] ->
    _load_main's own loop [runs it a SECOND time on the mutated result],
    is what actually fixes that double-execution bug, not a change to
    iris_trace.py alone).

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

    # No pre-registration loop here anymore. This used to hand-allocate a
    # frozenset of bootstrap command words (_BOOTSTRAP_CMDS, then its
    # mechanically-discovered successor _bootstrap_only_cmds) as real
    # symbols before any file was read, so a bare mention of one as a
    # plain operand value (e.g. `El1=WAX_CMD` in wax.re/parser.re) would
    # resolve. It never needed to run this early: `_lazy_ok` -- the flag
    # that gates `_resolve`'s lazy-stub-on-miss path -- is already true by
    # the time any file's Wave 2 (build) pass sees its first line (set at
    # the top of `_run_pass_over`), including aspects.re, the very first
    # file loaded. So any of these words, mentioned as a value anywhere in
    # real .re source, already gets a correctly-sized stub the first time
    # `_resolve` sees it -- the same general mechanism that already
    # handles every other forward reference project-wide (see `_resolve`'s
    # own note: "everything that sweeps the project up front ... existed
    # to guarantee no miss ever happened. With this, a miss is ordinary").
    # Verified directly: removing this loop changes nothing but symbol
    # count (fewer stubs pre-minted) -- zero new "Unknown name" errors,
    # same pre-existing warnings as before. See BUGS.md Pattern Z for the
    # history of this loop's frozenset growing out of sync with reality,
    # which is the failure mode this deletion removes for good rather than
    # narrowing further.

    # Python loads all files — fully, without restrictions.
    # aspects.re must load first (its 24 names are used as literal op= values
    # by everything else). Found by recursive search (seed_candidates), not a
    # hardcoded path -- the canonical location is core/aspects.re, but the
    # search itself doesn't assume any particular directory layout.
    _seeds = seed_candidates()
    if not _seeds:
        print("Error: aspects.re not found anywhere under project root", file=sys.stderr)
        sys.exit(1)
    seed = _seeds[0]
    if len(_seeds) > 1:
        print(f"Warning: multiple aspects.re found, using {seed}; "
              f"others ignored: {_seeds[1:]}", file=sys.stderr)
    if not loader.load_file(seed):
        print(f"Error: failed to load seed file {seed}", file=sys.stderr)
        sys.exit(1)
    loader.auto_discover(_HERE, exclude_seed=os.path.relpath(seed, _HERE))
    interp.update_relations(loader.symbols)
    loader._setup_load_aspects()   # re-register after final update_relations

    # Reca LOAD_MAIN: a parallel pass over the same files.
    # Builds BS_HTAB independently. Not yet the source of truth.
    a = interp.aether.aether
    # re_sources order (full path), seed last -- see load_main_order.
    # Bootstrap files are already wired by Python Wave 2; LOAD_MAIN Sweep-B skips them.
    # All other files: LOAD_MAIN processes them (Python Wave 2 = scaffolding only).
    all_files = load_main_order(seed)

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

        # Bootstrap used to also clear and pre-populate BS_HTAB directly
        # here, in Python, as a single-word packed format (hash32<<32|addr32,
        # no *2 stride). That never matched HT_LOOKUP/HT_INSERT's real
        # 2-word-per-slot layout (word0=hash+lid, word1=name-pool pointer,
        # address = base + slot*2) -- it was writing entries Reca's own
        # lookup could never correctly read, and clearing luces starting at
        # the table's own header address, corrupting BS_HTAB_000's self-ref
        # in the process. Confirmed dead by removal: tests, lint, the full
        # check suite, and self-hosting were all identical with this block
        # disabled entirely.

        # LUX_REGISTRY / NOVA_REGISTRY: write contiguous address blocks before
        # K_CURSOR is frozen, so _bump_alloc moves the cursor correctly.
        lrb = loader.symbols.get("LUX_REGISTRY_BASE")
        lrs = loader.symbols.get("LUX_REGISTRY_SIZE")
        if lrb and lrs and loader._data_addrs:
            reg_base = loader._alloc(len(loader._data_addrs))
            for i, addr in enumerate(loader._data_addrs):
                a[reg_base + i] = addr
            a[lrb] = reg_base
            a[lrs] = len(loader._data_addrs)

        irb = loader.symbols.get("NOVA_REGISTRY_BASE")
        irs = loader.symbols.get("NOVA_REGISTRY_SIZE")
        if irb and irs and loader._nova_addrs:
            nova_base = loader._alloc(len(loader._nova_addrs))
            for i, addr in enumerate(loader._nova_addrs):
                a[nova_base + i] = addr
            a[irb] = nova_base
            a[irs] = len(loader._nova_addrs)

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
        from aether import STACK_TOP, STACK_BOTTOM, FRAME_SIZE, FLUX_BOTTOM, FLUX_TOP
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

        # Restore self-referential flag luces — Wave 2 macros may have written
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

        # Restore buffer self-refs — LOAD_MAIN clears them as part of buffer init.
        # Each buffer lux[0] must equal its own address so Move reads the addr.
        # (Currently a no-op: none of these three names resolve in the live
        # build any more — left as a guarded no-op rather than removed here,
        # out of scope for this session's cleanup.)
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

    # ── Pre-populate SK_DECL_TABLE ──────────────────────────────────
    # Scan every .re file (not a hardcoded <root>/commands.re -- see
    # _discover_nova_naming_commands) for NOVAE builders with At=RA_MA0 and NOVA builders
    # with El1=RA_MA0 + Exit=RA_MC_LUX — the two patterns that mean "this
    # command's first positional arg becomes a named NOVA." Their Cmd=
    # names are the declare-only allowlist: commands that must run during
    # LOAD_MAIN's declare pass (Wave 2) to establish forward references.
    # Replaces the static FOR loop that used to live in wax.re LOAD_MAIN.
    sk_decl_table = loader.symbols.get("SK_DECL_TABLE", 0)
    sk_decl_table_base = loader.symbols.get("SK_DECL_TABLE_BASE", 0)
    if sk_decl_table and sk_decl_table_base:
        decl_cmds = set()
        for _cmds_path in re_sources():
            with open(_cmds_path, encoding='utf-8', errors='replace') as f:
                raw = f.readlines()
            for _lineno, text in _strip_comments(raw):
                p = _tokenize_quoted(text)
                if not p:
                    continue
                cmd = p[0].upper()
                _args, _slots = _split_args_slots(p[1:])
                _syn = _slots.get('Cmd')
                if not _syn:
                    continue
                if cmd == 'NOVAE' and _slots.get('At') == 'RA_MA0':
                    decl_cmds.add(_syn)
                elif (cmd == 'NOVA'
                      and _slots.get('El1') == 'RA_MA0'
                      and _slots.get('Exit') == 'RA_MC_LUX'):
                    decl_cmds.add(_syn)
        ptr = sk_decl_table
        for cmd_name in sorted(decl_cmds):
            cmd_addr = loader.symbols.get(cmd_name, 0)
            if cmd_addr:
                a[ptr] = cmd_addr
                ptr += 1
        a[ptr] = 0  # null terminator
        a[sk_decl_table_base] = sk_decl_table

    # ── Pre-populate SK_OPENER_TABLE ────────────────────────────────
    # The block openers, as the full-64-bit djb2 hash of each command word
    # -- exactly what BS_READ_TOKEN stores in a token's hash field, so
    # TIDE_RESTRUCTURE (wax.re) recognises a block header by its first
    # token's hash with no hand-kept list. Same single source Python's own
    # _parse_indent_blocks reads (_discover_block_openers, which discovers
    # them from the corpus's Cmd=WORD Open=mode declarations), and the same
    # freeze-time precompute pattern as SK_DECL_TABLE above.
    sk_opener_table = loader.symbols.get("SK_OPENER_TABLE", 0)
    sk_opener_table_base = loader.symbols.get("SK_OPENER_TABLE_BASE", 0)
    if sk_opener_table and sk_opener_table_base:
        ptr = sk_opener_table
        for word in sorted(_discover_block_openers()):
            a[ptr] = _reca_djb2(word)
            ptr += 1
        a[ptr] = 0  # null terminator
        a[sk_opener_table_base] = sk_opener_table

    # ── Pre-populate SK_REGION_TABLE + SK_COMBO_SIZE (region combos, B) ──
    # Region membership is derived on THIS side -- the source of truth:
    # _name_regions / _combo_key (In=OWN/SAVE, K_ -> save+lock, RA_ -> own).
    # LOAD_MAIN does not re-derive any of it; it consumes two baked tables,
    # the same pattern as SK_DECL_TABLE / SK_OPENER_TABLE above:
    #   SK_REGION_TABLE -- [name-hash, bitmask] pairs, one per region name,
    #     walked by REGION_LOOKUP (alloc.re) by djb2 hash so it is stable
    #     across Python's and LOAD_MAIN's differing layouts.
    #   SK_COMBO_SIZE[mask] -- total luces per combo, summed with LOAD_MAIN's
    #     own stub sizing (NOVA_SIZE for a nova/naming name, else 1), so
    #     RESERVE_COMBOS reserves exactly what the prepass then fills.
    # Bitmask matches REGION_LOOKUP / ALLOC_COMBO: own=1, save=2, lock=4.
    sk_region_table = loader.symbols.get("SK_REGION_TABLE", 0)
    sk_region_table_base = loader.symbols.get("SK_REGION_TABLE_BASE", 0)
    sk_combo_size = loader.symbols.get("SK_COMBO_SIZE", 0)
    if sk_region_table and sk_region_table_base:
        _RBIT = {'own': 1, 'save': 2, 'lock': 4}
        combo_totals = [0] * 8
        ptr = sk_region_table
        for name in sorted(loader._name_regions):
            mask = 0
            for r in loader._name_regions[name]:
                mask |= _RBIT.get(r, 0)
            if not mask:
                continue   # only own/save/lock form combo blocks today
            a[ptr] = _reca_djb2(name)
            a[ptr + 1] = mask
            ptr += 2
            combo_totals[mask] += _NOVA_SIZE if name in loader._nova_names else 1
        a[ptr] = 0  # null terminator (a[ptr+1] stays 0)
        a[sk_region_table_base] = sk_region_table
        if sk_combo_size:
            for m in range(1, 8):
                a[sk_combo_size + m] = combo_totals[m]

    # ── SK_HTAB command seed ────────────────────────────────────────────────
    # Python bakes the command vocabulary (keyword -> handler-symbol lux)
    # straight into the self-hosted hash table, the same way it bakes
    # SK_DECL_TABLE / SK_OPENER_TABLE / SK_REGION_TABLE above.
    #
    # Why this is needed: the aether symbol tables (BS_HTAB, SK_HTAB) are empty
    # until a load pass runs -- Python resolves through its own native `symbols`
    # dict, not the aether table, and cannot share that with Reca. The
    # self-hosted LOAD_MAIN pass interns declared *names* as it processes them,
    # but never the command *keywords*: those are registered on the Python side
    # through the Cmd= slot's own declare/wire methods, which the aether has no
    # way to replay. So dispatch's BS_LOOKUP had nothing to resolve command
    # words (NOVA/NEW/SET/...) against -- every command line fell through as
    # "unknown name". Command handlers are stable runtime code whose addresses
    # do not move between the Python build and LOAD_MAIN, so the vocabulary is
    # exactly the consistent core to hand over rather than rebuild. LOAD_MAIN no
    # longer clears SK_HTAB (wax.re), so this seed survives and its own name
    # interning lands on top; one BS_LOOKUP recognizer then serves both the
    # prepass and the build passes (the MATCH byte-tree is gone).
    #
    # Entry layout matches htable.re exactly (post-header-removal): bucket i at
    # base + i*2, word0 = hash32<<32 | lid, word1 = &[len, bytes...]. Names live
    # in their own freshly-allocated block, NOT the runtime HT_NAME_POOL, so
    # LOAD_MAIN's pool reset never disturbs them.
    sk_htab = loader.symbols.get("SK_HTAB_000", 0)
    bs_mask = loader.symbols.get("BS_HT_MASK", 0)
    if sk_htab and bs_mask:
        cmd_base = a[sk_htab]                       # NEW Count/Width region ptr
        mask     = a[bs_mask]
        cmd_words = sorted(n for n in _known_commands(loader)
                           if n in loader.symbols)
        name_cells = sum(1 + len(n) for n in cmd_words)
        names_ptr = loader._alloc(name_cells) if name_cells else 0
        for name in cmd_words:
            lid  = loader.symbols[name]
            h32  = _reca_djb2(name) & 0xFFFFFFFF
            name_ptr = names_ptr
            a[names_ptr] = len(name); names_ptr += 1
            for ch in name:
                a[names_ptr] = ord(ch); names_ptr += 1
            slot = h32 & mask
            while a[cmd_base + slot * 2] != 0:      # open-addressing, as HT_INSERT
                slot = (slot + 1) & mask
            a[cmd_base + slot * 2]     = (h32 << 32) | (lid & 0xFFFFFFFF)
            a[cmd_base + slot * 2 + 1] = name_ptr
        # The names block was bump-allocated above, AFTER the K_CURSOR double-
        # deref block set a[K_CURSOR+1] to the then-current cursor. Refresh it to
        # the true post-seed cursor, or LOAD_MAIN's runtime K_CURSOR would still
        # point at the names block and re-hand-out those cells, overwriting the
        # command name bytes (word1 targets) -- HT_NAME_CMP then reads a lux word
        # as a name length and its char loop never terminates.
        _kc = loader.symbols.get("K_CURSOR", 0)
        if _kc:
            a[_kc + 1] = loader._bump

    return loader, all_files, file_list_base


def _run_load_main_once(loader: "Loader", max_steps: int = 200_000_000) -> None:
    """Phase 2 of freeze(): actually execute LOAD_MAIN once against a
    Loader from `_freeze_prepare()`. This IS the real, authoritative
    self-hosting run -- freeze() calls it exactly once. Do not call this
    twice on the same loader: LOAD_MAIN mutates the aether in place, and a
    second call runs on top of already-mutated state (already-inserted
    hashtable entries, an already-advanced K_CURSOR, ...), which is a
    different computation, not a repeat of the same one -- this was the
    exact bug iris_trace.py's --load-main had before it switched to
    calling `_freeze_prepare()` directly instead of `load_or_freeze()`.
    """
    interp = loader.interp
    load_main_addr = loader.symbols.get("LOAD_MAIN", 0)
    if not load_main_addr:
        return
    try:
        interp.execute_aether(load_main_addr, no_cache=True, max_steps=max_steps)
        print("  LOAD_MAIN complete.", file=sys.stderr)
    except Exception as e:
        import traceback as _tb
        print(f"  LOAD_MAIN error: {type(e).__name__}: {e}", file=sys.stderr)
        _tb.print_exc()
        # Names are owned by the Python symtab (loader.symbols); no aether
        # name-recovery needed. Reca does not store names in a lux.


def freeze() -> "Loader":
    """Bootstrap Aether from .re sources: build the Python-side
    authoritative structure (_freeze_prepare), run the self-hosted
    LOAD_MAIN once for real (_run_load_main_once), restore the handful of
    volatile registers LOAD_MAIN's own execution leaves stale, and write
    reca.bin + reca.sym.
    """
    loader, all_files, file_list_base = _freeze_prepare()
    _run_load_main_once(loader)
    interp = loader.interp
    a = interp.aether.aether

    # LUX_REGISTRY and NOVA_REGISTRY are built by LOAD_MAIN directly
    # (LOAD_REG_LUX/LOAD_REG_NOVA subroutines + LOAD_FINALISE_REGISTRY).
    # Python _write_registry no longer needed.

    # Restore system registers that LOAD_MAIN's builder macros may have corrupted.
    # These are set by loader.py init, but JZ/SWITCH builders overwrite them if
    # the _J/_K NOVAs happen to be allocated at the same address as system luces.
    _bs_fc = loader.symbols.get("BS_FILE_COUNT", 0)
    if _bs_fc: a[_bs_fc] = len(all_files)
    _bs_fl = loader.symbols.get("BS_FILE_LIST", 0)
    if _bs_fl: a[_bs_fl] = file_list_base

    # SAVE (see _reserve_save_block/_reserve_ownsave_block) replaced the
    # restoration this comment used to describe: instead of detecting and
    # patching corruption after LOAD_MAIN runs, a name that must stay
    # readable as a stable address gets that address from a block nothing
    # else can ever be allocated into, structurally, before LOAD_MAIN
    # ever starts. Confirmed empirically before removal: with the old
    # restoration loop fully disabled, a from-scratch build's tests,
    # lint, and self-hosting count were all identical to a build with it
    # enabled — it was restoring 11 names that were never actually at
    # risk (each cached into its own _BASE lux before anything could
    # touch it) and one (RA_JEQ_FLAG) where "restoring" it actively
    # overwrote a legitimate live comparison-flag value with a
    # meaningless self-ref, for a name nothing ever reads directly by
    # address anyway (only through RA_JEQ_FLAG_PTR).

    k_cursor    = loader.symbols.get("K_CURSOR", 0)
    k_watermark = loader.symbols.get("K_WATERMARK", 0)
    k_trace     = loader.symbols.get("K_TRACE_POS", 0)
    # After LOAD_MAIN, read back cursor from K_CURSOR+1 (updated by ALLOC).
    # No pointer restoration needed here anymore -- K_CURSOR/K_WATERMARK/
    # K_TRACE_POS are all SAVE'd (see alloc.re), so their address is
    # structurally protected. Confirmed empirically before removing the old
    # forced restore: disabling it entirely left tests/lint/the full check
    # suite/self-hosting all identical -- LOAD_MAIN never actually corrupts
    # their double-deref pointer value either.
    if k_cursor:
        final_bump = a[k_cursor + 1]
        if final_bump and final_bump > loader._bump:
            loader._bump = final_bump
            loader._water = final_bump - 1
    if k_watermark:
        wm_val = a[k_watermark + 1]
        if wm_val and wm_val > loader._water:
            loader._water = wm_val
    if k_trace:
        a[k_trace + 1] = 0

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

    from aether import STACK_TOP
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

    # Self-hosting progress: how many source lines did LOAD_MAIN actually
    # dispatch, out of how many there are?
    #
    # This used to report SK_FIDX out of BS_FILE_COUNT -- "42/42 files". That
    # number never meant what it read as. SK_FIDX is the loop variable of the
    # file walk, reset to 0 at the start of each of the three waves, so it
    # reports where the run stopped counting files, not what it did with
    # them: a run that opened all 42 files, dispatched one line in each and
    # gave up showed a full bar. Lines dispatched cannot be faked that way --
    # nothing increments it except a line actually going through the
    # dispatcher.
    # Count what the dispatcher will actually meet, not what the file holds.
    # A macro body is read once as source and then dispatched again for each
    # line it expands into, so source lines and dispatched lines are simply
    # different quantities -- and the expanded count is the one that can be
    # compared against a counter sitting in the dispatcher. _read_re_file is
    # the same expansion the loader itself uses, so the two sides are counting
    # the same thing by construction rather than by agreement.
    total_lines = 0
    for fp in all_files:
        try:
            total_lines += len(_read_re_file(fp, loader))
        except Exception:
            pass
    # Two waves dispatch every file -- declare, then load -- so every expanded
    # line is met twice. (The prepass walks files through LOAD_PREPASS_FILE,
    # which never reaches the line dispatcher, so it adds nothing.)
    total_lines *= 2
    total_lines *= 2   # declare wave and load wave both dispatch every file

    def _n(sym):
        ad = loader.symbols.get(sym, 0)
        return int(a[ad]) if ad else 0

    done_lines   = _n('SK_LINES_DONE')
    done_files   = _n('SK_FILES_DONE')
    done_cmds    = _n('SK_CMDS_DONE')
    done_skipped = _n('SK_SKIPPED_DONE')
    done_noname  = _n('SK_NONAME_DONE')
    n_files      = len(all_files) * 2

    # Four numbers, because one was never enough. Lines dispatched says how
    # far the reader got; files says whether it got all the way through each
    # one; commands vs skipped says whether anything was actually done with
    # what was read. The build that reported a full bar while dispatching
    # 194 lines could not have survived any two of these side by side.
    if total_lines:
        frac   = min(1.0, done_lines / total_lines)
        filled = int(20 * frac)
        bar    = '█' * filled + '░' * (20 - filled)
        print(f"Self-hosting: [{bar}] {done_lines}/{total_lines} lines "
              f"({100 * done_lines // total_lines}%)", file=sys.stderr)
    else:
        print(f"Self-hosting: {done_lines} lines dispatched", file=sys.stderr)
    print(f"              files {done_files}/{n_files}   "
          f"commands run {done_cmds}   no handler {done_skipped}   "
          f"unknown name {done_noname}",
          file=sys.stderr)

    return loader


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
    the frozen graph — used by repl.py --run, test_macros.py, and iris.py.
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
    from aether import Aether
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
