# ROADMAP.md — Open Tasks

---

## Active

### A1. Flux — [x] Done

Flux is an Aria convention, not part of the canon — a lux of fully free-form
structure, living in a reserved region of Aether. Unlike NOVA or Data (fixed layouts),
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
if raw == 0:             → nxt = pc + NOVA_SIZE  (step — sequential, prefetcher-friendly)
elif raw >= FLUX_BOTTOM: → _exec_flux(raw)       (flux — structured parallel branch)
else:                    → nxt = raw             (warp — explicit address jump)
```

**Type lux:**
Word of a flux points to a type lux (Data lux) that describes its structure —
a sequence of slot codes, 0-terminated. The author defines this sequence freely:
any slot can repeat any number of times, appear in any order, or be absent entirely.
This is what makes flux a superset of both NOVA and Data — and strictly more,
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
Requires a new aspect or NOVA flag. Non-trivial, deferred.

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

**Status (2026-07-12): considered ready to start**, in a fresh session
(the design work belongs on its own thread, not folded into whatever
the current session happened to be about). B6's "42/42 means nothing
about correctness" gap that used to make this feel premature is now
closed as its own, separately-verified milestone (see B6 below) — the
graph-equivalence question this note used to flag as unverified has
actually been checked, with real findings, not just assumed clean. See
`PLAN_SCHEDULER.md` for whatever design thinking already exists —
read it first rather than starting from scratch.

---

### B6. Self-hosting: LOAD_MAIN as sole source of truth

**Status (updated 2026-07-12): the "42/42 means nothing about
correctness" gap this section used to flag as its own open follow-up
task has now actually been done — with real, substantial findings, not
a clean bill of health.** A new tool, `check_graph_equivalence.py` (see
TOOLING.md), compares the self-hosted graph against Python's real one
by symbolic name (not raw address) for every name Python's own source
confirms is genuinely NOVA-kind. First run surfaced ~1900 self-hosted
op-primitives looking like garbage instead of clean self-ref constants
— not a documentation gap, a real, previously undiagnosed correctness
bug. Tracing it down (via `trace.py`'s existing `--find-writer`, fixed
this session after discovering it was itself broken, plus a new
`sample_every` periodic-PC-history capability built specifically for
this) led to four real bugs in `htable.re`, the last of which was a
genuine, reproducible **infinite loop** (not a performance issue) that
the old "did the file loop finish" metric could never have caught,
since a run that never finishes just reports a lower N/42, which reads
identically to "still working on it" and "will never finish." See
`BUGS.md`'s forty-third round for the full, detailed account — worth
reading in full before touching `htable.re`, `BS_INTERN`, or the
self-hosted build path generally, since the same register-sharing bug
class (a value used by both a caller's own loop state and a callee's
internal scratch, no save/restore) recurred four times in one file and
is exactly the kind of thing to recognize on sight elsewhere too.

**Update (2026-07-12, later in the same session):** the infinite-loop
diagnosis above needed one more round -- a premature "just slow, not a
loop" conclusion was caught by the author asking for real verification,
which found a genuine, still-ongoing stall (`RA_LOAD_RPOS`/`SK_FIDX`
frozen for 190+ million of 200 million steps), traced to the real
cause: two `JEQ`s in `HT_LOOKUP`/`HT_INSERT` still jumped to their
*original* pre-session targets, bypassing every save instruction added
earlier in the session entirely (inserted code, never-redirected jump —
see BUGS.md's forty-fourth round for the full account, including a
real but non-causal `El2=`/`Exit=` bug found and fixed along the way).
Fixed: self-hosting now reaches **14/42** within the existing
200-million-step budget (confirmed via full-run tracing, not a
sample), reaching 20/42 in a one-off 500-million-step test — the
remaining shortfall is now confirmed to be a genuine step-budget/table-
load question, not a further correctness bug.

**Update (2026-07-12, final round this session):** the real, dominant
root cause found via actual profiling (not more guessing) — the same
"jump bypasses reset code" bug class, this time in `HT_NAME_CMP`'s own
per-character comparison loop, present since before this session:
`RA_HT_SLOT` (the loop's character-index counter, shared with the
outer probe-loop's bucket index) was never reset to 0 on a length
match, so every comparison started from whatever bucket index the
outer loop happened to be at instead of 0 — averaging ~3000+ wasted
character-comparisons per call, ~80-90% of all executed steps. Fixed
(one `JEQ` retarget). **Self-hosting now reaches 42/42 within the
original, un-boosted 200-million-step budget — no step-budget increase
needed.** See BUGS.md's forty-fifth round for the full account,
including two new pieces of reusable infrastructure built along the
way (a resumable checkpoint/resume harness and a fast hotspot
profiler) worth reaching for again if a similar deep-run investigation
comes up.

**Remaining, separate, non-blocking follow-up**:
`check_graph_equivalence.py` still reports 681 structural mismatches
concentrated in `macros.re`'s own internals (`WISR_START`,
`AUTOLINK_START`, `SWITCH_START`, `FOR_START`, and others) — the same
"Exit resolves to the next declared name" pattern noted earlier this
session. This is a real, narrower correctness question for whoever
picks this up next (does `macros.re` reprocessing itself during
self-hosted Wave-1 need to be excluded, fixed, or is this expected/
harmless given nothing at runtime actually calls into the self-hosted
copies of these macros — the question the author originally raised
about why `macros.re` isn't skipped, still not fully resolved) — but
it does not block self-hosting from completing, and does not need a
bigger step budget.

**Second open item**: a single hashtable lookup can still require
walking the entire table twice (`RA_HT_PROBE` reaching exactly
2×`RA_HT_SIZE`), confirmed present even after the fix above. This was
the session's original investigative lead, correctly abandoned as "not
the main blocker" once the math was checked (a full double-sweep costs
at most a few hundred thousand steps — nowhere near enough to explain
the multi-hundred-million-step slowdowns that turned out to have a
completely different cause). Real, but low-urgency and not yet
understood — see BUGS.md's forty-fifth round for what was and wasn't
established.

**New reusable tooling from this session** (see TOOLING.md):
`long_run.py` (resumable, checkpointed multi-billion-step runs —
pauses/resumes across separate tool invocations instead of restarting
from step 0 every time) and `fast_profile.py` (a hotspot profiler fast
enough to actually reach deep into such runs, unlike `trace.py
--profile`'s fully-traced loop — this is literally the tool that found
the `HT_NC_I` fix above). Reach for these first for any future
"self-hosting is slow/hangs somewhere deep" investigation instead of
rebuilding similar instrumentation from scratch.

**Current state, honestly:** the infinite loop is fixed and confirmed
gone (traced: the self-hosted run now genuinely completes a full
declare-only pass over all 42 files, then correctly transitions into
the real Wave-1 pass — see `SK_DECLARE_ONLY` in wax.re for the
two-pass architecture this now correctly exercises for the first time).
Wave-1 itself, now that hashtable lookups/upserts actually work instead
of always silently missing, is measurably expensive — the existing
200-million-step budget in `loader.py`'s `freeze()` gets through the
full declare-only pass but only a fraction of Wave-1 (14/42 files)
before running out. This is now an open, well-scoped, well-instrumented
performance question (raise the step budget and/or reconsider
`SK_HTAB_000`'s sizing for Wave-1's real load), not a correctness
mystery — see BUGS.md's own "Follow-up, not yet done" note for the
specific next steps and which tools verify them.

**Given this, "self-hosting reached 42/42" (the historical achievement
recorded below, 2026-07-11) should be read narrowly**: it means
`LOAD_MAIN`'s own file-processing loop, as it existed *before this
session's fixes*, ran to completion without crashing on all 42 files —
true, and still a real milestone, but (now confirmed) not the same
claim as "the graph it built was correct," which turned out to be
false in a specific, now-fixed way. Future self-hosting status claims
in this file should specify which of these two things is meant.

**Previous status (2026-07-11, kept for record — see above for what
changed since):** 42/42 files (100%), verified via the real, unpatched
`freeze()` call, stable across five repeated runs. The `14/42` regression recorded below
(2026-06-30) and the `33/42`/`1/42` history before it are kept as
historical record — worth reading for the specific bug classes found
along the way (several recurred in different shapes across sessions:
duplicate-label collisions, unreachable guard instructions positioned
before their own real entry point, shared/intermediate registers
clobbered by a called subroutine, dereference-depth mismatches). See
`BUGS.md` Patterns 9 through 20-ish (consult the file's own numbered
catalog for the current, full list) for the consolidated, current
catalog of what these actually were and how each was fixed, rather
than re-deriving the story from the round-by-round narrative below.

**What "self-hosting" means here, precisely, and what's NOT yet
verified beyond it:** `LOAD_MAIN` (running as compiled Reca code, not
Python) correctly processes its own, full 42-file source tree end to
end, reaching a clean, normal program end with no crash and no
diagnostic-tool finding. **Not yet separately verified:** whether the
*graph LOAD_MAIN itself produces* when self-hosting is byte-identical
(or behaviorally equivalent) to the one Python's own loader produces —
i.e., true fixed-point self-compilation (`LOAD_MAIN` compiling itself
compiling itself, producing the same result each generation) has not
been explicitly checked as its own, separate milestone. Worth doing as
a follow-up, not assumed for free from "self-hosting reached 42/42."

Python loader (Wave-A/B) duplicates LOAD_MAIN. Two sources of truth.
Currently Python loader is authoritative for the INITIAL build (the one
LOAD_MAIN itself needs to exist and run at all); once bootstrapped,
LOAD_MAIN can now process the full source tree itself.

**Done in the 2026-06-19 session (see `BUGS.md` for full detail on each):**
- Allocator under-sizing for RVOCA/RREDI/NOVA/JZ/JEQ/CLEAR/NOP/ALLOC_TO
  targets (`loader.py`, `_scan_nova_names` prepass)
- Systemic missing-closing-`//` comment bug across `yaku.re` (~37 fixes)
- Dead "OUTER register" pattern replaced with `RCALL_AT` (4 instances)
- `ALLOC_TO` given a Python-side fast-path (was missing one, unlike
  JZ/JEQ/CLEAR/NOP/RCALL_AT)
- **Fundamental fix**: `_resolve()` no longer returns bare integer literals
  as raw values — resolves to a `C_<N>` register address instead, since
  every NOVA field is read through a dereference. This was the root cause
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

**Historical blocker (resolved this session, kept for record):** a
call-stack imbalance (`RA_SP` underflow) while processing `wax.re`,
centred on `RREDI`'s own self-referential generic-macro definition
(`macros.re:86`, `RREDI RREDI_RET`). See `BUGS.md` for the specific
fix; the root cause turned out to be one of several separate,
unrelated bug classes chased across this session (see the note above),
not a single, isolated issue.

A first attempted fix (fast-pathing `RVOCA`/`RREDI` dispatch the same way
`N`/`S`/`L`/`I`/`B` already are) was tried and **made things worse**
(regressed 33/42 → 1/42) at the time — reverted then; not revisited,
since the eventual, real fixes took a different path entirely.

**Next step:** the Wave-1 performance follow-up noted above (raise
`max_steps` and/or `SK_HTAB_000` sizing, verify with
`check_graph_equivalence.py`), then continue hardening the systematic
checks (`run_all_checks.py`) against whatever the next real bug class
turns out to be, rather than assuming self-hosting's own completion
means the codebase needs no further systematic review.


### B6a. Reca must RE-DERIVE the prepass tables Python bakes for it — not consume them

**The principle.** Several LOAD_MAIN inputs are *computed by the Python
bootstrap loader and baked into the aether at freeze time* (in
`prepare_reca_freeze`, loader.py), and LOAD_MAIN then only *reads* the
finished table. That is convenient and correct as a bootstrap crutch, but
it is **not self-hosting**: Reca is leaning on the bootstrap language to do
the derivation for it. For true self-hosting, Reca must do that derivation
*itself*, in its own pass — not from scratch as raw bytes (the corpus is
still the single source), but *re-computed by Reca's own code* rather than
handed over pre-chewed by Python. Nothing here is a bug today; it is a
self-hosting-completeness debt to pay before B6 can call LOAD_MAIN the
*sole* source of truth.

**Canonical example — `SK_REGION_TABLE` / `SK_COMBO_SIZE` (region combos, B).**
Python's `_name_regions` / `_combo_key` derive every name's region combo
(In=OWN/SAVE, `K_`→save+lock, `RA_`→own) and `prepare_reca_freeze` bakes
two tables: `SK_REGION_TABLE` (`[name-hash, bitmask]` pairs) and
`SK_COMBO_SIZE` (luces per combo). LOAD_MAIN's `REGION_LOOKUP` /
`RESERVE_COMBOS` (alloc.re) merely consume them. To self-host, Reca's own
prepass must *derive* each name's combo — read `In=` from the token stream
(note: the region VALUES need real handling, since today `OWN`/`LOCK` are
not symbols and `SAVE` collides with the SAVE command — so compare the raw
region token, or give the three regions distinct readable identities), apply
the `K_`/`RA_` prefix conventions itself, and sum the combo sizes in a Reca
pass — instead of reading Python's `SK_REGION_TABLE`/`SK_COMBO_SIZE`.

**The same debt applies to every other Python-baked LOAD_MAIN table**, each
to be re-derived by Reca the same way:
  - `SK_DECL_TABLE` — the naming-command set (`_discover_nova_naming_commands`).
  - `SK_OPENER_TABLE` — block openers (`_discover_block_openers`, Open= hashes).
  - (and any future `prepare_reca_freeze` pre-population of the same shape.)

**Do not do this now** (agreed): it is real self-hosting work, unverifiable
until LOAD_MAIN actually runs authoritatively (0/42 today). Recorded so the
"Python bakes it, Reca reads it" pattern is revisited deliberately, not left
as an accidental permanent dependency.


### B6b. Wave-2 command coverage gap: 6 Python compound commands unhandled by LOAD_MAIN

A step-by-step wax.re ↔ loader.py diff (2026) confirmed the two sides'
Wave-2 dispatch and cross-cutting facilities are structurally parallel
(one dispatch point each: `cmd in _COMMANDS` / `BS_LOOKUP`+`Cmd=`; matching
Cmd=-word-write, arg pre-read + intern-on-miss, declare gate). The one real
difference is **command coverage**: Python's `_COMMANDS` has 24 commands;
LOAD_MAIN has dedicated handlers for ~18 of them (the 8 primitives plus
FOR/DEF/SAVE/SWITCH/MATCH/JEQ/JZ/NOP/CLEAR/ALLOC_TO). It has **no** handler
equivalent for these six, all present in the corpus and built by Python:

  - `NOVAE`      (38 corpus lines) — runtime NOVA synthesis (`_CmdNOVAE`)
  - `WRITE_OUT`  (6)
  - `HASH_STEP`  (5) — djb2 update step (`_CmdHASH_STEP`)
  - `ALLOC_RAW`  (5)
  - `PACK_BYTE` / `UNPACK_BYTE` (3 total)
  - (`READ_BODY` — 0 corpus lines, dead; ignore.)

Reca's runtime macros (EMIT/LR/LT/WALK_ONE/YAKU/PUTBYTE/... , registered via
Cmd=) are correctly OUTSIDE Python's `_COMMANDS` -- Python handles them
through its generic discovered-naming-command branch, not a handler -- so
they are not part of this gap.

This is self-hosting coverage, not an architecture-cleanliness issue, so it
belongs here under B6, not in the "make it clean" pass. Adding these six
handlers is real, unverifiable-until-live LOAD_MAIN work (0/42 today).


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

The integer resolver in `LOAD_RKVR_INT` (wax.re) uses `addr(C_0)+N*2` formula
to find a lux holding value N. This is wrong for sparse C_N (only C_0..C_19
are contiguous; C_32, C_48, C_100, C_4096... have gaps). Formula gives garbage
address for any N not in 0..19.

**Required fix:**
- `wax.re` `LOAD_RKVR_INT`: instead of formula, allocate (or find via htable)
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

### D4. Rename NOVA and NOVAE (runtime NOVA creation)

**NOVAE is implemented** (loader.py + macros.re). Syntax:
`NOVAE label Op [El1=X] [El2=Y] [Exit=Z] [At=A] [Next=N] [Reset=1]`
Covers all builder macros. Only the renaming remains open.

**Two separate naming decisions:**

1. **NOVA → ?** — "step" is not quite right; NOVA is an autonomous graph node,
   not necessarily sequential. Need a word that conveys
   "autonomous unit of execution". Candidates considered:
   ACT, BEAT, NODE, PULSE, QUANT, CELL, GRAIN, GLYPH, FORM.
   None were satisfying — open question, revisit later.

2. **EMIT_NOVA → NOVAE (preliminary)** — runtime macro that creates a NOVA
   in the graph during execution. Syntax mirrors NOVA:
   `NOVAE label Op El1=X El2=Y Exit=Z`
   If NOVA is renamed — revisit NOVAE too.
   NOVAE chosen preliminarily; finalize together with point 1.

**Do not implement until names are resolved.**

---

### D6. Idem — Aspect #25, the zero-dereference rung — [x] Done

**Final name: `Idem`** (Latin: "the same") — chosen from the candidate
list below after discussion with the author.

Implemented as a genuine, 25th native Aspect in `aspects.re`
(`SEAL Idem`), not a load-time primitive and not a `derived.re`
composition — see `aspects.re`'s own "Why exactly these" section for
the full reasoning: it maps to immediate-load, a real, universal CPU
instruction class (x86 `MOV reg,imm` / ARM `MOV`,`MOVZ` / RISC-V `LI`)
distinct from the memory-load class the other 24 already cover, not
something Reca-specific. `interpreter.py`'s own `_idem` sits directly
beside `_move` for comparison: zero dereferences (`aether[exit] = a1`)
versus `_move`'s one (`aether[exit] = aether[a1]`).

Confirmed directly: `Idem(El1=SEAL, Exit=X)` writes `SEAL`'s own real
address into `X`, not `SEAL`'s word (what it points to) — the exact gap
the `_REF` pattern (`SEAL X_REF X`, `C_7_REF` etc.) was working around
project-wide. `_REF` holders are not obsoleted by this (a caller who
wants an address still gets one, one way or another) but their
*mechanism* now has a direct alternative that needs no pre-declared
holder lux at all.

Not yet done, deliberately deferred as a separate, later idea (per the
author's own framing): a macro/expansion mechanism for generating N
`Idem`-based references at once (the "however many you need" case) —
not needed until a real call site wants it.

**Original candidate list, for the record:** Latin — `Ipse`, `Sic`,
`Merus`/`Purus`, `Recta`, `Idem` (chosen), `Nudus`. Japanese — `Sugu`,
`Mama`, `Jika`, `Sono`. English — `Bare`, `Flat`, `Lit`.

---


RCN_IMPL synthesizes 2 NOVAs: first via direct slot-writes + AUTOLINK, second
via __LT_ALLOC_NOVA + manual slot-writes. The second NOVA (Voca RA_MC_OP) could
be expressed as NOVAE At=RA_MC_J, but the manual next-link from first to second
(RCN_NL/RCN_NLW) would remain. Net saving: ~5 lines. Low priority.

The EMIT/EMITI/PUTBYTE macros depend on RCN_IMPL and cannot be simplified
further without changing RCN_IMPL's first-NOVA mechanism.
