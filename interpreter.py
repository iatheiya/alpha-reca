"""
interpreter.py — Reca execution engine

ITO lux compact layout (fixed slots, direct-index):
  slot 0  = word   = self-ref (aether[addr] = addr; Voca uses this)
  slot 1  = op     = primitive addr (dispatch key)
  slot 2  = e1     = first operand
  slot 3  = e2     = second operand
  slot 4  = exit   = destination (store target / jump target / RA_LINK addr for Voca)
  slot 5  = next   = 0 → implicit fall-through (pc+ITO_SIZE); !0 → explicit graph link
  slot 6  = pad    = 0 (terminates lumen scan before extra lumens)
  slot 7+ = extra LINK lumens: (rel_addr, tgt_addr) pairs, 0-terminated

Data lux layout:
  slot 0  = word   = value
  slot 1+ = lumen pairs (rel_addr, tgt_addr), 0-terminated

Key invariant: aether[ito_lux] = ito_lux (self-ref)
  -> Voca El1=func reads aether[func] = func -> jumps to func directly.

Three next-pc paths (checked in this order every step):
  line  — SLOT_NEXT == 0          → nxt = pc + ITO_SIZE (sequential, prefetcher-friendly)
  warp  — 0 < SLOT_NEXT < FLUX_BOTTOM → nxt = aether[SLOT_NEXT] (explicit address jump)
  flux  — SLOT_NEXT >= FLUX_BOTTOM → nxt = _exec_flux(...) (structured parallel branch)

Handlers signature: (a1, a2, exit, nxt, aether) -> next_pc
"""
import os
import sys
import stat

from symphony import Aether, XLEN, _MASK64


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
        # ITO layout slots — read from aether via update_relations (defined in constants.re)
        "_slot_op", "_slot_e1", "_slot_e2", "_slot_exit", "_slot_next", "_ito_size",
    )

    def __init__(self) -> None:
        self.aether        = Aether()
        self.R: dict         = {}   # name → address (filled by loader/update_relations)
        self._dispatch: dict = {}
        # ITO layout slots with defaults (overridden by update_relations when constants.re loaded)
        self._slot_op   = 1
        self._slot_e1   = 2
        self._slot_e2   = 3
        self._slot_exit = 4
        self._slot_next = 5
        self._ito_size  = 7
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

        # ITO layout slots — read from aether (defined in constants.re as NEWSET SLOT_*).
        # Not hardcoded: Aria owns layout, Python only reads what Aria declared.
        def _slot(name, default):
            lux = R.get(name, 0)
            return int(a[lux]) if lux else default
        self._slot_op   = _slot("SLOT_OP",   1)
        self._slot_e1   = _slot("SLOT_E1",   2)
        self._slot_e2   = _slot("SLOT_E2",   3)
        self._slot_exit = _slot("SLOT_EXIT", 4)
        self._slot_next = _slot("SLOT_NEXT", 5)
        self._ito_size  = _slot("ITO_SIZE",  7)

        # Build dispatch: op_id → handler fn.
        # op_id is the address of the Lux (aether[lux_addr] = lux_addr, self-ref).

        # ── 24 Native Aspects (aspects.re) ────────────────────────────────────
        # These map 1:1 to CPU instructions. Irreducible from each other.
        # Changing this set redefines Reca.
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

        self._dispatch = {}
        for name, fn in {**_native, **_derived}.items():
            addr = R.get(name)
            if addr:
                self._dispatch[addr] = fn

        # Trace: K_TRACE_POS is an aria symbol. Its word is the address of the
        # lux holding the current trace position (0 = tracing off).
        # iris.re writes to aether[K_TRACE_POS.word] to enable/disable.
        # The double indirection matches the existing convention in iris.re/shioreru.re.
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
        """
        tpc = self._trace_pos_lux
        if tpc and self.aether.aether[tpc]:
            return self._run_traced(start, progress_every, sym)
        return self._run(start, progress_every, sym, use_cache=not no_cache,
                         max_steps=max_steps)

    def _run(self, start: int, progress_every: int, sym,
             use_cache: bool = True, max_steps: int = 0) -> int:
        """Untraced execution loop — hot ops inlined.

        use_cache=False disables icache for graph-building contexts (LOAD_MAIN).
        use_cache=True (default) enables icache for repeated Phase B macro calls.
        """
        aether    = self.aether.aether
        dispatch  = self._dispatch
        pc        = start
        steps     = 0
        _max_steps = max_steps  # 0 = unlimited
        SLOT_OP   = self._slot_op
        SLOT_E1   = self._slot_e1
        SLOT_E2   = self._slot_e2
        SLOT_EXIT = self._slot_exit
        SLOT_NEXT = self._slot_next
        ITO_SIZE  = self._ito_size

        R = self.R
        # Dynamic flux_bottom — correct even if Aether was created with custom size
        _FLUX_BOTTOM = self.aether.flux_bottom
        OP_MOVE   = R.get("Move",   -1)
        OP_ADD    = R.get("Add",    -2)
        OP_EQUAL  = R.get("Equal",  -3)
        OP_JUMPIF = R.get("JumpIf", -4)
        OP_JUMP   = R.get("Jump",   -5)
        OP_READ   = R.get("Read",   -6)
        OP_WRITE  = R.get("Write",  -7)
        OP_VOCA   = R.get("Voca",   -8)
        OP_REDI   = R.get("Redi",   -9)
        OP_END    = R.get("End",    -10)

        fv = self._fault_vector

        if _max_steps and not progress_every:
            # Fast path with step limit
            while pc:
                steps += 1
                if steps > _max_steps:
                    break
                op_id = aether[pc + SLOT_OP]
                e1    = aether[pc + SLOT_E1]
                e2    = aether[pc + SLOT_E2]
                ex    = aether[pc + SLOT_EXIT]
                _raw  = aether[pc + SLOT_NEXT]
                if _raw == 0: nxt = pc + ITO_SIZE
                elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
                else: nxt = _raw
                handler = dispatch.get(op_id)
                if handler is None:
                    break
                pc = handler(e1, e2, ex, nxt, aether)
            return steps
        if not progress_every:
            if use_cache:
                ic = self._icache
                # Generation-counter icache: each entry stores (fn,a1,a2,ex,nxt,gen).
                # Write increments _write_gen (O(1)) instead of scanning a range.
                # Cache hit with stale gen → re-execute and re-cache. O(1) amortised.
                wg = self._write_gen
                while pc:
                    cached = ic.get(pc)
                    if cached and cached[5] == wg:
                        pc = cached[0](cached[1], cached[2], cached[3], cached[4], aether)
                        continue
                    op_id = aether[pc + SLOT_OP]
                    a1    = aether[pc + SLOT_E1]
                    a2    = aether[pc + SLOT_E2]
                    ex    = aether[pc + SLOT_EXIT]
                    _raw  = aether[pc + SLOT_NEXT]
                    if _raw == 0: nxt = pc + ITO_SIZE
                    elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
                    else: nxt = _raw
                    if op_id == OP_MOVE:
                        if ex: aether[ex] = aether[a1] if a1 else 0
                        ic[pc] = (self._move, a1, a2, ex, nxt, wg); pc = nxt
                    elif op_id == OP_ADD:
                        if ex: aether[ex] = (aether[a1] + aether[a2]) & _MASK64 if (a1 and a2) else 0
                        ic[pc] = (self._add, a1, a2, ex, nxt, wg); pc = nxt
                    elif op_id == OP_EQUAL:
                        if ex: aether[ex] = 1 if (a1 and a2 and aether[a1] == aether[a2]) else 0
                        ic[pc] = (self._cmpeq, a1, a2, ex, nxt, wg); pc = nxt
                    elif op_id == OP_JUMPIF:
                        pc = ex if (a1 and aether[a1]) else nxt
                    elif op_id == OP_READ:
                        if ex and a1: aether[ex] = aether[aether[a1]]
                        pc = nxt
                    elif op_id == OP_WRITE:
                        if a1 and a2:
                            aether[aether[a1]] = aether[a2]
                            wg += 1
                            self._write_gen = wg
                        pc = nxt
                    elif op_id == OP_JUMP:
                        pc = ex
                    elif op_id == OP_VOCA:
                        if ex:
                            if ex == self._ra_link and self._ra_sp:
                                sp = aether[self._ra_sp] - self._ra_frame_size
                                aether[self._ra_sp] = sp
                                aether[sp] = aether[ex]
                            aether[ex] = nxt
                        pc = aether[a1] if a1 else 0
                    elif op_id == OP_REDI:
                        pc = aether[a1] if a1 else 0
                        if a1 == self._ra_link and self._ra_sp:
                            ssp = aether[self._ra_sp]
                            aether[a1] = aether[ssp]
                            aether[self._ra_sp] = ssp + self._ra_frame_size
                    elif op_id == OP_END:
                        pc = 0
                    else:
                        fn = dispatch.get(op_id)
                        if fn is not None:
                            ic[pc] = (fn, a1, a2, ex, nxt, wg)
                            pc = fn(a1, a2, ex, nxt, aether)
                        elif fv and aether[fv]:
                            self.pc = pc; pc = aether[fv]; fv = 0
                        else:
                            break
            else:
                # No-cache path: safe for self-modifying graph construction.
                while pc:
                    op_id = aether[pc + SLOT_OP]
                    a1    = aether[pc + SLOT_E1]
                    a2    = aether[pc + SLOT_E2]
                    ex    = aether[pc + SLOT_EXIT]
                    _raw  = aether[pc + SLOT_NEXT]
                    if _raw == 0: nxt = pc + ITO_SIZE
                    elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
                    else: nxt = _raw
                    if op_id == OP_MOVE:
                        if ex: aether[ex] = aether[a1] if a1 else 0
                        pc = nxt
                    elif op_id == OP_ADD:
                        if ex: aether[ex] = (aether[a1] + aether[a2]) & _MASK64 if (a1 and a2) else 0
                        pc = nxt
                    elif op_id == OP_EQUAL:
                        if ex: aether[ex] = 1 if (a1 and a2 and aether[a1] == aether[a2]) else 0
                        pc = nxt
                    elif op_id == OP_JUMPIF:
                        pc = ex if (a1 and aether[a1]) else nxt
                    elif op_id == OP_READ:
                        if ex and a1: aether[ex] = aether[aether[a1]]
                        pc = nxt
                    elif op_id == OP_WRITE:
                        if a1 and a2:
                            aether[aether[a1]] = aether[a2]
                        pc = nxt
                    elif op_id == OP_JUMP:
                        pc = ex
                    elif op_id == OP_VOCA:
                        if ex:
                            if ex == self._ra_link and self._ra_sp:
                                sp = aether[self._ra_sp] - self._ra_frame_size
                                aether[self._ra_sp] = sp
                                aether[sp] = aether[ex]
                            aether[ex] = nxt
                        pc = aether[a1] if a1 else 0
                    elif op_id == OP_REDI:
                        pc = aether[a1] if a1 else 0
                        if a1 == self._ra_link and self._ra_sp:
                            ssp = aether[self._ra_sp]
                            aether[a1] = aether[ssp]
                            aether[self._ra_sp] = ssp + self._ra_frame_size
                    elif op_id == OP_END:
                        pc = 0
                    else:
                        fn = dispatch.get(op_id)
                        if fn is not None:
                            pc = fn(a1, a2, ex, nxt, aether)
                        elif fv and aether[fv]:
                            self.pc = pc; pc = aether[fv]; fv = 0
                        else:
                            break
            return steps

        # Progress path (no cache for simplicity — only used for debugging).
        last_rep = 0
        while pc:
            steps += 1
            if steps - last_rep >= progress_every:
                last_rep = steps
                name = sym.get(pc, f"#{pc}") if sym else f"#{pc}"
                print(f"  {steps:,} steps  pc={name}", file=sys.stderr)
            op_id = aether[pc + SLOT_OP]
            a1    = aether[pc + SLOT_E1]
            a2    = aether[pc + SLOT_E2]
            ex    = aether[pc + SLOT_EXIT]
            _raw  = aether[pc + SLOT_NEXT]
            if _raw == 0: nxt = pc + ITO_SIZE
            elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
            else: nxt = _raw
            if op_id == OP_MOVE:
                if ex: aether[ex] = aether[a1] if a1 else 0
                pc = nxt
            elif op_id == OP_ADD:
                if ex: aether[ex] = (aether[a1] + aether[a2]) & _MASK64 if (a1 and a2) else 0
                pc = nxt
            elif op_id == OP_EQUAL:
                if ex: aether[ex] = 1 if (a1 and a2 and aether[a1] == aether[a2]) else 0
                pc = nxt
            elif op_id == OP_JUMPIF:
                pc = ex if (a1 and aether[a1]) else nxt
            elif op_id == OP_READ:
                if ex and a1: aether[ex] = aether[aether[a1]]
                pc = nxt
            elif op_id == OP_WRITE:
                if a1 and a2:
                    aether[aether[a1]] = aether[a2]
                pc = nxt
            elif op_id == OP_JUMP:
                pc = ex
            elif op_id == OP_VOCA:
                if ex:
                    if ex == self._ra_link and self._ra_sp:
                        sp = aether[self._ra_sp] - self._ra_frame_size
                        aether[self._ra_sp] = sp
                        aether[sp] = aether[ex]
                    aether[ex] = nxt
                pc = aether[a1] if a1 else 0
            elif op_id == OP_REDI:
                pc = aether[a1] if a1 else 0
                if a1 == self._ra_link and self._ra_sp:
                    ssp = aether[self._ra_sp]
                    aether[a1] = aether[ssp]
                    aether[self._ra_sp] = ssp + self._ra_frame_size
            elif op_id == OP_END:
                pc = 0
            else:
                fn = dispatch.get(op_id)
                if fn is not None:
                    pc = fn(a1, a2, ex, nxt, aether)
                elif fv and aether[fv]:
                    self.pc = pc; pc = aether[fv]; fv = 0
                else:
                    break
        return steps

    def _run_traced(self, start: int, progress_every: int, sym) -> int:
        """Traced loop — writes pc to trace buffer each step.
        Separate from _run so the hot untraced path has zero branch overhead.
        """
        aether    = self.aether.aether
        dispatch  = self._dispatch
        SLOT_OP   = self._slot_op
        SLOT_E1   = self._slot_e1
        SLOT_E2   = self._slot_e2
        SLOT_EXIT = self._slot_exit
        SLOT_NEXT = self._slot_next
        ITO_SIZE  = self._ito_size
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
        self._icache.clear()  # fresh per execution
        icache    = self._icache

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
            cached = icache.get(pc)
            if cached and cached[5] == self._write_gen:
                pc = cached[0](cached[1], cached[2], cached[3], cached[4], aether)
                continue
            op_id = aether[pc + SLOT_OP]
            a1    = aether[pc + SLOT_E1]
            a2    = aether[pc + SLOT_E2]
            ex    = aether[pc + SLOT_EXIT]
            _raw  = aether[pc + SLOT_NEXT]
            if _raw == 0: nxt = pc + ITO_SIZE
            elif _raw >= _FLUX_BOTTOM: nxt = self._exec_flux(_raw, aether)
            else: nxt = _raw
            fn = dispatch.get(op_id)
            if fn is not None:
                icache[pc] = (fn, a1, a2, ex, nxt, self._write_gen)
                pc = fn(a1, a2, ex, nxt, aether)
                continue
            fv = self._fault_vector
            if fv and aether[fv]:
                self.pc = pc; pc = aether[fv]; fv = 0; continue
            break
        return steps

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

    def _exec_flux(self, addr: int, aether) -> int:
        """Execute a flux at addr.

        Flux structure:
          slot 0: word = type_lux_addr (points to type lux describing layout)
          slot 1..: slots in order described by type lux codes

        Type lux = [any_word, code1, code2, ..., 0]
        Codes: 1=Word 2=Op 3=El1 4=El2 5=Exit 6=Next 7=Rel

        Reads type lux to understand layout, extracts op/el/exit/next values,
        dispatches op (if present), collects all Next addrs into _pending_next.
        Returns first Next addr (or 0 if none). Future Aria scheduler uses
        _pending_next for parallel fan-out via Exire(clone).
        """
        # Read type lux address from word slot
        type_lux = aether[addr]
        if not type_lux:
            return 0  # no type lux → nothing to do

        # Scan type lux codes starting at pos 1 (skip word)
        type_pos = type_lux + 1
        data_pos = addr + 1  # first slot after word in flux

        op = 0; e1 = 0; e2 = 0; exit_v = 0
        nexts: list = []
        FLUX_SLOT_OP   = 2
        FLUX_SLOT_E1   = 3
        FLUX_SLOT_E2   = 4
        FLUX_SLOT_EXIT = 5
        FLUX_SLOT_NEXT = 6

        while True:
            code = aether[type_pos]
            if not code:
                break
            val = aether[data_pos]
            if   code == FLUX_SLOT_OP:   op     = val
            elif code == FLUX_SLOT_E1:   e1     = val
            elif code == FLUX_SLOT_E2:   e2     = val
            elif code == FLUX_SLOT_EXIT: exit_v = val
            elif code == FLUX_SLOT_NEXT: nexts.append(val)
            type_pos += 1
            data_pos += 1

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
