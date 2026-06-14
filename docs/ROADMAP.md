# ROADMAP.md — Open Tasks

---

## Active

### A1. Flux — ✅ Done

Flux is an Aria convention, not part of the canon — a lux of fully free-form
structure, living in a reserved region of Aether. Unlike ITO or Data (fixed layouts),
a flux has no required slot order, no fixed size, and no fixed meaning per slot.
Any part can be omitted, repeated, or redefined.

Triggered only by an explicit jump (next != 0 AND addr >= FLUX_BOTTOM).
The fast path (+7, fall-through) never sees flux — zero overhead for code
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

### B2. ADD_LUMEN: overflow chain — ✅ Done

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

**Status:** in progress. LOAD_MAIN runs but completes in 1 step (0/43 files). Builder-class macros (LH, LR, LT, WALK_ITO, RCALL_AT) are unwired — they require LOAD_MAIN to run correctly. FOR macro expansion bug fixed (170→143 issues). See BUGS.md Pattern N.

Python loader (Wave-A/B) duplicates LOAD_MAIN. Two sources of truth.
Currently Python loader is authoritative. Self-hosting = LOAD_MAIN becomes authoritative.

**Done:**
- Phantom labels, name conflicts, comment-line bugs fixed
- SETREF dispatch fixed (tlen check)
- BS_FILE_COUNT corruption fixed
- K_CURSOR/K_WATERMARK double-deref setup
- Post-LOAD_MAIN restoration in loader.py

**Blocker:**
LOAD_MAIN exits after 1 step — root cause not yet established.
Details: see `BUGS.md`, Pattern 8.

**Next step:**
Find root cause of RA_LOAD_BYTE=LF at MA1 check.
Recommendation: clean architecture first (uniform implementations) to make
errors more visible.

---

## M1 — Native mobile target (no LLVM)

Yaku currently emits LLVM IR, which requires clang/llc at build time.
Goal: Yaku generates ARM64 directly — no LLVM dependency.

Result: Reca programs distribute as a single signed binary.
No JIT, no W^X, no entitlements. Legal everywhere, including App Store.

**Prerequisite:** B6 (self-hosting).
