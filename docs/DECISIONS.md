# DECISIONS.md — Architectural Decisions

This file records decisions that **look like problems** but are intentional,
and explains why. The goal: avoid repeating the same reasoning twice.

---

## Architecture

### SLOT_NEXT: hybrid semantics (implicit fall-through + explicit link)

```
aether[pc + SLOT_NEXT] == 0  →  implicit fall-through: next_pc = pc + NOVA_SIZE
aether[pc + SLOT_NEXT] != 0  →  explicit graph link:   next_pc = aether[pc + SLOT_NEXT]
```

Loader leaves SLOT_NEXT=0 for physically adjacent NOVAs (already in memory order).
Runtime can patch SLOT_NEXT to insert a new lux without memory shifts.
This gives both cache-friendly linear execution and runtime graph mutability.

Trade-off: ~14% memory overhead (one zero slot per NOVA). Acceptable — no better alternative.
Removing SLOT_NEXT = killing runtime mutability.
Two NOVA types (6-slot + 7-slot) = branch predictor chaos.

---

### Inline lumina vs separate luces

Lumina sit inline in the same lux (after pad). In 95–99% of cases there are zero
extra lumina — the branch predictor learns the pattern, the pipeline flies.
A rare mispredict (10–15 cycles) << constant cache misses from jumping to separate luces
(200–300 cycles). Inline wins.

---

### Call stack: CS_PUSH / CS_POP

Separate region in Aether (STACK_BOTTOM..STACK_TOP).
All non-leaf functions go through CS_PUSH/CS_POP.
The `_OUTER` pattern is fully removed (561 replacements).

---

### Flux: no predefined types

No prebuilt slot combinations. A type lux describes structure dynamically.
Fixing specific types contradicts the idea of flux — the author decides the structure.

---

### Flux as operand

The interpreter does not interfere. A flux in el1/el2 → the reader gets
the word as a value. Semantics are defined by the author through usage context.

---

### Flux zone fragmentation

Current: bump allocator (simple, fast).
Future: free-list when fragmentation becomes a real problem.
Does not block the current implementation.

---

### Exit — single direction concept

`Target` and `Dest` as separate relations are retired. `Exit` covers both.
Unifying them removes a redundant distinction that added no information.

---

### Parallelism is an Aria, not native

Baking dataflow scheduling into the native interpreter would add overhead to every
instruction, killing the fast path even where parallelism is not needed.

---

### Variadic element macro — open future Aria

Extending NOVA with N inputs and M exits is a higher-level graph pattern.
The macro compiles to a chain of NOVAs. NOVA stays 7 slots.

---

## Macros

### EMIT / EMITI / PUTBYTE — three separate macros

**Looks like:** duplication (three similar bodies).
**Why not:** Semantically different operations — string, integer, byte. Names carry
semantics and are used at call-sites (yaku.re). Implementations share `RCN_IMPL`
(common body); public names are preserved.

---

### WALK_ONE vs WALK_NOVA — different lux naming mechanisms

**Looks like:** clones with one difference (offset C_1 vs C_7).
**Why not:** WALK_ONE generates **named** luces (MA3/MA4/MA5 — auto-suffix, entered
into LSYM). WALK_NOVA — **anonymous** (`__LT_ALLOC_NOVA`). Intentional distinction:
named luces are needed when sub-luces must be referenced externally.
Structurally different, not duplication.

---

### JEQ/JZ/LX/LH/LINK_OP/UNLINK_OP — inline autolink with RA_MA_RET

**Looks like:** inconsistency (others use `Voca AUTOLINK`).
**Why not:** `RA_MA_RET` here links luces **within one macro** (between the second
and third lux). `AUTOLINK` and `RA_MC_PREV` link **with the external chain**
(previous NOVA in the .re file). Fundamentally different roles.
`WIRE_AUTOLINK` is not applicable for intra-macro links.

---

### RRET/RCALL — named luces linked via explicit Next, not RA_MC_PREV

**OBSOLETE — RRET/RCALL have been removed (zero callers; superseded by the
automatic call stack, see registers.re). Kept for historical reference only.**

**Looks like:** does not use WIRE_AUTOLINK.
**Why not:** Luces MA1..MA5 are linked via `Add MA_X+C_5→slot; Write slot←MA_Y`.
This is an explicit link through the Next slot, not through `RA_MC_PREV`.
WIRE_AUTOLINK updates `RA_MC_PREV` — not applicable here. Inline wire stays.

---

### Two-pass scheme in wax.re

**Looks like:** could be single-pass (backfill exists).
**Why not:** Pass 1 counts the number of LINK lumina per source.
This is needed for compact in-place memory allocation — no overallocation.
Backfill resolves forward references but does not give lumen block size upfront.
Two passes is an architectural decision, not a mistake.

---

### parser.re — own 3-digit number parser instead of BS_PARSE_INT

**Looks like:** duplicated parsing logic.
**Why not:** parser.re parses a specific format — 3-digit numeric suffixes in names
(NNN in OB_NNN, DS_NNN). This is a specialised parser for a fixed context,
not a general number parser. BS_PARSE_INT is universal. Different tasks.

---

### parser.re — two-level JT/JTL dispatch instead of hash table

**Looks like:** inconsistency with wax.re/lexer.re.
**Why not:** parser.re is a compiler, optimised for speed. JT on first byte +
JTL on second byte = O(1) dispatch without hash computation. wax.re is a
runtime loader with lower speed requirements. Different contexts, different
approaches are justified.

---

### Three string packing approaches (lexer / loader / parser)

**Looks like:** inconsistency.
**Why not:** Each level solves its own task — lexer.re: speed without escape,
loader: full string parser with special characters, parser: templating with
`{name}` placeholders. These are layers, not duplication.

---

### Stream parsing (lexer.re) vs line buffering (parser)

**Looks like:** inconsistency in I/O model.
**Why not:** parser.re buffers lines for backtracking and contextual analysis
(LLVM IR rules). lexer.re reads a stream for maximum speed.
Different requirements, different solutions.

---

### RA_BS_RESULT save in BS_INTERN_NAMED

**Looks like:** hack (save → call → restore).
**Why not:** BS_PACK_TOKBUF uses ALLOC_LUCES which writes to RA_ALLOC_RESULT,
but BS_INTERN_NAMED already wrote the address to RA_BS_RESULT before the call.
Saving via RA_BS_EL0 is the correct caller-save register pattern around a call.
Not a hack — correct calling convention.

---

### ETH_* shared restore pattern in yaku.re

**Looks like:** many handlers manually save RA_TW_LUX into RA_ETH_TW_SAVED.
**Why not:** This is intentional shared-endpoint architecture. All handlers jump
to a common `ETH_TW_RESTORE` which restores and proceeds to `ET_ADVANCE`.
SAVE macro generates save/restore around each block separately — here one common
restore. This is more efficient than N separate SAVEs.

---

### LOAD_MAS_0..7 in wax.re — MA-slot reading not via FOR

**Looks like:** 7 identical RVOCA+JZ+NOVA patterns for MA1..MA7.
**Why not:** Each iteration ends with `RVOCA LOAD_MAS_{N+1}_RD` — calling the
next reader by name. FOR does not support `{N+1}` indexing in a template.
Applying FOR is impossible without restructuring.

---

### NONOVA and CHAIN — macros for grouping luces

**NONOVA** — list of independent luces (each with its own NOLINK):
```
NONOVA
    NAME1 Move El1=foo Exit=bar
        RVOCA X Y
    NAME2 Move El1=baz Exit=qux
```
Expands to: NOLINK + NOVA NAME1 + RVOCA X Y + NOLINK + NOVA NAME2.

**CHAIN** — one NOLINK, chain of linked luces. Named or anonymous:
```
CHAIN LOAD_FL
    _DONE Move El1=SYS_CLOSE Exit=SC_NR
    Move El1=RA_LOAD_FD Exit=SC_A0
        RVOCA X CS_POP
    _RET Redi El1=RA_LINK
```
Three unified modes for NONOVA/CHAIN/SWITCH: anonymous, explicit anonymous (`_`), named.

---

### Wave-B: unknown name warnings for FOR templates

Wave-B emits `Unknown name: 'X{N}'` for names from FOR/CHAIN bodies containing `{N}`.
This is expected: FOR templates expand in Pass-2 (Reca runtime), not Wave-B (Python).
Wave-B sees the literal `{N}` as a name and cannot find it — correct behaviour.
Fixing it would require Wave-B to understand FOR/CHAIN syntax. Not worth the complexity.

---

### CHAIN cannot apply when a block does not start with NOVA

Pattern: `RVOCA X CS_PUSH` → `NOVA A ...` → `NOVA B ...`
Applying CHAIN to NOVA A/B adds an extra NOLINK between CS_PUSH and A.
CHAIN only fits blocks where the first line is an entry point.
Blocks starting with RVOCA (call as part of the chain) — do not apply CHAIN.

---

### DJB2 hashing — three inline copies in hot loops

**Looks like:** duplication (3–4 places: BS_RT, BS_TV, LOAD_DL_CO, parser.re).
**Why not:** DJB2 is in the **hot loop** of the tokenizer. Extracting to a
subroutine adds Voca overhead on every iteration — unacceptable.
Each location also differs slightly (different registers for byte, different masks).
Stays inline.

---

### Division by zero returns 0 instead of fault

**Looks like:** silent bug.
**Why:** `_fault_vector` is not yet implemented as a full exception system.
Division by zero → 0 is predictable behaviour.
When fault_vector is implemented — add a jump to it on div/0.

---

## Python Bootstrap

### FUNC hardcoded in Python _expand_indent

**Looks like:** duplication — Python hardcodes `Sub RA_SP + Write RA_SP RA_LINK`
instead of using the FUNC macro from macros.re.
**Why not:** This is the bootstrap layer. macros.re is loaded through the same
Python loader. Removing FUNC from Python would break it before macros.re loads.
Duplication is unavoidable at the bootstrap level. When self-hosting is stable —
the Python layer is removed entirely.

---

### Syscall number caching in interpreter.py

**Looks like:** violation of "aether as single source of truth".
**Why not:** SYS_WRITE and other syscall numbers are Linux ABI constants.
They do not change at runtime. Caching at `update_relations` is a correct
performance trade-off. If syscall virtualisation is ever needed — add a hook
in `update_relations` when aether[SYS_*] changes.

---

### `_resolve()` mints a `C_<N>` register for bare numeric literals instead of returning the literal value (2026-06-19)

**Looks like:** unnecessary indirection — why not just use the literal `0`/`47`/etc.
directly as the field value?
**Why not:** every NOVA field (`e1`/`e2`/`exit`) is read by the interpreter
through a dereference (`aether[a1]`, never `a1` itself). A field holding the
bare value `47` doesn't mean "compare against 47" — it means "compare
against whatever's stored at address 47". The project's own established
convention already worked around this by hand for the single most common
case (`JZ` uses `RA_C0_REF`, the *address* of the `C_0` register, never bare
`"0"`) — this decision generalises that pattern to `_resolve` itself, so
every bare-literal operand resolved through it gets the same treatment
automatically, including ones synthesised by macro expansion (e.g.
`_expand_indent`'s `SWITCH`-to-`JEQ` lowering) where the literal was never
written by hand in the first place. Reuses an existing `C_N` constant where
the project already defines one; otherwise lazily allocates and memoizes a
dedicated 1-lux register (`self._lit_const_cache`), so repeated uses of the
same literal share one address. See `BUGS.md`, Session 2026-06-19, section K,
for the concrete bug this fixed (an infinite loop from `Equal`'s
truthiness-gated logic treating a *zero address* as "no operand" for the
literal `0` case specifically).

---

## Stable Invariants

These are not canon but should not change without deliberate discussion:

- NOVA layout: 7 base luces (word, op, e1, e2, exit, next, pad)
- `BS_HT_MASK = 0x3FFFF` (262144 slots) — **still correct as the slot-index
  mask** (2026-06-19 session: found and fixed a bug where `lexer.re`'s hash
  routines were *also* applying this same narrow mask to the hash *value*
  itself, before it ever reached `HT_LOOKUP`/`HT_INSERT` — that defeated
  wide-hash collision resolution, since the "distinguishing" stored hash
  became just as narrow as the slot index. Fixed by keeping the running
  hash wide (natural 64-bit) until `BS_LOOKUP`/`BS_INTERN`/`BS_INTERN_NAMED`
  mask it to `MASK_LOW32` — matching `htable.re`'s own documented "hash32"
  slot format — right before handing it to `RA_HT_HASH`. `BS_HT_MASK`
  itself, and its use *inside* `HT_LOOKUP`/`HT_INSERT` for slot selection,
  is unchanged and correct. See `BUGS.md`, Session 2026-06-19, section L.)
- BLOCK base addresses (`_000`) in htable — LOAD_MAIN resolves them correctly
- 25 Aspects in `aspects.re` (as of the Idem addition, BUGS.md Pattern Z) — changing this set is a real, deliberate decision, not a casual one

---

### Mobile execution model

Reca's execution model is structurally legal on all mobile platforms.

The interpreter lives in a signed, static binary — RX, never touched at runtime.
The Aether is plain RW memory — from the OS's perspective, just an array of numbers.
No mprotect, no anonymous executable mappings, no JIT spray surface.

When Yaku generates ARM64 natively (M1), the result is a standard signed binary.
No LLVM at runtime, no dynamic code generation, no entitlements required.
The OS sees an interpreter reading numbers. That's all it ever needs to see.

---

## D-NOVAE. NOVAE — runtime NOVA synthesizer (2026-06)

**Problem:** All builder macros (RVOCA, RREDI, CLEAR, NOP, JEQ, JZ, LX, LH, WALK_ONE, LINK_OP, UNLINK_OP, LR, LT, WALK_NOVA, ALLOC_TO, RCALL_AT) manually set `RA_MC_OP/E1/E2/DEST/LUX` via 4–6 Move instructions before each `RVOCA WRITE_NOVA_SLOTS` call. RCALL_AT alone was 54 lines.

**Decision:** Add `NOVAE` as a preprocessor command in `loader.py` that expands to the standard builder sequence. Mirrors `NOVA` syntax exactly: `NOVAE label Op [El1=X] [El2=Y] [Exit=Z] [At=A] [Next=N] [Reset=1]`.

**Key design points:**
- `At=` eliminates the first `__LT_ALLOC_NOVA` when lux already exists externally
- `Next=` eliminates manual `Add El1=src El2=SLOT_NEXT + Write slot←dst` link pairs
- `Reset=1` selects `WRITE_NOVA_SLOTS_RESET` for chain terminators
- All parameters optional — simplest case: `NOVAE label Op`
- Preprocessor (not Reca macro) because Reca can't efficiently pass 5 independent arguments

**Result:** RCALL_AT: 54→14 lines. JEQ: 20→5. ALLOC_TO: 33→6. RVOCA/RREDI/CLEAR/NOP: 10→3 each. Total macros.re: ~400 lines saved.

---

## Will Not Be Implemented

*(Moved from HISTORY.md — these are decisions, not just history.)*

### Lumen without rel (anonymous lumina)
Without `rel`, multiple lumina on one lux are indistinguishable except by position.
Positional semantics is equivalent in complexity to explicit rel, but less readable.
A lumen without rel is indistinguishable from a plain exit at the NOVA level.
The `(rel, exit)` structure is optimal and stays.

A library may still add rel to an existing exit after the fact via Write into Aether.
The core does not prevent this.

### rbin bootstrap as the primary self-hosting path
The idea: Python loads `reca.rbin` → registers 25 Aspects → runs LOAD_MAIN.
LOAD_MAIN rebuilds the graph → saves a new rbin.

Not the primary approach because rbin is fragile under architecture changes.
If the fundamental structure changes (NOVA layout, lux size, lumen format) — the old
rbin is incompatible with new code. Maintaining a separate old Python loader or
manually rebuilding rbin is undesirable in active development.

The correct path: Wave-B in Python stays, but gradually transfers to Reca.
Parts of the Python loader that can live on Reca move there. Not "rbin instead of
Python" — "Reca gradually takes over from Python".

### Per-thread registers via runtime name→address translation
Considered as a way to give each thread of a future scheduler its own copy
of scratch registers (`RA_TMP` etc.) without touching existing code.
Rejected: by the time a program is built, register names no longer exist
anywhere — every instruction already contains the bare resolved address,
decided once at build time. "Translating" a name to an address at run time
would mean re-deciding it on every single instruction that touches a
register (nearly all of them), on every step of execution. Address
resolution correctly happens exactly once, at build time; making it happen
per-instruction at run time instead would be a large, ongoing cost for no
real benefit over the region-copy approach below.

### Per-thread registers via universal double-indirection
Considered as an alternative: make every register work like `K_CURSOR`
already does (`aether[K_X] = K_X+1`, real value one slot further), so a
thread's register set could be repointed by writing one new address instead
of copying values. Rejected: ordinary `Move`/`Read`/`Write` are single-
dereference today (`aether[exit] = aether[a1]`). Making all registers
double-deref would change the meaning of nearly every existing instruction
project-wide — a large, high-risk rewrite to solve a problem that doesn't
actually need double-indirection (see Pattern W in BUGS.md for the adopted
fix: collect registers into one contiguous block, give each thread a cheap
copy of that one block; no instruction needs to change).

### Per-thread copies of only the scheduler's own bookkeeping registers
Considered as a cheaper, narrower fix: give private copies only to the 2-3
registers `SCHED_FORK` itself uses (`RA_SCHED_TID`, `RA_SCHED_NEXT`, etc.),
leave the rest of the ~251 registers shared. Rejected as incomplete: the
race condition isn't limited to the scheduler's own bookkeeping — any two
threads running any two pieces of ordinary code will contend for the same
general-purpose scratch registers (`RA_TMP`, `RA_I`, ...) constantly, not
just in the narrow window right after `clone`. A patch that only covers
the scheduler's own registers would work for one narrow test and fail the
moment real parallel work (not just the fork mechanism itself) runs on two
threads. See Pattern W in BUGS.md for the full writeup and the adopted
fix.

### `NEW` defaulting to per-thread-personal instead of shared
Considered making `NEW` itself mean "one private copy per thread" and
adding a separate word for the ~35 registers that must stay genuinely
shared (`BS_HT_BASE` and similar base-address pointers). Rejected: the
project's own stated nature of the aether is "no separate heap, stack, or
code segment — only Aether" (`GLOSSARY.md`) — one shared space by default,
with anything private explicitly carved out on top (the call stack, the
flux zone), never the default itself. Defaulting `NEW` to personal would
mean a future register declared without thinking about thread-safety
silently becomes invisible to other threads unless someone remembers to
opt out — the dangerous failure direction. `NEW` keeps its current meaning
(shared); a new word marks the registers that must become personal.
Rewriting cost is the same either way (~251 registers need marking
regardless of which way the default points) — the choice was about which
way a forgotten declaration fails safe, not about typing effort. See
Pattern W in BUGS.md for the full writeup.

---

### Wave 2 is one pass; reference targets always go through `_resolve`

Wave 2 runs as a single pass: `_wave2_build_line` calls `_wave2_alloc_line`
then `_wave2_wire_line` for each line (`update_relations` runs once before it).
In one pass, a target declared later in the same pass has not been allocated
yet when a reference to it is wired. So every operand-slot write resolves its
target with `_resolve`, which mints a correctly sized stub on a miss. Never use
a raw `self.symbols.get(name, 0)` for a target: its 0 lands in an operand slot,
and the self-hosted run then executes into address 0 and stops. Two instances
of this class were fixed: `CLEAR Name=SCAN_REGISTRY` got op=0 because `Move` was
declared later, and `RVOCA LOAD_CNR_ICTX LOAD_CN_INTERN` got e1=0.

Deliberate exceptions, where a raw lookup is correct:
- `_resolve_block_count`: it needs the real declared size. A stub would be
  wrong, and a missing name is a genuine error (it raises `LoadError`).
- Lookups of the lux *being declared* (`_nova_lux`'s own name, `NEW`'s
  existence check): a miss there means "allocate it now", not a forward
  reference.
- Post-build setup inside `freeze()`: every symbol is final by then.

Idea left open: defer macro execution to the end of the build and drain the
queue to empty, with nested macros allowed and resolved addresses stored
instead of strings.

## Completed cleanups (architectural, not bugs)

*(Moved from HISTORY.md "Fixed" section.)*

- `WIRE_AUTOLINK` / `WIRE_AUTOLINK_RESET` — unified wire pattern in RVOCA, RREDI, CLEAR, NOP, RCALL_AT
- `RCN_IMPL` — shared body for EMIT/EMITI/PUTBYTE
- `SNL_IMPL` — shared body for WAX_NEXO_TERM/CMP/ARITH
- Inline autolink → `Voca AUTOLINK` in 8 places
- `UNLINK_OP` aligned to named-lux mechanism like LINK_OP
- `FUNC_ENTRY`/`FUNC_RET` removed (dead code)
- `RCALL`/`RRET` macros removed (dead code; superseded by automatic call stack)
- `BS_CS_BUF_000`/`BS_CS_SP` (1024-entry inline call-stack array) removed
- `WIRE_LUX` removed (unused)
- 62 `Equal+C_0+JumpIf` patterns → `JZ` in wax.re
- `PTBUF_000` increased to 256 (matches the C_255 check)
- Empty `PS_NEXT_SYMBOL` section removed from parser.re
- Ghost comment `RA_DEST_N` removed from yaku.re
- 24 of 27 redundant JMP2 jumps removed from wax.re
- String stride fixed: stride=1 in Python and Reca (was DATA_NODE_MIN=2 mismatch)
- `HARD_BOTTOM` made dynamic: computed from real `size` in Symphony.__init__

---

## Resolved: unified the `NEW`/`NEWREF`/`NEWSET`/`SETREF`/`ALIAS` declaration family under `NEW`/`SEAL` (implemented 2026-07-04)

Not yet implemented — a large, project-wide syntax change, deliberately
not started mid-task. Recorded here so the reasoning isn't lost.

**The observation:** `NEW`, `NEWREF`, `NEWSET`, `SETREF` (and the new
`OWN`, see BUGS.md Pattern W) are five different words for the same
underlying action — "create a name, decide what goes in its word slot at
creation." `NEW` puts 0. `NEWSET X N` puts the number `N`. `NEWREF X` puts
X's own address (self-ref) or, when given a second name (`NEWREF X Y`,
94 real uses in the project vs. 55 true self-refs), puts Y's address
instead — two different meanings currently sharing one word. `SETREF` is
the same "point at another name" action for an already-declared name.
`ALIAS` was a related case, now folded into the same analysis below.

This is the kind of divergence the project's own pattern-analysis
principle says to look at hard: is `NEW`/`NEWREF`/`NEWSET` a real,
necessary difference, or five names for one idea, kept apart by history
rather than by meaning? Checked honestly — it's the latter. `SET` (plain,
not `NEWSET`) is genuinely different (rewriting an *existing* name's value
later, not creation) and stays separate; it does not belong in this
unification.

**Shape (implemented):** one word, `NEW`, followed by the name, then
modifiers in the same `key=value` / bare-flag style already used by
ordinary NOVA instructions (`El1=X`, `At=Y`, `Reset=1`) — reusing an
existing project convention rather than inventing a new one. `NEW X`
alone stays word=0 (today's default, unchanged). `NEW X | SET=12`
(word=12, replaces `NEWSET X 12`). `NEW X | SEAL` alone (bare, no `=`)
means self-ref — replaces bare `NEWREF X`. `NEW X | SEAL=Y` (word=Y's
address) replaces `NEWREF X Y` and `SETREF X Y`'s creation case — kept as
one word with two forms (bare vs. `=Y`) rather than being split into two
separate words, per the author's preference; the bare/`=` distinction
already reads clearly at a glance and keeps the concept coherent.

**The word `REF` was replaced with `SEAL` (author's call, 2026-07-04) —
the semantics above (one primitive, bare = self-ref, `=Y` = cross-ref)
are unchanged; only the spelling changed.** Reason: `REF` read as a
generic programming-jargon abbreviation, out of step with the project's
own naming register (Latin/Greek/Italian/Japanese thematic words: `Voca`,
`Redi`, `Exire`, `Nova`, `Lux`, `Lumen`, `Aria`, `Yaku`, `Wax`,
`Wane`, `Iris` — see GLOSSARY.md). Candidates considered: `INDICA`,
`MONSTRA`, `SIGNA`, `TENDE` (all Latin imperatives meaning "point
out"/"show"/"mark"/"aim at") and the author's own `SEAL`/`SIGN` — `SEAL`
was chosen over `SIGN` specifically to avoid colliding with "signed"
(sign bit / signed-vs-unsigned), a real, existing low-level concept this
project's own arithmetic and masking already touch. Renamed everywhere in
one pass: `_BOOTSTRAP_CMDS`, every Wave-A/A2/B0/B handler, `.re` files
(211 lines), `discover_ref_macro_names` → `discover_seal_macro_names`,
`test_macros.py`, and the runtime macro-registration lines in
`macros.re` (`SEAL NEWSET NEWSET_START`, `SEAL NEWREF NEWREF_START`,
`SEAL SETREF SETREF_START` — the two latter registrations are dead code
now, since no `.re` file spells `NEWREF`/`SETREF` as a command anywhere
any more; not deleted yet, kept as a mechanical, zero-risk rename only —
see BUGS.md Pattern X).

`NEW X | OWN` / `NEW X | FREE` mark and unmark the Pattern W
per-thread-personal primitive (`FREE` confirmed as the symmetric opposite
of `OWN`; `PUBLIC` was considered and rejected as an adjective/state word
rather than a matching action-verb; `SHARE` was also considered, `FREE`
is the one kept). **Both `OWN` and `FREE` also work as standalone
top-level commands** (`OWN RA_TMP` / `FREE RA_TMP` on an
already-declared name, without recreating it) **as well as** modifiers
inside `NEW`'s tail (`NEW RA_TMP | OWN`) — the two forms serve different
moments (declare-and-mark-at-once vs. mark-an-existing-name-later) and
neither replaces the other; both write into the same
`self._new_modifiers` dict, by name, so there is exactly one place "is
this name owned" ever lives regardless of which form declared it.
Standalone `OWN`/`FREE` on a name that was never declared raises
`LoadError` (mark, not create) — logged and skipped by
`_run_pass_over`'s uniform per-line error handling, same as every other
command's line-level errors, not a special case. Modifiers can combine
on one line (e.g. `NEW X | SEAL=Y OWN`). `line.split()` (the loader's
actual line-parsing, checked) splits on whitespace, so bare modifier
words never collide with existing compound names that happen to contain
the same substring (e.g. `RA_LINK_REF` stays one token, never confused
with the standalone modifier word `SEAL`). See BUGS.md Pattern W for the
full note and a `__slots__` bug this surfaced and fixed
(`_new_modifiers` wasn't declared in `Loader.__slots__` — dormant only
because no `.re` file used the `|` tail syntax yet at the time).

**`ALIAS` joins the family as `NEW X | ALIAS=Y` (resolved, not a
special case after all).** `ALIAS X Y` (already existed, Wave-B0, and
still works standalone) makes X the *same address* as Y — zero
allocation, a true synonym — unlike `SEAL`/`SET`/`OWN`/`FREE`, which all
act on a lux `NEW` actually allocates fresh for X. This was flagged as a
tension (`NEW` implying "allocate", `ALIAS` implying "don't"), but on
reflection it isn't one: every modifier in this family already does
something structurally different from the others (`SET` writes a value,
`SEAL` writes an address with one hop of indirection, `OWN`/`FREE` just
flag) — `ALIAS` skipping allocation entirely is simply the most extreme
point on that same spectrum, not a break in the pattern. Implementation:
`NEW`'s Wave-A handler skips the ordinary allocation step for any name
that a global prepass (`_scan_deferred_names`, generalized from what was
`_scan_block_span_names` to also cover `ALIAS`/`ALIAS=` targets, not
only `BLOCK`/`SPAN`) has already marked as "defined by something else" —
Wave-B0 then resolves `self.symbols[X] = resolve(Y)` once every name
project-wide is known, safe regardless of file/line order (mirrors how
`SEAL`'s own cross-ref target was already protected from the same
premature-preallocation hazard). `ALIAS=Y` always requires a value, like
`SET=Y` — there is no meaningful bare-`ALIAS` self-case, since a name is
already its own address by definition.

**Scope of what "the syntax" covers (implemented) vs. what's still
separate:** this section is the *parsing/allocation* layer only — the
loader now correctly understands and applies `NEW`/`SEAL`/`OWN`/`FREE`/
`SET`/`ALIAS` in every combination described above. It does **not**
include actually marking the ~251 real scratch registers `OWN`, or
collecting them into the contiguous per-thread block Pattern W needs —
see PLAN_SCHEDULER.md's step 1, a deliberately separate next step that
depends on this one.

**Historical context (why this took a whole session):** this touched
every declaration in the project (250+ registers alone, plus every
`NEWREF`/`NEWSET`/`SETREF` site — well over 300 lines project-wide).
Doing it well required the loader's parsing of each of these commands to
be re-examined together (not just find-and-replaced textually), which was
real design work, not routine — done as its own dedicated pass rather
than interleaved with the scheduler/arena work in PLAN_SCHEDULER.md /
PLAN_ARENA_UNIFY.md, which it unblocks (see PLAN_SCHEDULER.md's step 0).

## Open: the declaration family, second pass — modifiers, SPAN/BLOCK, NEXO

The 2026-07-04 unification above (`NEW`/`SEAL`/`OWN`/`FREE`/`SET`/`ALIAS`)
landed, but a re-examination found three things it got wrong or left
undone. Recorded here, not started.

### Most of the modifier vocabulary is dead

Counted across every `.re` file:

| modifier   | real uses |
|------------|-----------|
| `\| OWN`    | 300       |
| `\| ANON`   | 8         |
| `\| NOLINK` | 7         |
| `\| SET=`   | **0**     |
| `\| SEAL`   | **0**     |
| `\| FREE`   | **0**     |
| `\| ALIAS`  | **0**     |

`| SET=` is dead for a concrete reason worth keeping: plain `SET X 5`
**already creates the name if it does not exist** (`loader.py`: `if name
not in self.symbols: alloc`). So the modifier duplicates what the bare
command already does — the unification added a second way to say
something that needed no second way.

**Decision:** `SET`, `SEAL`, `ALIAS` go back to being standalone
commands that create their own name when missing — which is what `SET`
already does today. `OWN` stays a modifier (300 uses). `FREE` stays a
modifier too: it is deliberate groundwork for the per-thread-personal
work (Pattern W / PLAN_SCHEDULER.md), not dead weight — it is the
symmetric opposite of `OWN` and will be used when that lands.

### SET does not absorb SEAL — and the reason is not what it looked like

An earlier attempt to fold `SEAL` into `SET` argued they differ only by
*when* they run (creation vs. later rewrite). That argument is wrong:
`SET` creates too, so timing does not separate them.

The real difference is *what* is written:

- `SET X Y`  → `word(X) = word(Y)` — copies Y's **value**
- `SEAL X Y` → `word(X) = addr(Y)` — writes Y's **address**

Two different actions. `SEAL` does not reduce to `SET`.

### BLOCK reduces to SPAN, and its aliases are a latent bug

`loader.py` states outright that `BLOCK name N = NEW name + NEW name_001
+ ... + NEW name_(N-1)`, allocating with `BLOCK_ELEM_SIZE = 2`. Checked
against real use:

- Every `BLOCK` whose indexing is visible is indexed by **1**, not 2
  (`IRIS_BUF` + `RA_IRIS_SIZE`, `DS_START` + `RA_DS_POS`, `BS_TOKBUF`
  + `RA_LOAD_TLEN`). So half of every BLOCK is simply unused —
  `BLOCK BS_TOKBUF_000 256` occupies 512 luces and addresses 256.
- The genuinely pair-shaped structure in the project — the symbol table,
  `2 words` per slot — is declared with **`SPAN`**, not `BLOCK`
  (`SPAN BS_HTAB_000 131072  /65536 slots * 2 words`). Pairing is
  already expressed by writing the product; BLOCK was not needed for it.
- The per-element aliases (`X_001`, `X_002`) have exactly one user in
  the whole project, and **it is broken**: `parser.re` writes
  pre-computed hash entries as `SET PBKT_011 <n>`, but `PBKT_011`
  resolves to **3698** while the block starts at **1358197** — outside
  it. `SET` created the name first (it auto-creates), then `BLOCK` saw
  the name taken and silently skipped that alias. The author intends to
  write slot 11 of the table and writes into unrelated memory instead.

**Decision:** delete `BLOCK`. Replace its 16 uses with `SPAN` of the
same size (they already address by 1, so the memory halves back). Fix
`PBKT` to use explicit offsets. Shape: `SPAN X Y N` — Y elements of N
luces each, N defaulting to 1 — positional like `LINK src rel tgt`, not
a modifier section, because SPAN has no modifiers and a modifier section
for zero modifiers is empty ceremony.

### NEXO wants to be general but is wired to one consumer

`NEXO name [relation] [alias]` already absorbed five commands
(`YAKU_NEXO`, `YAKU_NEXO_TERM`, `YAKU_NEXO_CMP`, `YAKU_NEXO_ARITH`,
`YAKU_NEXO_ALIAS`) — the right instinct. But its contract is "build a
packed string template **and optionally link to Yaku**": the general
shape (attach a couple of relations to a lux) is hardcoded to one
specific target. `Yaku_name`, `Yaku`, and `ForType` appear directly in
the macro's own definition.

The question to answer: is NEXO "declare a lux with N relations", with
Yaku merely its first caller — in which case the Yaku part belongs at
the call site, not in the macro? Or is it genuinely a Yaku thing wearing
a general name?

Not answered here. Needs its own pass, like the NOVA-family work above.
