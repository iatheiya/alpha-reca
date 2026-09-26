"""
interpreter.py — Reca execution engine

NOVA lux compact layout (fixed slots, direct-index):
  slot 0  = word   = self-ref (aether[addr] = addr; Voca uses this)
  slot 1  = op     = primitive addr (dispatch key)
  slot 2  = e1     = first operand
  slot 3  = e2     = second operand
  slot 4  = exit   = destination (store target / jump target / RA_LINK addr for Voca)
  slot 5  = next   = 0 → implicit fall-through (see below); !0 → explicit graph link
  slot 6  = pad    = 0 → no extra lumens (free, as always); !=0 → extra LINK
                     lumen pairs (rel_addr, tgt_addr), 0-terminated, start
                     right AT this slot -- same self-describing pairs a Data
                     lux already uses. Only consulted when slot 5 (next) is
                     0 -- an explicit next (warp/flux) is unrelated. See
                     _nova_extra_skip.

Data lux layout:
  slot 0  = word   = value
  slot 1+ = lumen pairs (rel_addr, tgt_addr), 0-terminated

Key invariant: aether[nova_lux] = nova_lux (self-ref)
  -> Voca El1=func reads aether[func] = func -> jumps to func directly.

Three next-pc paths (checked in this order every step):
  line  — SLOT_NEXT == 0          → nxt = pc + NOVA_SIZE, or, if the pad
                                     slot is non-zero, past its extra lumens
                                     (still arithmetic, never a jump --
                                     see _nova_extra_skip)
  warp  — 0 < SLOT_NEXT < FLUX_BOTTOM → nxt = aether[SLOT_NEXT] (explicit address jump)
  flux  — SLOT_NEXT >= FLUX_BOTTOM → nxt = _exec_flux(...) (structured parallel branch)

Handlers signature: (a1, a2, exit, nxt, aether) -> next_pc
"""
import os
import sys
import stat

from aether import Aether, XLEN, _MASK64


def _s64(v: int) -> int:
    v = int(v) & _MASK64
    return v - (1 << XLEN) if v >= (1 << (XLEN - 1)) else v


def _u64(v: int) -> int:
    return int(v) & _MASK64


def _trunc_div_s(a: int, b: int) -> int:
    if b == 0: return 0
    sign = -1 if (a < 0) != (b < 0) else 1
    return sign * (abs(a) // abs(b))


def _trunc_mod_s(a: int, b: int) -> int:
    if b == 0: return 0
    return a - b * _trunc_div_s(a, b)


def _read_packed_bytes(aether, base: int, byte_count: int) -> bytearray:
    """Read byte_count bytes from packed aether buffer (8 bytes per lux, little-endian)."""
    buf = bytearray(byte_count)
    for i in range(byte_count):
        buf[i] = (aether[base + (i >> 3)] >> ((i & 7) << 3)) & 0xFF
    return buf


def _read_packed_cstr(aether, base: int) -> bytearray:
    """Read a null-terminated packed string from aether (8 bytes per lux, little-endian)."""
    pb = bytearray()
    i = base
    while i < len(aether):
        word = aether[i]
        if word == 0: break
        for shift in range(0, 64, 8):
            b = (word >> shift) & 0xFF
            if b == 0: return pb
            pb.append(b)
        i += 1
    return pb


class LockViolation(Exception):
    """Raised when an ordinary (non-Key) opcode's write target falls
    within a LOCK-protected range. This should never happen in a correct
    program -- it means some instruction, other than the one specific
    site declared with the Key opcode, is trying to write where nothing
    but that one authorized writer may ever write. A real bug to surface
    loudly, not silently ignore.
    """


class KeyShapeError(Exception):
    """Raised when a Key instruction's own Op1 extra-lumen names
    something _resolve_key_shape doesn't recognize. A typo in Op1= should
    surface here, at first use, not as a mysteriously-wrong write or a
    silent no-op -- see _resolve_key_shape's own note.
    """


class Interpreter:
    """Execute Reca programs from the Aether.

    Python knows: 6-lux instruction layout, 32 op handlers.
    Python does NOT know: lumen structure, data Lux layout, special luces,
    RA_LINK location (resolved through symbols), trace location (same).
    """

    __slots__ = (
        "aether", "R", "_dispatch",
        "_trace_pos_lux",       # aether[symbols['K_TRACE_POS']] points to the trace lux
        "_symbol_names",         # {addr: name} for progress display
        "pc",                    # last pc (for fault handler)
        # Exire register addresses (read from R after update_relations)
        "_sc_nr", "_sc_a0", "_sc_a1", "_sc_a2",
        "_sys_write", "_sys_write_packed", "_sys_read", "_sys_openat", "_sys_close", "_sys_exit",
        "_fault_vector",
        "_ra_link", "_ra_sp", "_ra_frame_size",  # automatic Voca/Redi call-stack registers
        "_block_syscalls",
        "_icache",               # shared with _run/_run_traced; _write invalidates touched entries
        "_write_gen",            # monotonic counter: incremented on every Write; used for icache validity
        "_pending_next",         # next addrs collected by _exec_flux (for future Aria scheduler)
        "_lock_ranges",          # [(base, end), ...] for every LOCK-containing combo
                                 # block (loader.py's _combo_blocks) -- set once,
                                 # before execution begins. Checked at write time by
                                 # every ordinary opcode; Write/Key are the exception
                                 # (their real target isn't the exit operand at all).
        "_key_shape",            # {pc: handler} -- which write-shape handler a given
                                 # Key instruction resolves to (Op1's target), memoized
                                 # per pc by _exec_key/_resolve_key_shape. Only consulted
                                 # in cached execution -- see _exec_key's own note.
        # NOVA layout slots — read from aether via update_relations (defined in constants.re)
        "_slot_op", "_slot_e1", "_slot_e2", "_slot_exit", "_slot_next", "_slot_pad", "_nova_size",
    )

    def __init__(self) -> None:
        self.aether        = Aether()
        self.R: dict         = {}   # name → address (filled by loader/update_relations)
        self._dispatch: dict = {}
        # NOVA layout slots with defaults (overridden by update_relations when constants.re loaded)
        self._slot_op   = 1
        self._slot_e1   = 2
        self._slot_e2   = 3
        self._slot_exit = 4
        self._slot_next = 5
        self._slot_pad  = 6
        self._nova_size  = 7
        self._trace_pos_lux = 0
        self._symbol_names   = {}
        self.pc              = 0
        self._sc_nr = self._sc_a0 = self._sc_a1 = self._sc_a2 = 0
        self._sys_write = self._sys_write_packed = self._sys_read = self._sys_openat = 0
        self._sys_close = self._sys_exit = 0
        self._fault_vector = 0
        self._ra_link = self._ra_sp = self._ra_frame_size = 0
        self._block_syscalls = False
        self._icache: dict    = {}   # populated during run; _write invalidates
        self._write_gen: int  = 0    # incremented on every Write
        self._pending_next: list = [] # filled by _exec_flux for Aria scheduler
        self._lock_ranges: list = []  # set by loader.py before running LOAD_MAIN
        self._key_shape: dict = {}    # {pc: handler}, see _exec_key

    def update_relations(self, new_symbols: dict) -> None:
        """Called after freeze/thaw. Resolves addresses from symbol table.

        Python looks up only what it needs:
        - The 32 Aspect op_ids (to build dispatch table)
        - RA_LINK address (for Call/Redi) — resolved through symbols
        - K_TRACE_POS address (for trace) — resolved through symbols
        - Exire register addresses
        - Fault vector address
        """
        self.R.update(new_symbols)
        R = self.R
        a = self.aether.aether

        # NOVA layout slots — read from aether (defined in constants.re as NEWSET SLOT_*).
        # Not hardcoded: Aria owns layout, Python only reads what Aria declared.
        def _slot(name, default):
            lux = R.get(name, 0)
            return int(a[lux]) if lux else default
        self._slot_op   = _slot("SLOT_OP",   1)
        self._slot_e1   = _slot("SLOT_E1",   2)
        self._slot_e2   = _slot("SLOT_E2",   3)
        self._slot_exit = _slot("SLOT_EXIT", 4)
        self._slot_next = _slot("SLOT_NEXT", 5)
        self._slot_pad  = _slot("SLOT_PAD",  6)
        self._nova_size  = _slot("NOVA_SIZE",  7)

        # Build dispatch: op_id → handler fn.
        # op_id is the address of the Lux (aether[lux_addr] = lux_addr, self-ref).

        # ── 25 Native Aspects (aspects.re) ────────────────────────────────────
        # These map 1:1 to CPU instructions. Irreducible from each other.
        # Changing this set is a real, deliberate decision — not a casual one,
        # but also not some irreversible redefinition of Reca: a previously-
        # missing category (see Idem, aspects.re) is not the same as "closed".
        _native = {
            "Read":    self._read,      "Write":   self._write,
            "Add":     self._add,       "Sub":     self._sub,
            "Mul":     self._mul,       "Div":     self._div,
            "Rem":     self._mod,       "UDiv":    self._divu,
            "URem":    self._modu,      "And":     self._and,
            "Or":      self._or,        "Xor":     self._xor,
            "Left":    self._shl,       "Right":   self._shr,
            "ARight":  self._shra,      "Equal":   self._cmpeq,
            "Less":    self._cmplt,     "ULess":   self._cmpltu,
            "JumpIf":  self._jumpif,    "JumpReg": self._jumpreg,
            "End":     self._end,       "Exire":   self._syscall,
            "Voca":    self._voca,      "Redi":    self._redi,
            "Idem":    self._idem,
        }

        # ── Derived ops (derived.re) ──────────────────────────────────────────
        # Expressible through native aspects in 1-3 steps.
        # Present here because the Python interpreter has no compile step to
        # lower them. A native binary never needs these — the compiler emits
        # native instructions directly. NOT aspects. NOT part of the Reca ISA.
        _derived = {
            "Move":              self._move,      # Add(src, C_0, tgt)
            "Jump":              self._jump,      # JumpIf(C_1, dest)
            "Not":               self._not,       # Xor(a, -1)
            "Greater":           self._cmpgt,     # Less(b, a) — swapped els
            "UGreater":          self._cmpgtu,    # ULess(b, a) — swapped els
            "NotEqual":          self._cmpne,     # not Equal(a, b)
            "LessOrEqual":       self._cmple,     # not Less(b, a)
            "ULessOrEqual":      self._cmpleu,    # not ULess(b, a)
            "GreaterOrEqual":    self._cmpge,     # not Less(a, b)
            "UGreaterOrEqual":   self._cmpgeu,    # not ULess(a, b)
            "Load":              self._read,      # alias Read
            "Store":             self._write,     # alias Write
        }

        # ── Relations-only ops (relations.re) ─────────────────────────────────
        # Neither hardware-native (aspects.re) nor derivable from hardware
        # ops in 1-3 steps (derived.re) -- purely Reca's own interpreter-
        # level semantics, existing only so Python can dispatch on a name.
        # Key: the authorized counterpart to Write. LOCK-protected data
        # (see loader.py's _combo_key/_lock_names) refuses any write from
        # every ordinary opcode -- Key is the one exception, identical
        # write semantics to Write, but skips the guard. Being dispatched
        # via Key rather than Write, at the one specific declared call
        # site that legitimately needs to write there, IS the
        # authorization -- no separate authorized-code region, no per-NOVA
        # flag, nothing else to check.
        #
        # Key's own write shape is resolved per-instance at runtime, not
        # by which name got dispatched -- see _exec_key/_resolve_key_shape.
        # "KeyMove" (Move's single-indirection shape, for LOCK-protected
        # data that isn't the double-deref K_ pattern Write's shape
        # assumes) is NOT a separately dispatched op name for that reason:
        # every Key instruction's own SLOT_OP is always literally "Key",
        # regardless of Op1, so registering a second dispatch entry here
        # would never actually be reached. _key_move stays a plain method,
        # looked up by _resolve_key_shape, same as _key itself is.
        _relations_only = {
            "Key": self._key,
        }

        self._dispatch = {}
        for name, fn in {**_native, **_derived, **_relations_only}.items():
            addr = R.get(name)
            if addr:
                self._dispatch[addr] = fn

        # Trace: K_TRACE_POS is an aria symbol. Its word is the address of the
        # lux holding the current trace position (0 = tracing off).
        # iris.re writes to aether[K_TRACE_POS.word] to enable/disable.
        # The double indirection matches the existing convention in iris.re/wane.re.
        a = self.aether.aether
        k_trace = R.get("K_TRACE_POS", 0)
        self._trace_pos_lux = a[k_trace] if k_trace else 0

        # Exire registers
        self._sc_nr  = R.get("SC_NR",  0)
        self._sc_a0  = R.get("SC_A0",  0)
        self._sc_a1  = R.get("SC_A1",  0)
        self._sc_a2  = R.get("SC_A2",  0)

        # Exire numbers (stored as word in their Lux)
        def _w(name): return a[R[name]] if name in R and R[name] else 0
        self._sys_write         = _w("SYS_WRITE")
        self._sys_write_packed  = _w("SYS_WRITE_PACKED")
        self._sys_read          = _w("SYS_READ")
        self._sys_openat = _w("SYS_OPENAT")
        self._sys_close  = _w("SYS_CLOSE")
        self._sys_exit   = _w("SYS_EXIT")

        self._fault_vector = R.get("FAULT_VECTOR", 0)

        # Automatic Voca/Redi call-stack registers (see _voca/_redi).
        self._ra_link       = R.get("RA_LINK", 0)
        self._ra_sp         = R.get("RA_SP", 0)
        ra_frame_size_lux   = R.get("RA_FRAME_SIZE", 0)
        self._ra_frame_size = int(a[ra_frame_size_lux]) if ra_frame_size_lux else 8

    # ── Convenience wrappers for loader.py ───────────────────────────────────
    def get_word(self, addr: int) -> int:  return self.aether.aether[addr]
    def set_word(self, addr: int, v: int): self.aether.aether[addr] = v & _MASK64

    # ── Execution loops ───────────────────────────────────────────────────────

    def execute_aether(self, start: int, progress_every: int = 0, sym=None,
                       no_cache: bool = False, max_steps: int = 0) -> int:
        """Dispatch to traced or untraced loop.

        no_cache=True disables icache — required for LOAD_MAIN which modifies
        luces during graph construction (icache would serve stale snapshots).
        This applies whether or not tracing is also active: no_cache and
        tracing are two independent reasons to pick a path (self-modifying
        graph construction; wanting a pc-by-pc record), and a caller asking
        for both gets both -- no_cache used to be silently dropped whenever
        tracing happened to be on, because this only ever checked tracing
        before routing to _run_traced, which always cached unconditionally.
        """
        tpc = self._trace_pos_lux
        if tpc and self.aether.aether[tpc]:
            return self._run_traced(start, progress_every, sym, use_cache=not no_cache)
        return self._run(start, progress_every, sym, use_cache=not no_cache,
                         max_steps=max_steps)

    def _run(self, start: int, progress_every: int, sym,
             use_cache: bool = True, max_steps: int = 0) -> int:
        """Untraced execution loop — hot ops inlined.

        use_cache=False disables icache for graph-building contexts (LOAD_MAIN).
        use_cache=True (default) enables icache for repeated Phase B macro calls.

        Two real shapes, not more: the cached path below (only when
        use_cache and no progress reporting -- "zero branch overhead"
        for the genuinely hot case) and everything else, one shared
        uncached loop covering no-cache, progress reporting, or both at
        once via cheap per-iteration checks. progress_every truthy still
        implies uncached execution today (unchanged from before this
        merge) -- that coupling was never revisited, just no longer
        duplicated.
        """
        aether    = self.aether.aether
        dispatch  = self._dispatch
        pc        = start
        steps     = 0
        _max_steps = max_steps  # 0 = unlimited
        SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, SLOT_PAD, NOVA_SIZE = self._slot_constants()
        # Dynamic flux_bottom — correct even if Aether was created with custom size
        _FLUX_BOTTOM = self.aether.flux_bottom
        # Every other opcode now dispatches uniformly through dispatch.get(op_id)
        # in every branch below (see the branch-2/3/4 consolidation this
        # session) -- OP_WRITE is the one exception, still checked directly
        # because branch 2's icache deliberately never caches a write (its
        # whole point is a side effect elsewhere, and it bumps _write_gen
        # itself -- caching it would immediately invalidate on the next check).
        OP_WRITE  = self.R.get("Write",  -7)
        OP_KEY    = self.R.get("Key",    0)

        fv = self._fault_vector

        if use_cache and not progress_every:
            ic = self._icache
            # Generation-counter icache: each entry stores (fn,a1,a2,ex,nxt,gen).
            # Write increments _write_gen (O(1)) instead of scanning a range.
            # Cache hit with stale gen → re-execute and re-cache. O(1) amortised.
            wg = self._write_gen
            while pc:
                if _max_steps:
                    steps += 1
                    if steps > _max_steps:
                        break
                cached = ic.get(pc)
                if cached and cached[5] == wg:
                    # No lock re-check here: ex is a fixed property of this
                    # pc as long as it stays cached (unrewritten) -- already
                    # verified once, below, the first time this address was
                    # ever cached. Re-deriving the same verdict on every
                    # repeat visit was real, paid-every-step cost for a
                    # once-per-address fact.
                    pc = cached[0](cached[1], cached[2], cached[3], cached[4], aether)
                    continue
                op_id = aether[pc + SLOT_OP]
                a1    = aether[pc + SLOT_E1]
                a2    = aether[pc + SLOT_E2]
                ex    = aether[pc + SLOT_EXIT]
                _raw  = aether[pc + SLOT_NEXT]
                if _raw == 0:
                    _pad = aether[pc + SLOT_PAD]
                    nxt = pc + NOVA_SIZE if _pad == 0 else self._nova_extra_skip(pc + SLOT_PAD, aether)
                elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
                else: nxt = _raw
                if op_id == OP_WRITE:
                    # Not cached: a write must always re-run (its whole
                    # point is a side effect elsewhere), and it also
                    # bumps wg itself -- caching a stale wg snapshot
                    # here would immediately mismatch on the very next
                    # cache check. Guard: Write's real target is
                    # aether[a1], not the exit operand (which is
                    # unused here) -- checked directly, not via
                    # _check_lock_write (that helper only looks at
                    # the exit operand, which every OTHER opcode's
                    # real target actually is).
                    if a1 and a2:
                        target = aether[a1]
                        for lo, hi in self._lock_ranges:
                            if lo <= target < hi:
                                raise LockViolation(
                                    f"Write attempted to LOCK-protected address {target} "
                                    f"(range [{lo}, {hi})) -- only Key may write there")
                        aether[target] = aether[a2]
                        wg += 1
                        self._write_gen = wg
                    pc = nxt
                    continue
                if op_id == OP_KEY:
                    # Not cached (like Write, and for the same reason:
                    # _key/_key_move's own write always bumps _write_gen,
                    # so a cached entry for this pc would be stale on the
                    # very next check regardless -- pointless to store).
                    # Which shape to run IS memoized, inside _exec_key
                    # itself (use_cache=True), separately from this outer
                    # icache. Resyncs the local wg afterward: the shape
                    # handler bumps self._write_gen directly, across the
                    # function-call boundary this local copy can't see.
                    pc = self._exec_key(pc, a1, a2, ex, nxt, aether, True)
                    wg = self._write_gen
                    continue
                fn = dispatch.get(op_id)
                if fn is not None:
                    # Checked once, here, before this pc is ever cached --
                    # a forbidden op never enters the cache at all, so the
                    # hit branch above never needs to ask again.
                    self._check_lock_write(fn, ex)
                    ic[pc] = (fn, a1, a2, ex, nxt, wg)
                    pc = fn(a1, a2, ex, nxt, aether)
                else:
                    pc, fv = self._fault_redirect(pc, fv, aether)
                    if pc is None:
                        break
            return steps

        # Uncached path: no-cache (safe for self-modifying graph construction
        # -- also LOAD_MAIN's own real production path, no_cache=True,
        # max_steps=200_000_000) and progress reporting (debug) both land
        # here, whether either, both, or (via use_cache=False with neither
        # set) plain step-unlimited no-cache execution. One loop body with
        # two independent, cheap per-iteration checks, not two near-
        # duplicate copies of it differing only in which one was inline
        # (previously a separate "progress path" branch whose only real
        # difference from the no-cache branch above was the print
        # statement -- _run_traced already handles its own progress_every
        # this way, a plain `if` inside one loop, not a second branch; this
        # also fixes progress_every silently ignoring max_steps, which the
        # old separate progress branch never checked at all).
        last_rep = 0
        while pc:
            if _max_steps or progress_every:
                steps += 1
                if _max_steps and steps > _max_steps:
                    break
                if progress_every and steps - last_rep >= progress_every:
                    last_rep = steps
                    name = sym.get(pc, f"#{pc}") if sym else f"#{pc}"
                    print(f"  {steps:,} steps  pc={name}", file=sys.stderr)
            op_id = aether[pc + SLOT_OP]
            a1    = aether[pc + SLOT_E1]
            a2    = aether[pc + SLOT_E2]
            ex    = aether[pc + SLOT_EXIT]
            _raw  = aether[pc + SLOT_NEXT]
            if _raw == 0:
                _pad = aether[pc + SLOT_PAD]
                nxt = pc + NOVA_SIZE if _pad == 0 else self._nova_extra_skip(pc + SLOT_PAD, aether)
            elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
            else: nxt = _raw
            if op_id == OP_KEY:
                # use_cache (not False, not True) -- this branch never
                # consults the outer icache regardless (progress_every
                # alone routes here even when the caller asked for
                # caching), but _key_shape's own validity tracks the same
                # thing use_cache always tracked: is the graph safe from
                # being rewritten between visits, not merely "is the
                # icache being consulted for opcode lookup right now".
                pc = self._exec_key(pc, a1, a2, ex, nxt, aether, use_cache)
                continue
            fn = dispatch.get(op_id)
            if fn is not None:
                self._check_lock_write(fn, ex)
                pc = fn(a1, a2, ex, nxt, aether)
            else:
                pc, fv = self._fault_redirect(pc, fv, aether)
                if pc is None:
                    break
        return steps

    def _run_traced(self, start: int, progress_every: int, sym,
                    use_cache: bool = True) -> int:
        """Traced loop — writes pc to trace buffer each step.
        Separate from _run so the hot untraced path has zero branch overhead
        (tracing itself is a per-instruction cost -- tp/trace-buffer checks
        every step -- that path already pays for; one more cheap `if
        use_cache` alongside that doesn't change what kind of path this is).

        use_cache=False (see execute_aether): required whenever the graph
        itself may be modified during this run (self-modifying graph
        construction, LOAD_MAIN) -- exactly the same correctness reason
        _run's own no-cache branch exists. Previously execute_aether had no
        way to ask for this while tracing was active: it looked only at
        whether tracing was on and always routed here regardless, and this
        method always cached unconditionally -- so a caller's no_cache=True
        was silently dropped the moment tracing happened to be enabled at
        the same time. Both are real, independent reasons to pick a code
        path; neither should silently override the other.
        """
        aether    = self.aether.aether
        dispatch  = self._dispatch
        OP_KEY    = self.R.get("Key", 0)
        SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, SLOT_PAD, NOVA_SIZE = self._slot_constants()
        tpc       = self._trace_pos_lux
        # Trace buffer bounds: disable tracing when buffer full (no overwrite).
        _R = self.R
        _iris_buf_lux  = _R.get("RA_IRIS_BUF",  0)
        _iris_size_lux = _R.get("RA_IRIS_SIZE", 0)
        _aether_now = self.aether.aether
        _trace_end = (_aether_now[_iris_buf_lux] + _aether_now[_iris_size_lux]
                      if tpc and _iris_buf_lux and _iris_size_lux else 0)
        pc        = start
        steps     = 0
        last_rep  = 0
        if use_cache:
            self._icache.clear()  # fresh per execution
        icache    = self._icache
        # Dynamic flux_bottom -- correct even if Aether was created with custom
        # size. Was missing entirely: this method crashed with NameError on
        # the first instruction whose NEXT field was non-zero, since _FLUX_
        # BOTTOM below was silently relying on _run's own local variable of
        # the same name, which isn't visible here (separate method).
        _FLUX_BOTTOM = self.aether.flux_bottom

        while pc:
            steps += 1
            if progress_every and steps - last_rep >= progress_every:
                last_rep = steps
                name = sym.get(pc, f"#{pc}") if sym else f"#{pc}"
                print(f"  {steps:,} steps  pc={name}", file=sys.stderr)
            tp = aether[tpc] if tpc else 0
            if tp and (not _trace_end or tp < _trace_end):
                aether[tp] = pc
                aether[tpc] = tp + 1
            elif tp and _trace_end and tp >= _trace_end:
                aether[tpc] = 0  # disable tracing: buffer full
            if use_cache:
                cached = icache.get(pc)
                if cached and cached[5] == self._write_gen:
                    # No lock re-check here -- see _run's matching hit
                    # branch: already verified once, at the pc's first
                    # insert into icache below, and ex can't change while
                    # this pc stays cached.
                    pc = cached[0](cached[1], cached[2], cached[3], cached[4], aether)
                    continue
            op_id = aether[pc + SLOT_OP]
            a1    = aether[pc + SLOT_E1]
            a2    = aether[pc + SLOT_E2]
            ex    = aether[pc + SLOT_EXIT]
            _raw  = aether[pc + SLOT_NEXT]
            if _raw == 0:
                _pad = aether[pc + SLOT_PAD]
                nxt = pc + NOVA_SIZE if _pad == 0 else self._nova_extra_skip(pc + SLOT_PAD, aether)
            elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
            else: nxt = _raw
            if op_id == OP_KEY:
                # Not cached in icache -- see _run's matching branch: the
                # resolved handler always bumps _write_gen, so a cached
                # entry would be stale on the very next check regardless.
                # Which shape to run is memoized separately, inside
                # _exec_key itself.
                pc = self._exec_key(pc, a1, a2, ex, nxt, aether, use_cache)
                continue
            fn = dispatch.get(op_id)
            if fn is not None:
                # Checked before caching (when caching at all) -- see
                # _run's matching miss branch for why the order matters:
                # a forbidden op must never end up in the cache.
                self._check_lock_write(fn, ex)
                if use_cache:
                    icache[pc] = (fn, a1, a2, ex, nxt, self._write_gen)
                pc = fn(a1, a2, ex, nxt, aether)
                continue
            pc, _fv = self._fault_redirect(pc, self._fault_vector, aether)
            if pc is None:
                break
        return steps

    def _slot_constants(self):
        """(SLOT_OP, SLOT_E1, SLOT_E2, SLOT_EXIT, SLOT_NEXT, SLOT_PAD,
        NOVA_SIZE), as a tuple. Was six duplicated lines, identical, in
        both _run and _run_traced -- called once per _run/_run_traced
        invocation, not per instruction, so this costs nothing in the
        hot loop itself.
        """
        return (self._slot_op, self._slot_e1, self._slot_e2,
                self._slot_exit, self._slot_next, self._slot_pad,
                self._nova_size)

    def _fault_redirect(self, pc: int, fv: int, aether):
        """dispatch.get(op_id) missed -- op_id isn't a registered op, an
        error/edge case in practice (every real instruction has a valid
        op), never the common case. Shared by all three loops (_run's
        cached and uncached bodies, _run_traced) purely because it's off
        the hot path already -- a function call here costs nothing that
        wasn't already being paid by reaching an unresolved op_id.

        Returns (new_pc, new_fv). new_pc is None when the loop should
        break instead of continue. Takes fv (not self._fault_vector)
        because callers differ in whether they let the fault vector fire
        once per run (a local fv, zeroed after use, matching _run's own
        cached/uncached bodies) or fresh every miss (re-reading
        self._fault_vector each time, matching _run_traced) -- this
        doesn't decide that, just what happens once a candidate fv is in
        hand.
        """
        if fv and aether[fv]:
            self.pc = pc
            return aether[fv], 0
        return None, fv

    def _nova_extra_skip(self, pad_pos: int, aether) -> int:
        """Fall-through target for a NOVA whose pad slot is non-zero --
        extra lumen pairs (rel, target), 0-terminated, start right at
        pad_pos (SLOT_PAD; see constants.re's NOVA lux slot offsets and
        aether.py's SLOT_PAD). Scans past them arithmetically -- this
        is still the "line" path, never a jump -- to find where the
        real next instruction begins.

        Only called when SLOT_NEXT==0 AND the pad slot is non-zero: the
        rare case. The common case (pad==0) never reaches this method
        at all -- it's resolved inline at the call site as a single
        comparison, same cost as before this existed.
        """
        scan = pad_pos
        while aether[scan] != 0:
            scan += 2
        return scan + 1

    def _resolve_key_shape(self, pc: int, aether):
        """Which write-shape handler this Key instruction means: scan
        pc's own extra lumina (the pad+ region, same convention as any
        other NOVA extra-lumen) for rel=Op1; its target names the shape.
        No Op1 present -> Key's own default, Write's shape (double
        indirection via El1) -- the double-deref K_ pattern this
        project's LOCK'd names actually use.

        Called once per pc by _exec_key, not once per execution -- see
        its own note. Raises KeyShapeError on an Op1 target this
        interpreter doesn't have a handler for, rather than silently
        falling back to Write's shape: a typo here should surface at
        first use, not as a mysteriously-wrong write.
        """
        SLOT_PAD = self._slot_pad
        scan = pc + SLOT_PAD
        if aether[scan] == 0:
            return self._key
        op1_addr = self.R.get('Op1', 0)
        while aether[scan] != 0:
            if aether[scan] == op1_addr:
                target = aether[scan + 1]
                if target == self.R.get('Move', 0):
                    return self._key_move
                raise KeyShapeError(
                    f"Key at {pc}: Op1 names an unrecognized write shape "
                    f"(target address {target}) -- no handler registered for it")
            scan += 2
        return self._key

    def _exec_key(self, pc: int, a1: int, a2: int, ex: int, nxt: int,
                  aether, use_cache: bool) -> int:
        """Dispatch a Key instruction: resolve which write-shape handler
        pc means, then always call it -- Key is a real write with a real
        side effect, exactly like Write, and must re-run every time; only
        WHICH shape to run is a fixed, per-pc property, worth memoizing.

        use_cache=True (the cached loop only): consult/populate
        _key_shape, so repeat visits to the same pc skip the extra-lumen
        scan entirely -- the same "resolve once, reuse forever" principle
        _check_lock_write's own cached-path shortcut relies on, and for
        the same reason: Op1's target can't change for a pc that stays
        cached (unrewritten). use_cache=False (uncached/self-modifying
        graph construction): resolve fresh every call, deliberately --
        nothing about a pc is assumed stable there, Op1 included.
        """
        if use_cache:
            fn = self._key_shape.get(pc)
            if fn is None:
                fn = self._resolve_key_shape(pc, aether)
                self._key_shape[pc] = fn
        else:
            fn = self._resolve_key_shape(pc, aether)
        return fn(a1, a2, ex, nxt, aether)

    def _check_lock_write(self, fn, ex) -> None:
        """Refuse if `fn` (about to be called as this instruction's
        handler) would write to a LOCK-protected address via the exit
        operand. Not per-opcode-method -- one place this logic lives, not
        several hand-copied checks.

        ex is a fixed property of a given pc for as long as it stays
        cached (unrewritten) -- so on the cached paths (both _run and
        _run_traced) this only actually runs once per DISTINCT pc, right
        before that pc's dispatch tuple is first inserted into icache, not
        once per execution of it. The uncached paths call it every time
        instead, because there nothing about ex is guaranteed to stay the
        same between two executions of the same pc -- that's what "the
        graph might be rewritten between calls" means.

        Write is exempt here: its real target is aether[a1], computed
        inside its own method body, not the exit operand -- it carries
        its own guard for that reason (see _write's own note). Key never
        even reaches this function at all, in any of the three loops --
        it's special-cased at op_id == OP_KEY, same as Write, and routed
        through _exec_key instead; dispatch via Key IS the authorization,
        with nothing left here to check by function identity anymore.

        Cheap for the overwhelming common case (no LOCK'd names at all,
        or `ex` falsy): short-circuits before touching _lock_ranges.
        """
        if not ex or not self._lock_ranges or fn is self._write:
            return
        for lo, hi in self._lock_ranges:
            if lo <= ex < hi:
                raise LockViolation(
                    f"write attempted to LOCK-protected address {ex} "
                    f"(range [{lo}, {hi})) via a non-Key opcode")

    # ── Aspect handlers ───────────────────────────────────────────────────────
    # Convention: aether[addr] = word of Data Lux at addr.
    # Instruction operands (a1, a2, tgt, dest, nxt) are Data Lux addresses.
    # handlers read/write aether[operand_addr + 0] = word.

    def _read(self, a1, a2, exit, nxt, aether):
        # Read: exit.word = aether[a1.word]  (double indirection through a1's word)
        if exit and a1: aether[exit] = aether[aether[a1]]
        return nxt

    def _write(self, a1, a2, exit, nxt, aether):
        # Write: aether[a1.lux] = a2.lux
        # Increment _write_gen so any cached entries are invalidated on next hit check.
        # LOCK guard lives here, not at the call site (unlike every other
        # opcode): Write's real target is aether[a1], computed here, not
        # the exit operand -- the call-site check (which only looks at
        # exit) can't see it. Key (below) is identical except for this
        # check -- being dispatched via Key, not Write, is what makes a
        # write authorized.
        if a1 and a2:
            target = aether[a1]
            for lo, hi in self._lock_ranges:
                if lo <= target < hi:
                    raise LockViolation(
                        f"Write attempted to LOCK-protected address {target} "
                        f"(range [{lo}, {hi})) -- only Key may write there")
            aether[target] = aether[a2]
            self._write_gen += 1
        return nxt

    def _key(self, a1, a2, exit, nxt, aether):
        # Key: identical to Write, but no guard at all -- see relations.re's
        # own note and the dispatch-table registration above. Being
        # dispatched via this opcode, at the one specific declared call
        # site that legitimately needs to write to LOCK-protected memory,
        # IS the authorization.
        if a1 and a2:
            aether[aether[a1]] = aether[a2]
            self._write_gen += 1
        return nxt

    def _add(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (aether[a1] + aether[a2]) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _sub(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (_s64(aether[a1]) - _s64(aether[a2])) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _mul(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (_s64(aether[a1]) * _s64(aether[a2])) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _div(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = _trunc_div_s(_s64(aether[a1]), _s64(aether[a2])) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _mod(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = _trunc_mod_s(_s64(aether[a1]), _s64(aether[a2])) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _and(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (aether[a1] & aether[a2]) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _or(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (aether[a1] | aether[a2]) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _xor(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (aether[a1] ^ aether[a2]) & _MASK64 if (a1 and a2) else 0
        return nxt

    def _not(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = (aether[a1] ^ _MASK64) & _MASK64 if a1 else _MASK64
        return nxt

    def _shl(self, a1, a2, exit, nxt, aether):
        if exit:
            s = int(aether[a2]) & 63 if a2 else 0
            aether[exit] = (aether[a1] << s) & _MASK64 if a1 else 0
        return nxt

    def _shr(self, a1, a2, exit, nxt, aether):
        if exit:
            s = int(aether[a2]) & 63 if a2 else 0
            aether[exit] = (_u64(aether[a1]) >> s) & _MASK64 if a1 else 0
        return nxt

    def _shra(self, a1, a2, exit, nxt, aether):
        if exit:
            s = int(aether[a2]) & 63 if a2 else 0
            aether[exit] = (_s64(aether[a1]) >> s) & _MASK64 if a1 else 0
        return nxt

    def _divu(self, a1, a2, exit, nxt, aether):
        if exit:
            b = _u64(aether[a2]) if a2 else 0
            aether[exit] = (_u64(aether[a1]) // b) & _MASK64 if b else 0
        return nxt

    def _modu(self, a1, a2, exit, nxt, aether):
        if exit:
            b = _u64(aether[a2]) if a2 else 0
            aether[exit] = (_u64(aether[a1]) % b) & _MASK64 if b else 0
        return nxt

    def _cmpltu(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _u64(aether[a1]) < _u64(aether[a2])) else 0
        return nxt

    def _cmpeq(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and aether[a1] == aether[a2]) else 0
        return nxt

    def _cmplt(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _s64(aether[a1]) < _s64(aether[a2])) else 0
        return nxt

    def _cmpgt(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _s64(aether[a1]) > _s64(aether[a2])) else 0
        return nxt

    def _cmpgtu(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _u64(aether[a1]) > _u64(aether[a2])) else 0
        return nxt

    def _cmpne(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and aether[a1] != aether[a2]) else 0
        return nxt

    def _cmple(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _s64(aether[a1]) <= _s64(aether[a2])) else 0
        return nxt

    def _cmpleu(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _u64(aether[a1]) <= _u64(aether[a2])) else 0
        return nxt

    def _cmpge(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _s64(aether[a1]) >= _s64(aether[a2])) else 0
        return nxt

    def _cmpgeu(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = 1 if (a1 and a2 and _u64(aether[a1]) >= _u64(aether[a2])) else 0
        return nxt

    def _jumpif(self, a1, a2, exit, nxt, aether):
        return exit if (a1 and aether[a1]) else nxt

    def _jumpreg(self, a1, a2, exit, nxt, aether):
        # JumpReg: pc = word(a1). Used for Redi (a1=RA_LINK).
        return aether[a1] if a1 else 0

    def _jump(self, a1, a2, exit, nxt, aether):
        return exit

    def _move(self, a1, a2, exit, nxt, aether):
        if exit: aether[exit] = aether[a1] if a1 else 0
        return nxt

    def _key_move(self, a1, a2, exit, nxt, aether):
        # Move's write shape (single indirection via Exit), authorized --
        # resolved here by _resolve_key_shape for a Key instruction whose
        # own Op1 extra-lumen names Move. Unlike plain _move, this bumps
        # _write_gen: Key(-shaped) writes target a dynamically-meaningful
        # cell the icache generation scheme has to know changed, exactly
        # like _key/_write do and ordinary _move deliberately doesn't
        # (_move's own target is a fixed operand slot, not the kind of
        # write self-modifying-graph invalidation guards against) --
        # omitting this was a real bug, a locked cell's own value could
        # change here without any cached read of it ever invalidating.
        if exit:
            aether[exit] = aether[a1] if a1 else 0
            self._write_gen += 1
        return nxt

    def _idem(self, a1, a2, exit, nxt, aether):
        # Idem(a) = a, verbatim — zero dereferences (contrast with _move's
        # own single dereference just above). a1 is already the operand's
        # real, resolved address (every operand always is, by the time it
        # reaches here) -- write it directly, don't read through it.
        if exit: aether[exit] = a1 if a1 else 0
        return nxt

    def _exec_flux(self, addr: int, aether) -> int:
        """Execute a flux at addr.

        Flux structure (single lux -- no separate type lux):
          slot 0: descriptor -- a sequence of 4-bit digits (nibbles),
                  unpacked from the least-significant nibble upward,
                  terminated by a 0 digit. Up to 16 digits fit in one
                  64-bit word; real fluxes today need far fewer than
                  that, so digit 15 ("descriptor continues in the next
                  slot") is reserved but not yet implemented.
          slot 1..: the actual values, consumed in the order the
                  descriptor calls for them.

        Digit meanings:
          0        = end of descriptor
          1-7      = a code (FLUX_SLOT_WORD..FLUX_SLOT_REL -- same
                     meaning as the old type-lux codes)
          13       = PIVOT, only meaningful inside a group -- separates
                     the group's code(s) (before) from its count digit
                     (after)
          14       = GROUP boundary -- toggles open/close by occurrence
                     parity (1st 14 in the descriptor opens, 2nd closes,
                     3rd opens the next group, ...). Always written in
                     pairs, even when a group starts at digit 0: omitting
                     the opener would make every later 14 ambiguous
                     between "opens a new group" and "closes the implicit
                     one that silently started at 0" -- parity is the
                     only thing that disambiguates 14 at all, so it
                     cannot be skipped anywhere.
          8-12, 15 = reserved

        A group (14 code(s)... 13 count 14) is one of two shapes, told
        apart by how many codes sit before the pivot:
          - one code before 13, count N after  -> "one-to-many": this
            single code has N separate values, each consumed in turn
            (e.g. several Next targets that are genuinely different
            places -- ordinary fan-out, just written without repeating
            the code digit N times).
          - several distinct codes before 13, count N after ->
            "many-to-one": those codes all share the same N value(s)
            (N is 1 in the overwhelmingly common case -- several
            different relations that happen to converge on one target).
        Outside any group, a plain code digit consumes exactly one
        value, same as the old type-lux's 1:1 code-to-value mapping.

        Dispatches op (if present), collects all Next addrs into
        _pending_next. Returns first Next addr (or 0 if none). Future
        Aria scheduler uses _pending_next for parallel fan-out via
        Exire(clone).
        """
        descriptor = aether[addr]
        if not descriptor:
            return 0  # no descriptor -> nothing to do

        FLUX_SLOT_OP   = 2
        FLUX_SLOT_E1   = 3
        FLUX_SLOT_E2   = 4
        FLUX_SLOT_EXIT = 5
        FLUX_SLOT_NEXT = 6
        FLUX_SLOT_REL  = 7
        PIVOT = 13
        GROUP = 14

        # Unpack the descriptor into its digit sequence (LSB nibble
        # first), stopping at the first 0 digit.
        digits = []
        d = descriptor
        for _ in range(16):
            nib = d & 0xF
            if nib == 0:
                break
            digits.append(nib)
            d >>= 4

        op = 0; e1 = 0; e2 = 0; exit_v = 0
        nexts: list = []
        rels: list = []

        def _store(code, val):
            nonlocal op, e1, e2, exit_v
            if   code == FLUX_SLOT_OP:   op     = val
            elif code == FLUX_SLOT_E1:   e1     = val
            elif code == FLUX_SLOT_E2:   e2     = val
            elif code == FLUX_SLOT_EXIT: exit_v = val
            elif code == FLUX_SLOT_NEXT: nexts.append(val)
            elif code == FLUX_SLOT_REL:  rels.append(val)

        val_pos = addr + 1
        i = 0
        n = len(digits)
        while i < n:
            nib = digits[i]
            if nib == GROUP:
                i += 1
                left_codes = []
                while i < n and digits[i] != PIVOT:
                    left_codes.append(digits[i])
                    i += 1
                if i >= n:
                    break  # malformed: no pivot -- stop, don't misread
                i += 1  # skip PIVOT
                if i >= n:
                    break  # malformed: no count digit
                count = digits[i]
                i += 1
                if i >= n or digits[i] != GROUP:
                    break  # malformed: missing closing GROUP marker
                i += 1  # skip closing GROUP

                if len(left_codes) == 1:
                    # one-to-many: this one code gets `count` separate values
                    code = left_codes[0]
                    for _ in range(count):
                        _store(code, aether[val_pos]); val_pos += 1
                else:
                    # many-to-one: all these codes share the same value(s)
                    # (count is 1 in the common case).
                    shared = 0
                    for _ in range(max(count, 1)):
                        shared = aether[val_pos]; val_pos += 1
                    for code in left_codes:
                        _store(code, shared)
                continue
            # plain, ungrouped code -- consumes exactly one value
            _store(nib, aether[val_pos])
            val_pos += 1
            i += 1

        # Store all next addrs for Aria scheduler (future use)
        self._pending_next = nexts

        # Dispatch op if present
        if op:
            first_next = nexts[0] if nexts else 0
            fn = self._dispatch.get(op)
            if fn is not None:
                return fn(e1, e2, exit_v, first_next, aether)

        # No op: just return first next
        return nexts[0] if nexts else 0

    def _end(self, a1, a2, exit, nxt, aether):
        return 0

    def _voca(self, a1, a2, exit, nxt, aether):
        # Voca El1=target Exit=ra_link: save nxt into aether[exit], jump to aether[a1]
        # If exit is RA_LINK, this is a call: push the OLD RA_LINK onto the
        # automatic call stack (RA_SP) before overwriting it, so nested calls
        # can return through multiple levels correctly.
        if exit:
            if exit == self._ra_link and self._ra_sp:
                sp = aether[self._ra_sp] - self._ra_frame_size
                aether[self._ra_sp] = sp
                aether[sp] = aether[exit]
            aether[exit] = nxt
        return aether[a1] if a1 else 0

    def _redi(self, a1, a2, exit, nxt, aether):
        # Redi El1=ra_link: jump to aether[a1] (ra_link passed explicitly)
        # If a1 is RA_LINK, this is a return: after jumping, pop the automatic
        # call stack back into RA_LINK, restoring the caller's return address.
        target = aether[a1] if a1 else 0
        if a1 == self._ra_link and self._ra_sp:
            sp = aether[self._ra_sp]
            aether[a1] = aether[sp]
            aether[self._ra_sp] = sp + self._ra_frame_size
        return target

    def _syscall(self, a1, a2, exit, nxt, aether):
        nr  = aether[self._sc_nr]  if self._sc_nr  else 0
        a0v = aether[self._sc_a0]  if self._sc_a0  else 0
        a1v = aether[self._sc_a1]  if self._sc_a1  else 0
        a2v = aether[self._sc_a2]  if self._sc_a2  else 0
        ret = self._do_syscall(int(nr), int(a0v), int(a1v), int(a2v), aether)
        if self._sc_a0: aether[self._sc_a0] = ret & _MASK64
        return nxt

    def _do_syscall(self, nr, a0, a1, a2, aether):
        if self._block_syscalls:
            return 0  # silently block all I/O during Phase-B macro execution
        sw = self._sys_write; swp = self._sys_write_packed
        sr = self._sys_read
        so = self._sys_openat; sc = self._sys_close; sx = self._sys_exit
        if nr == sw:
            try:
                # 1 byte per lux — read low byte of each u64.
                buf = bytearray(a2)
                for i in range(a2): buf[i] = aether[a1 + i] & 0xFF
                return os.write(a0, bytes(buf))
            except: return -1
        if nr == swp:
            try:
                # Packed: 8 bytes per lux, little-endian u64.
                return os.write(a0, bytes(_read_packed_bytes(aether, a1, a2)))
            except: return -1
        if nr == sr:
            try:
                data = os.read(a0, a2)
                for i, b in enumerate(data): aether[a1 + i] = b
                return len(data)
            except: return -1
        if nr == so:
            try:
                # Path is a packed null-terminated string: 8 bytes per lux.
                pb = _read_packed_cstr(aether, a1)
                return os.open(pb.decode('utf-8', 'replace'), a2,
                               stat.S_IRUSR | stat.S_IWUSR)
            except: return -1
        if nr == sc:
            try: os.close(a0); return 0
            except: return -1
        if nr == sx:
            sys.exit(a0 & 0xFF)
        return 0
