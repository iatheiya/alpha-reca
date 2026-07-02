# ROADMAP.md — Open Tasks

---

## Active

### A1. Flux — [x] Done

Flux is an Aria convention, not part of the canon — a lux of fully free-form
structure, living in a reserved region of Aether. Unlike ITO or Data (fixed layouts),
a flux has no required slot order, no fixed size, and no fixed meaning per slot.
Any part can be omitted, repeated, or redefined.

Triggered only by an explicit jump (next != 0 AND addr >= FLUX_BOTTOM).
The line path (+7, fall-through) never sees flux — zero overhead for code
that doesn't use it.

**Detection:**
Reserved region in Aether: `FLUX_BOTTOM <= addr < STACK_BOTTOM`.
One comparison on explicit jump. No false positives.

**Three execution paths:**
```
raw = aether[pc + SLOT_NEXT]
if raw == 0:             → nxt = pc + ITO_SIZE  (step — sequential, prefetcher-friendly)
elif raw >= FLUX_BOTTOM: → _exec_flux(raw)       (flux — structured parallel branch)
else:                    → nxt = raw             (warp — explicit address jump)
```

**Type lux:**
Word of a flux points to a type lux (Data lux) that describes its structure —
a sequence of slot codes, 0-terminated. The author defines this sequence freely:
any slot can repeat any number of times, appear in any order, or be absent entirely.
This is what makes flux a superset of both ITO and Data — and strictly more,
since neither order nor presence is fixed.

---

**Future (Aria, built on flux):**
- Thread scheduler (Exire clone for parallel N-next execution)
- Aether synchronisation between threads (futex via Exire)
- Free-list allocator for flux zone (fragmentation)
- Overflow chain for flux (if more slots needed than allocated)

---

## Future

### B1. Yaku: peephole optimisation — partially done

Equal/Less/ULess + JumpIf → fused into `br i1 %v{cmp}` (JumpIfCmp).
Add/Sub/Mul + JumpIf → fused into `icmp ne {arith}, 0 + br` (JumpIfNZ).
Constant folding: NEWSET luces emit `or i{XLEN} 0, val` — ~850+ loads eliminated.
LLVM -O2 folds `or i{XLEN} 0, K` → constant immediately.

**Remaining:**
- Redundant Move elimination (LLVM partially handles this)
- `Add + Write` → indexed store fusion (needs look-ahead, low priority)

**Direction:** `pair_rule_table[op1][op2]` → combined template. 2-lux window in EMIT_BLOCK.

---

### B2. ADD_LUMEN: overflow chain — [x] Done

OVERFLOW_REL + ADD_LUMEN + REMOVE_LUMEN + ALLOC_LUX_N updated.
On overflow: allocate an overflow lux and link via rel=OVERFLOW_REL.
ADD_LUMEN follows the chain automatically. Clean graph model, no Aether violations.

---

### B3. RA_JEQ_FLAG: global flag (threading hazard)

Not a problem now (single-threaded). Recorded for future.

RA_JEQ_FLAG is the single global lux where Equal writes its result.
Safe as long as Equal→JumpIf is always sequential and there are no interrupts.

**When it becomes a problem:** interrupts via _fault_vector or multithreading.

**Solution when needed:**
Move the flag into the stack frame (`RA_SP` points at the current frame;
`RA_FRAME_SIZE=8` luces per frame, slot 1 currently free). Using `RA_SP + 1`
(dynamic address) requires an "indirect exit" mechanism — where Exit= is
interpreted as a pointer to a lux rather than the lux itself.
Requires a new aspect or ITO flag. Non-trivial, deferred.

---

### B4. Flux zone free-list allocator

Flux has variable size → bump allocator creates fragmentation when luces
of different lengths are frequently created and deleted.

**Solution:** free-list allocator for the flux zone. On deallocation — add to free list.
On allocation — find a block of matching size (first-fit or best-fit).
Compact GC as an alternative for long-running programs.

---

### B5. Thread scheduler (Aria)

Depends on: A1 (flux) + Exire/clone infrastructure.

Flux with multiple Next slots enables fan-out.
Scheduler reads flux, sees N Next, launches N threads via Exire(clone).
Aether synchronisation via futex.

This is an Aria, not core. The core only provides structure (flux + N next slots).
What to do with them — the Aria decides.

---

### B6. Self-hosting: LOAD_MAIN as sole source of truth

**Status (updated 2026-06-30): REGRESSED, 14/42 files (33%).** A prior
session's note (see D3 below) claims 42/42 (full self-hosting) was reached
at some point before the 2026-06-24 project snapshot, but a clean `freeze()`
as of 2026-06-30 only gives 14/42 — worse than even the 33/42 (78%) baseline
recorded 2026-06-19 below. **The 42/42 → 14/42 regression is undocumented —
no commit/fix explains it.** Current blocker (`fi=14`, `fmt.re`, a `SWITCH`
statement that never returns control to the file loop after its first case)
is traced in detail in `BUGS.md` → "[!] CURRENT STATUS (updated 2026-06-30)"
at the top of that file — start there, not in the 2026-06-19 section below.

Python loader (Wave-A/B) duplicates LOAD_MAIN. Two sources of truth.
Currently Python loader is authoritative. Self-hosting = LOAD_MAIN becomes authoritative.

**Done in the 2026-06-19 session (see `BUGS.md` for full detail on each):**
- Allocator under-sizing for RVOCA/RREDI/ITO/JZ/JEQ/CLEAR/NOP/ALLOC_TO
  targets (`loader.py`, `_scan_ito_names` prepass)
- Systemic missing-closing-`//` comment bug across `yaku.re` (~37 fixes)
- Dead "OUTER register" pattern replaced with `RCALL_AT` (4 instances)
- `ALLOC_TO` given a Python-side fast-path (was missing one, unlike
  JZ/JEQ/CLEAR/NOP/RCALL_AT)
- **Fundamental fix**: `_resolve()` no longer returns bare integer literals
  as raw values — resolves to a `C_<N>` register address instead, since
  every ITO field is read through a dereference. This was the root cause
  of an infinite loop in `BS_READ_TOKEN`'s EOF detection.
- **Fundamental fix**: hash-table architecture bug — `lexer.re`'s hash
  routines were prematurely masking to the narrow 18-bit table-size mask
  *before* the hash reached `HT_LOOKUP`/`HT_INSERT`, defeating wide-hash
  collision resolution entirely. Fixed in `lexer.re`/`intern.re`/`loader.py`
  together (must stay in sync).
- Two genuine infinite loops fixed (`LOAD_PREPASS_FILE`'s `LOAD_PP_SKIP`/
  `BS_SKIP_TO_EOL` swallowing the next line; `LOAD_EXPAND_TEMPLATE` having
  no reachable termination path)
- A broken auto-chain in `LOAD_CMD_BLOCK` (a `NOLINK` was blocking a
  preceding `JZ`'s own fallthrough, sending execution into unrelated code)
- 5 label-name typos + 1 missing-constant typo (`ASCII_EQ`→`EQUALS`) in
  `lexer.re`

**Current blocker:** a call-stack imbalance (`RA_SP` underflow) while
processing `saku.re`, centred on `RREDI`'s own self-referential generic-
macro definition (`macros.re:86`, `RREDI RREDI_RET`) — but confirmed to be
deeper than just that one line (bypassing it entirely still reproduces the
symptom). See `BUGS.md` for the full hypothesis list; the leading
candidate is that `WIRE_AUTOLINK`/`WIRE_AUTOLINK_RESET`'s manual
save-into-`RA_MC_TMP_RL` pattern may predate, and now double-book against,
the automatic `RA_SP`-based call stack.

A first attempted fix (fast-pathing `RVOCA`/`RREDI` dispatch the same way
`N`/`S`/`L`/`I`/`B` already are) was tried and **made things worse**
(regressed 33/42 → 1/42) — reverted. Do not retry that exact approach
without first following hypothesis 1 in `BUGS.md` (find the *actual* first
point of imbalance via full-run trace, not just the crash site).

**Next step:** see `BUGS.md`'s ranked hypothesis list. Hypothesis 1 (full-run
push/pop diff, not just last-40-events) is the cheapest to try first.


---

## M1 — Native mobile target (no LLVM)

Yaku currently emits LLVM IR, which requires clang/llc at build time.
Goal: Yaku generates ARM64 directly — no LLVM dependency.

Result: Reca programs distribute as a single signed binary.
No JIT, no W^X, no entitlements. Legal everywhere, including App Store.

**Prerequisite:** B6 (self-hosting).

---

## Deferred from session 2026-06-23

### D1. NEWSET integer value unification (Pattern U)

**Prerequisite:** None. Self-contained change.

The integer resolver in `LOAD_RKVR_INT` (saku.re) uses `addr(C_0)+N*2` formula
to find a lux holding value N. This is wrong for sparse C_N (only C_0..C_19
are contiguous; C_32, C_48, C_100, C_4096... have gaps). Formula gives garbage
address for any N not in 0..19.

**Required fix:**
- `saku.re` `LOAD_RKVR_INT`: instead of formula, allocate (or find via htable)
  a 1-slot lux holding the literal value. Match Python `_resolve` contract
  exactly: integer → address of a lux whose word = that integer.
- `macros.re` `NEWSET_START`: add `Read El1=RA_MA1 Exit=RA_MC_SLOT` before
  Write, since MA1 will be a proper address (not raw value*2 as before).
- `loader.py`: audit whether Python pre-alloc must reserve the new luces
  (likely needs `_resolve` calls in the NEWSET pass to match Reca allocation).

**Already done (safe, kept):**
- `RA_BS_PIVAL` register in lexer.re (isolated BS_PARSE_INT from scratch collision)
- `RA_C0_REF` in resolver (was using C_0 value=0 instead of C_0 address)

**Impact when fixed:** PS_MAIN runtime parser works correctly (TAB=9, LF=10).

---

### D2. bound/next universal lux mutability

See `PLAN_BOUND_NEXT.md` for full design. DEFERRED — needs author confirmation
on open questions in that file before implementation.

---

### D3. ROADMAP stale items to update

- ~~B6 still says "33/42" — actual is 42/42. Update to done.~~ **STALE ITSELF
  (caught 2026-06-30):** this note claimed 42/42 was reached, but a clean
  `freeze()` as of 2026-06-30 gives **14/42**. Nobody documented when/why
  this regressed. See BUGS.md "CURRENT STATUS" section (top of file) for
  what's been traced so far. Do not write "42/42 done" anywhere again until
  a clean run actually prints it.
- Pattern P in BUGS.md was "NOT FIXED" — actually closed (see Pattern P update).

---

### D4. Rename ITO and WAVE (runtime ITO creation)

**WAVE is implemented** (loader.py + macros.re). Syntax:
`WAVE label Op [El1=X] [El2=Y] [Exit=Z] [At=A] [Next=N] [Reset=1]`
Covers all builder macros. Only the renaming remains open.

**Two separate naming decisions:**

1. **ITO → ?** — "step" is not quite right; ITO is an autonomous graph node,
   not necessarily sequential. Need a word that conveys
   "autonomous unit of execution". Candidates considered:
   ACT, BEAT, NODE, PULSE, QUANT, CELL, GRAIN, GLYPH, FORM.
   None were satisfying — open question, revisit later.

2. **EMIT_ITO → WAVE (preliminary)** — runtime macro that creates an ITO
   in the graph during execution. Syntax mirrors ITO:
   `WAVE label Op El1=X El2=Y Exit=Z`
   If ITO is renamed — revisit WAVE too.
   WAVE chosen preliminarily; finalize together with point 1.

**Do not implement until names are resolved.**

---

### D5. RCN_IMPL second block — partial WAVE opportunity

RCN_IMPL synthesizes 2 ITOs: first via direct slot-writes + AUTOLINK, second
via __LT_ALLOC_ITO + manual slot-writes. The second ITO (Voca RA_MC_OP) could
be expressed as WAVE At=RA_MC_J, but the manual next-link from first to second
(RCN_NL/RCN_NLW) would remain. Net saving: ~5 lines. Low priority.

The EMIT/EMITI/PUTBYTE macros depend on RCN_IMPL and cannot be simplified
further without changing RCN_IMPL's first-ITO mechanism.
