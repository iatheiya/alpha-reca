# Session log: self-hosting / SEAL-alias investigation

This documents one long investigative session, start to finish, so the
reasoning chain isn't lost. Written to be read top to bottom as a story,
not just a changelog -- each section explains *why* the next one became
necessary.

All work happened in a working copy (`/home/claude/reca`), not the real
project. Nothing here is applied to the real project until the person
copies these files over themselves.

---

## 0. How this started

Task: fix `tether_re.txt` naming (trivial, person's own side), then work
through remaining `BUGS.md`/`ROADMAP.md` items. Landed on: self-hosted
`LOAD_MAIN` (running as real Reca bytecode) builds its own copy of the
project's graph, and `check_graph_equivalence.py` found 681 structural
mismatches between that self-hosted copy and Python's own build (the
"source of truth"). Investigating *why* is what this whole session
turned into.

---

## 1. CONFIRMED, FIXED, TESTED (safe to trust)

Each of these was individually verified: applied, rebuilt, ran
`run_all_checks.py` clean, confirmed the specific symptom it targets no
longer reproduces.

### 1.1 `LOAD_CMD_BLOCK` / `LOAD_CMD_SPAN`: address-collision bug
**File:** `wax.re`
**Symptom:** two completely unrelated names in the self-hosted graph
sometimes got the *same* address, corrupting one when the other's
content got written.
**Root cause:** `BLOCK`/`SPAN`'s own count-token reading never checked
that a token was actually found (or that it parsed to a valid positive
number) before calling `ALLOC_LUCES`. A count of 0 doesn't advance the
bump allocator (`K_CURSOR`), so the *next* real allocation silently
reused the same address.
**Fix:** added two guards (empty token, non-positive parse result) in
both commands' count-reading path, routing to a clean early return
(still skipping to EOL first, so the reader position stays correct)
instead of falling through to a zero-size allocation.
**Verified:** wrote a full-run tracer, confirmed 42 duplicate-address
allocations before the fix, 0 after.

### 1.2 `htable.re`: `HT_NAME_CMP` / `HT_NAME_STORE` stray `NOLINK`
**File:** `htable.re`
**Symptom:** the *same* name, looked up twice with the *same* hash,
got two different self-hosted addresses -- meaning name lookup never
found a previously-inserted entry, always treating it as new.
**Root cause:** in both `HT_NAME_CMP` and `HT_NAME_STORE`, a `CLEAR`
instruction (resets the loop counter to 0) was immediately followed by
a stray `NOLINK`, then the loop's own condition check. `NOLINK` blocks
the normal auto-chain from `CLEAR` to the next instruction; with no
alternative wiring, `CLEAR`'s own compiled "next" field stayed 0, and
the interpreter's own "next=0 → fall back to physically-next-instruction"
heuristic overshot past the real condition check, landing on a
different, unrelated label entirely. Every full-name comparison
therefore silently compared against garbage and always reported "not
equal", so every hashtable insert for an already-known name looked like
a fresh miss and got a brand new address.
**Fix:** removed the two stray `NOLINK`s.
**Verified:** built a permanent tool for this (`check_htable_reentrancy.py`,
see §3). Confirmed clean in isolation (no corruption during a single
call). Also directly inspected the compiled `next` field before/after:
before, `0`; after, the real address of the intended next instruction.

### 1.3 Same bug class, found systematically project-wide
**Files:** `htable.re` (the 2 above), `bfs.re`, `flux.re`, `wax.re`
(×6), `yaku.re`.
**How found:** `CLEAR` is the one instruction with no `Exit=`-style
"what comes next" field of its own -- it relies entirely on auto-chain.
Wrote a scanner: every `CLEAR <label> <reg>` line immediately followed
by `NOLINK` and then more code is a suspect. Found 11 total (2 already
covered above + 9 more). Verified all 9 by checking the compiled `next`
field was `0` (confirmed broken) before fixing, matching the real
intended target after.
**Fix:** same -- remove the stray `NOLINK` in all 9 remaining spots.
**Status:** all 11 fixed and left in place.

### 1.4 `SK_BACKFILL_BUF` too small
**File:** `wax.re`
**Symptom:** once forward-reference resolution (`LOAD_INTERN`) got used
more (via the `NOVA` op/argument fix, see §2), the backfill buffer
(sized for the old LINK-only use case, ~682 entries) overflowed by 7x,
corrupting adjacent memory.
**Fix:** `SK_BACKFILL_BUF` 2048 → 32768 luces.
**Verified:** confirmed via direct buffer-usage inspection (`base`/`ptr`
distance) before and after.

### 1.5 `LOAD_MAIN_SEAL_BOOT`: SEAL's own bootstrap dispatch wiring
**File:** `wax.re`
**Symptom:** `LOAD_CMD_NEWREF` (the handler behind both `SEAL` and
`NEWREF`) was never once dispatched to, anywhere, project-wide -- every
`SEAL X Y` line in the whole project was silently inert in self-hosted
execution (confirmed true in the *original*, unmodified project too --
not a regression from this session's other work).
**Root cause:** Python pre-allocates bootstrap command words (like the
literal word `SEAL`) purely so they can be *string-compared* in
Python's own code -- per `loader.py`'s own comment, they "never needed
a real address for that", so they are **not** self-referencing the way
a normal declared name is (their own word starts at `0`, not their own
address). The old bootstrap line, `Write El1=SEAL El2=LOAD_CMD_NEWREF`,
assumed self-ref and therefore double-dereferenced, writing
`LOAD_CMD_NEWREF`'s address into `aether[0]` instead of into `SEAL`'s
own slot.
**Fix:** get `SEAL`'s own address explicitly (`Idem`, not a dereference)
first, then write through that address directly.
**Verified:** confirmed `aether[addr(SEAL)]` now correctly holds
`LOAD_CMD_NEWREF`'s address, and (once a separate methodology mistake
was corrected, see §4) confirmed real dispatches to `LOAD_CMD_NEWREF`
now occur.

### 1.6 Step 1 of "true SEAL-alias unification" (immediate-resolution case)
**File:** `wax.re`, function `LOAD_CMD_NEWREF`
**Why:** self-hosted `SEAL X Y` was representing an alias as a *separate*
lux whose own word holds the target's address (an indirection),
instead of making `X`'s own hashtable entry resolve straight to `Y`'s
real address (which is how Python's own build effectively behaves, and
how `Voca` already tolerates aliases by design -- it always
dereferences once before jumping, so an indirection lux is invisible to
it). `JEQ`/`JZ`/`Jump`, unlike `Voca`, use the resolved address
directly with no extra dereference, so they broke on any alias target.
**Design decision (discussed with the person):** rather than teach
every jump-family consumer to special-case aliases (rejected as
non-homogeneous), remove the indirection at the source: make `X`'s own
hashtable entry point directly at `Y`.
**Implemented (this part confirmed working):** when `Y` is already
resolvable at the moment `SEAL X Y` is processed, `X`'s own name/hash
gets a second `HT_INSERT` (upsert) with `lid = Y`'s real address --
`X`'s originally-allocated lux becomes harmless, never-looked-up-again
orphaned memory.
**Verified:** confirmed via direct dispatch-count tracing that this
code path fires and is reachable.

---

## 2. FOUND AND FIXED, BUT PART OF A STILL-INCOMPLETE FEATURE

### 2.1 Step 2 of SEAL-alias unification (deferred/forward-reference case)
**Status: implemented, but not yet fully verified end-to-end.** This is
the common case (the person's own project comments note most real
`SEAL` usage is a forward reference).

**What was built:**
- `SK_SEAL_NAMEPOOL` (persistent, append-only pool for pending alias
  names -- `SK_NEWREF_NAMEBUF`, the scratch buffer used by the
  immediate case, gets reused per-call and isn't safe for names that
  must survive until backfill runs later).
- Extended the shared backfill-buffer format with a leading "kind" tag
  (1 = existing "write resolved value to address" behavior, 2 = new
  "re-insert this name's own hashtable entry once its target resolves").
- `LOAD_APPLY_BACKFILL` now branches on kind; kind=2 re-runs `HT_INSERT`
  for `X`'s own saved hash/name/length once `Y`'s hash resolves.

**Bugs found and fixed along the way (each one real, each one blocked
progress until fixed):**
- Naming: `SK_ALIAS_NAMEPOOL` (original name) caused a large,
  wide-ranging compile corruption (`check_op_identity.py`: ~425
  mismatches, starting from `LOAD_MAIN` itself). Renaming to
  `SK_SEAL_NAMEPOOL` fixed the vast majority of it. The *exact*
  mechanism was never fully pinned down (didn't chase it further since
  the rename works and is also the better name anyway), but is likely
  some coincidental interaction with how Python's loader specially
  treats bootstrap-adjacent names.
- Pool sizing: 4096 bytes overflowed (many pending aliases accumulate
  across the whole run); tried 65536 next, which reintroduced *worse*
  corruption than 4096 did (a different, still-unexplained numeric
  coincidence -- 65536 also equals `SK_HT_SIZE`, may be related, not
  confirmed); settled on 16384, which is clean of that specific issue.
- Register clobbering: `LOAD_CN_INTERN` (a pre-existing, shared helper
  used to declare `X`'s own name) calls `LOAD_LCOUNT_GET` internally,
  which uses `RA_LOAD_HASH`/`RA_LOAD_TLEN` as its own scratch --
  clobbering them before returning. The new code was saving `X`'s own
  hash/length *after* calling `LOAD_CN_INTERN`, always capturing
  garbage (the hash seed, length 0) instead of `X`'s real values. Fixed
  by moving the save to immediately after reading `X`'s token, before
  `LOAD_CN_INTERN` runs.

**What's still open:** after the register-clobbering fix, most calls to
`LOAD_CMD_NEWREF` observed during tracing had `RA_LOAD_TLEN == 0` for
`X`'s own name at entry -- meaning the *dispatch itself* is landing on
spurious/non-command lines, not that Step 2's own logic is wrong. This
turned out to be a separate, deeper bug (§3).

**One more bug found and fixed in this same code (after re-verifying
state, see §3's addendum):** when `X`'s own name came back empty
(`len=0` -- itself a downstream symptom of §3's comment-boundary bug),
the deferred copy-loop's own termination check (`TMP2==LEN_SV`, i.e.
`0==0` on the very first iteration) was not reliably firing, causing a
runaway loop that corrupted memory until it eventually overwrote its
own instructions and crashed. Root mechanism not fully pinned down (may
be state left inconsistent by whatever spuriously invoked `SEAL` in the
first place, rather than a plain JEQ bug), but the fix is safe either
way: added an explicit guard skipping the whole copy-and-backfill path
whenever `X`'s name length is `0`, rather than relying on the loop
condition to self-terminate correctly on an all-zero first check.
**Verified:** `run_all_checks.py` fully clean, self-hosting reaches a
*stable* 26/42 across 4 consecutive fresh rebuilds (previously varied
11-22/42 run to run -- this fix also removed that non-determinism, at
least for now).

### 2.2 Doubling `AETHER_SIZE` did not help -- led to a much bigger finding
Self-hosting still stops at 26/42 after the above fix, cleanly (no
crash -- the interpreter halts itself via `AC_G_HALT`, the allocator's
own "about to exceed available space" guard). Doubling `AETHER_SIZE`
(8M → 16M luces, `symphony.py`) had *no effect* -- stopped at almost
exactly the same step count either way. This ruled out genuine memory
exhaustion and led to the real cause:

**`RA_FLUX_BOTTOM` (the register holding where the reserved "flux"
region starts) itself was corrupted to `69` instead of its real value
(~16.6M)** -- so the allocator's own guard check was comparing against
a near-zero boundary and halting almost immediately regardless of how
much real space existed.

Traced the corruption to `LOAD_CMD_NEWREF`'s own first copy loop (the
one saving `X`'s name into `SK_NEWREF_NAMEBUF`, from Step 1). Added the
same `len=0` guard there too (same reasoning as §2.1's fix) -- this did
**not** fully resolve it. Traced further: with the guard in place, the
loop's own length value was a perfectly ordinary `21` (not `0`, not
huge) -- yet the write still landed near address 82 (`RA_FLUX_BOTTOM`'s
own address). Direct inspection at the write site showed **the real
culprit is one level up**: `SK_NEWREF_NAMEBUF`'s *own* self-reference
had itself been corrupted to `65` (instead of its real address,
~1.4M) -- so a perfectly well-bounded, correctly-terminating loop was
still writing to the wrong place, because its *destination base
address* was already wrong before the loop ever started.

**This is not a new, SEAL-specific bug.** `wax.re` and `htable.re`
already contain three separate, previously-fixed instances of exactly
this shape, each explicitly labelled "See BUGS.md Pattern Z":
`HT_NAME_POOL_PTR` (an orphaned init instruction nothing ever called,
left at `0`), `SPAN`'s own self-hosted dispatch (never wired up at
all, word held garbage), and `HT_NAME_STORE` (a shared register
clobbered by a called subroutine). All three of those *also* describe
low, garbage values landing specifically on address 82 or nearby "by
allocation-order coincidence." This session's finding is a **fourth**
occurrence of the identical shape, this time hitting a register this
session itself declared (`SK_NEWREF_NAMEBUF`).

### 2.3 Pattern Z's real root cause was already found and agreed -- just not implemented
Searched the whole project for "Pattern Z" (50+ references) and found
`BUGS.md`'s own full write-up: root cause **fully confirmed by direct
execution tracing** in an earlier session (2026-07-05). Summary: unlike
Python's own build (a strict four-phase design where every declaration
anywhere is guaranteed visible before any use is processed, regardless
of file order), self-hosted `LOAD_MAIN`'s own file processing has **no
declare-before-use barrier across files** -- files are read with some
interleaved/round-robin scheduling (deliberate groundwork for a future
real scheduler, not an accident), so a file *using* a name declared in
a different file can get its turn before that declaration exists yet.

**The fix was already agreed, just never implemented:** extend the
existing Wave 0 prepass (until this session, it only handled `LINK`
lumen-counting and this session's own `NOVA` pre-sizing) to register
*every* name declared via the exact same command set Python's own
`_BOOTSTRAP_CMDS` treats as "declares a name": `NEW, SET, LINK, ALIAS,
NOVA, BLOCK, SPAN, NOLINK, SEAL, OWN, FREE` -- not a hand-picked,
guessed subset (the original author explicitly flagged that risk).

**This session's own earlier "Variant A" work (the `NOVA`-only prepass
extension) was, in hindsight, a partial implementation of this exact,
already-agreed plan** -- it only covered `NOVA`, which is why
`SK_NEWREF_NAMEBUF` (declared via `BLOCK`, not `NOVA`) remained
vulnerable to being referenced before its own declaration.

**Implemented this session:** extended the prepass's byte-dispatch to
also recognize and pre-register `NEW`, `SET`, `SEAL`, `SPAN`, and
`BLOCK` declarations (reading the declared name, then a plain
`BS_INTERN` -- the default 1-lux size is correct for all of these,
unlike `NOVA` which needed the dedicated `BS_INTERN_NOVA` sizing).
Deliberately left out this round: `NOLINK` (not a name declaration at
all -- included in `_BOOTSTRAP_CMDS` for other reasons), `LINK`
(already handled, for lumen-counting rather than name pre-registration
-- its own targets are existing names, not new declarations), `ALIAS`/
`OWN`/`FREE` (confirmed elsewhere in this codebase to have no real
self-hosted dispatch handler yet at all, so pre-registering them would
be speculative work with nothing yet to connect to).

**One bug hit while implementing, fixed immediately:** the new `BLOCK`
branch used `NOVA ... BS_READ_TOKEN` (a plain op form) instead of `RVOCA
... BS_READ_TOKEN` (`BS_READ_TOKEN` is a function call, not an op
name) -- caused an immediate, obvious crash (self-hosting dropped to
7/42). Fixed by correcting to `RVOCA`.

**Result, verified across 3 consecutive fresh rebuilds:** `run_all_
checks.py` fully clean, self-hosting stable at 26/42 (unchanged from
§2.1's fix -- no regression, and no further improvement in *files
completed* yet either). `SK_NEWREF_NAMEBUF`'s own corruption is
confirmed reduced but not eliminated: it now starts life correctly
(`1448449`, its real self-ref) and stays correct for roughly 10 million
steps before still eventually getting corrupted to a small garbage
value by (still) the same copy loop -- i.e. the fix measurably shrank
the window during which this register is vulnerable, but didn't close
it entirely.

### 2.4 `SK_NEWREF_NAMEBUF`'s real bug: not Pattern Z at all, a genuine design flaw
Traced §2.3's remaining corruption to completion. **It was never
Pattern Z (a wrong/uninitialized pointer) for this specific register --
it was a real, distinct design mistake**, confirmed directly: the copy
loop's own destination computation used `SK_NEWREF_NAMEBUF` (the raw
buffer name, which self-references its own address, `SEAL
SK_NEWREF_NAMEBUF SK_NEWREF_NAMEBUF`) directly as the base for offset
arithmetic. Since the loop's own index starts at `0`, its *first*
iteration writes to `SK_NEWREF_NAMEBUF`'s own address -- overwriting
its self-ref (the thing every later "where is this buffer" read
depends on) with real data (the copied name's own first character).
Every buffer in this codebase that needs this pattern safely
(`SK_BACKFILL_BUF`, `SK_SEAL_NAMEPOOL`) already avoids it by keeping a
*separate* base-pointer register (`SK_BACKFILL_BASE`, `SK_SEAL_
NAMEPOOL_PTR`) that's set once and never itself used as data storage --
`SK_NEWREF_NAMEBUF` was the one buffer added this session that didn't
follow that convention.
**Fix:** added `SK_NEWREF_NAMEBUF_BASE`, initialized once at `LOAD_
MAIN`'s own startup (same pattern as the other two), and repointed all
three usage sites (the immediate-case copy loop's destination, the
deferred-case copy loop's source, and the `HT_INSERT` name-address
field) to use it instead of the raw buffer name.
**Verified:** `RA_FLUX_BOTTOM` (the specific victim chased through
§2.2/§2.3) now holds its correct value (`16646144`) after a fresh
build -- confirmed fixed, not just reduced.

**But this shifted addresses, and a *different* Pattern-Z-shaped crash
appeared:** self-hosting dropped to 14/42. Traced it: `LOAD_CMD_
UNKNOWN`'s own command-word dispatch (`LOAD_CU_BCALL`) tried to `Voca`
to an address resolved for the text `RA_FMT_BYTE` -- not a command at
all, an ordinary register (`fmt.re`: `NEW RA_FMT_BYTE | OWN`, used two
lines later as `SWITCH RA_FMT_BYTE`'s own subject argument). This means
the *reader itself* momentarily lost track of where one line/construct
ends and the next begins, treating `SWITCH`'s own argument as if it
were a fresh line's first word. **This is very likely the same root
class as §3 (the comment-boundary bug)** -- both are "the reader thinks
a new top-level command is starting when it's actually still inside a
different construct" -- just triggered by `SWITCH`'s own body/argument
handling this time instead of a `//...//` comment. Not chased to its
own root cause this round (would need the same depth of tracing as
§3), but this is a concrete, useful data point: **§3 may not be
lexer-comment-specific at all -- it may be a single, more general
line/construct-boundary tracking gap**, worth investigating as one
unified question rather than two separate bugs next time.

**Where this leaves things:** `check_graph_equivalence.py` re-run at
this session's final state: 204 mismatches (down from the original
684) -- real, if not directly comparable, since self-hosting still
doesn't reach 42/42 (some of the original 684 simply can't be checked
yet because that part of the graph isn't built at all in a truncated
run).

### 2.5 The prepass's own SEAL-handling had the same flaw as the original bug -- fixed
Chased the 14/42 regression from §2.4 to a precise, confirmed root
cause: `SWITCH` (a builder macro, `SEAL SWITCH SWITCH_START` in
`macros.re`) resolved to a garbage self-ref stub when `fmt.re`'s own
`SWITCH RA_FMT_BYTE` line ran before `macros.re`'s real `SEAL`
declaration had been processed. Confirmed via direct tracing: `SK_CMD_
ADDR` was set from the literal token `SWITCH`, and the address it
resolved to held mostly self-ref content (a leftover, un-rebound
placeholder). Root cause: the prepass's own `SEAL` branch (added in
§2.3) only did a bare `BS_INTERN` on `X` -- a plain self-ref
placeholder, which is fine for a *data* register (harmless until
overwritten) but actively wrong for anything meant to be *executed*,
since jumping to a self-ref stub jumps into its own uninitialized
content.
**Fix:** the prepass's `SEAL` branch now calls `LOAD_CMD_NEWREF`
directly instead of doing its own simplified registration -- reusing
the already-correct immediate/deferred handling from Steps 1/2, rather
than a second, weaker implementation of the same idea. Confirmed safe
to call twice (Wave 1's real pass later is an idempotent upsert).
**Verified:** the specific `SWITCH`/`RA_FMT_BYTE` crash is gone,
`run_all_checks.py` clean, stable across 3 rebuilds (though self-
hosting completion moved to 19/42, see §2.6 for why).

### 2.6 A deeper, distinct problem: `LOAD_RKVA_RESOLVE`'s own bootstrap self-reference
With §2.5's fix in place, self-hosting now stops at a *different* wall:
`LOAD_RKVA_EXIT` -- **the exact example this whole SEAL investigation
started from**, several conversation turns ago. This is not another
Pattern Z victim in the usual sense. `LOAD_RKVA_RESOLVE` (the shared
function that resolves *every* builder-macro argument, including
`JEQ`'s own target) has, inside its own implementation, a `JEQ`
instruction whose own target is `LOAD_RKVA_EXIT` -- another label
*inside `LOAD_RKVA_RESOLVE` itself*. Compiling that inner `JEQ`
requires resolving `LOAD_RKVA_EXIT` -- which requires `LOAD_RKVA_
RESOLVE` to already be working. This is a genuine bootstrap paradox,
not a simple ordering gap that a prepass extension can close on its
own.

Confirms a limitation of the alias-unification approach worth stating
plainly: Steps 1/2 make a name's *hashtable entry* resolve correctly
once its target becomes known (immediately or via backfill). But `JEQ`
(unlike `Voca`) has no runtime dereference at all -- its own compiled
Exit field is a value baked in once, at the moment its argument gets
resolved. If that resolution happens *before* the deferred backfill
ever gets a chance to fire (which, for a self-referential case like
this, may never happen through any normal file-processing order), the
compiled instruction is permanently stuck with whatever it got at
compile time -- unification of the *name* doesn't retroactively fix an
*already-compiled instruction* that read the name too early.

This matches a specific, already-flagged (not by this session) open
problem: `wax.re`'s own comments around `LOAD_RKVA_RESOLVE` mention
"need for a two-phase Wave-A/Wave-B0 split" as not yet done -- this is
very likely exactly what would be needed to fully resolve this
particular case, and is a materially bigger undertaking than anything
else in this log (a genuine bootstrapping redesign, not a small,
contained fix). Not attempted this session -- flagged clearly instead.

### 2.7 Applied the same two-phase pattern used at Wave 0.5→1 to Wave 0 itself -- fixed SWITCH for good
The person suggested looking at how `parser.re`'s `PS_MAIN` avoids this
class of problem (pre-computed hashtable entries for its own fixed
keyword set, `PBKT_0xx`), and asked whether a general, reusable
mechanism made sense. `PS_MAIN`'s own trick doesn't generalize directly
(it's a hand-maintained, hardcoded table -- fine for a handful of
permanently-fixed language keywords that never move, fragile for
arbitrary project symbols whose addresses shift on every edit). But it
pointed at the right *shape* of fix: this codebase already has a
proven, general answer to "declare far ahead of any possible use" --
the exact two-phase pattern already used at the Wave 0.5 → Wave 1
boundary (`LOAD_MAIN_DBF`, `wax.re`): scan *every* file's declarations
first, run backfill once, only *then* let anything start using names.

**What was actually missing:** the prepass (Wave 0) itself didn't
follow its own codebase's established pattern. It scans all 42 files
once, and its own `SEAL` handling (added in §2.5) tries to resolve each
alias *as it goes*, within that same single pass -- meaning an alias
whose target lives in a not-yet-scanned file was still vulnerable,
exactly the same shape of bug being fixed everywhere else in this log,
just one level earlier.

**Fix:** added a dedicated `LOAD_APPLY_BACKFILL` call immediately after
the prepass has scanned every file (before Wave 0.5 even starts) --
the same call already used at the Wave 0.5 → 1 boundary, just moved one
phase earlier. Any `SEAL` alias the prepass had to defer (because its
target wasn't registered *yet within that same scan*) now gets resolved
here, guaranteed, before anything -- including Wave 0.5's own
processing -- ever tries to use it.

**One bug hit while wiring this in, fixed immediately:** the new
`RVOCA ... LOAD_APPLY_BACKFILL` call relied on auto-chain to reach
`LOAD_MAIN_DSTART` (Wave 0.5's own start) across an intervening comment
block -- same bug shape as the very first fix in this whole log
(`htable.re`'s stray `NOLINK`): the compiled `next` field was `0`,
falling back to `pc+NOVA_SIZE`, which coincidentally landed back inside
the prepass loop's own body -- causing a genuine infinite reprocessing
loop (self-hosting display showed `2774/42`, i.e. the 42-file scan
re-running roughly 66 times before hitting the step budget). Fixed by
adding an explicit `Jump Exit=LOAD_MAIN_DSTART` right after the call,
matching the *other* existing `LOAD_APPLY_BACKFILL` call site's own
already-correct style (which also uses an explicit `Jump` immediately
after itself, not auto-chain across a comment).

**Verified:** the specific `SWITCH`/`RA_FMT_BYTE` crash from §2.4/§2.5
stays fixed, `run_all_checks.py` clean, stable 19/42 across 3 rebuilds.

**Confirmed, precisely, that `LOAD_RKVA_EXIT` (§2.6) is genuinely
different and this fix does not (and structurally cannot) touch it:**
re-ran the crash trace with this fix in place -- still crashes at the
exact same place, `LOAD_RKVA_EXIT`. This is expected and consistent
with §2.6's own diagnosis: that case isn't "the target hasn't been
scanned by the prepass yet" (which this fix solves) -- it's "compiling
this specific `JEQ` requires `LOAD_RKVA_RESOLVE`, the function this
`JEQ` is *part of*, to already exist," a genuine circularity no amount
of "scan everything first" can resolve on its own. §2.6's own
assessment (needs the two-phase Wave-A/Wave-B0 split, a real
architectural undertaking) stands as the accurate scope for that
remaining piece.

---

## 3. FOUND, NOT YET FIXED: comment-boundary bug in the lexer

**Confirmed real** (not a methodology artifact -- ruled that out
carefully): a documentation example inside a properly-opened-and-closed
`//...//` block comment in `htable.re` --

```
//word = handler Lux ID, jumped to when HT_INSERT finds no free slot.
Default: HT_OVERFLOW_VEC = End (halt). Override via:
SEAL HT_OVERFLOW_VEC my_handler
This makes overflow handling reversible/customisable per Reca convention.//
```

-- gets read and dispatched by self-hosted execution as if
`SEAL HT_OVERFLOW_VEC my_handler` were real code. Confirmed via a
byte-exact Python re-implementation of the comment-skipping state
machine that the comment boundaries in the source text are themselves
correctly paired (this is not a malformed-source problem). The bug is
in the *runtime* comment-skip mechanism itself.

**Suspect, not confirmed:** `SK_BLOCK_CMT` (`wax.re`, "0=normal,
1=inside multi-line block comment") is declared with a clear intended
purpose in its own comment, but is never read or written anywhere else
in the codebase -- purely dead. Whether wiring it up is *the* fix, or
whether the real mechanism (`BS_BLOCK_SKIP` in `lexer.re`, which reads
byte-by-byte until a closing `//` regardless of line boundaries, and
looks correct on inspection) has its own separate bug, was not
determined with confidence. `BS_READ_BYTE` also has a completely
separate "redirect mode" (reads from a pre-built buffer instead of the
live file, used for reprocessing already-constructed macro-body text)
whose own EOF signal could plausibly be confused with real EOF if it
were active at the wrong time -- checked and ruled out for this
specific case (`RA_REDIRECT_BASE` was 0 throughout), but the mechanism
itself is worth keeping in mind for any *other* comment-leak sighting.

**Attempted and failed:** tried to establish the exact absolute file
offset where the leak happens, to compare directly against the
Python-side comment-boundary simulation. The tracking method (counting
"`RA_LOAD_RPOS` decreased → buffer refilled") is unreliable once
multi-pass/interleaved file (re)processing is in the picture -- it
produced a position (37568) larger than the entire file (18177 bytes),
which is impossible, meaning the counting logic itself conflates
genuine buffer refills with the file simply being reopened/reread as
part of self-hosted `LOAD_MAIN`'s own multi-pass structure. A more
reliable position-tracking method is needed before this can be
pinpointed further -- e.g. instrumenting `BS_RB_FILL`'s own `SYS_READ`
call directly (it is the one unambiguous "a real refill just happened"
event) rather than inferring it from `RA_LOAD_RPOS` wrapping.

**Why this matters beyond SEAL:** any sufficiently long documentation
comment containing example code anywhere in the project could trigger
the same misdispatch. This is likely a contributing (not necessarily
the only) cause of some of the broader self-hosting flakiness observed
all session, separate from anything about SEAL specifically.

**Addendum (state re-verified after a conversation interruption):**
rebuilding from *completely unchanged* source gave a notably better
result once (22/42 files, only 5 `check_op_identity` mismatches) and
then, on eight immediately-following rebuilds, a consistently worse one
(11/42 files, 425 mismatches, stopping cleanly -- no crash, no step-limit
hit, just ~10.8M steps then naturally done -- at `derived.re`, file
index 11). Source was not touched between any of these. This means the
self-hosted run itself is not fully deterministic run-to-run -- worth
keeping in mind for *any* future debugging in this area: a single
"looks better now" result is not enough to trust a fix without
rebuilding at least 2-3 times. The comment-boundary bug (this section)
is not currently reproducible in the same run, since the run no longer
reaches `htable.re` (file 18) at all -- whatever now stops the run at
`derived.re` needs to be understood first.

---

## 4. A methodology lesson worth keeping (cost real time twice)

Two separate wrong turns this session came from the same root mistake:
checking "did the program counter ever become exactly address X" using
an address obtained the *naive* way, without accounting for:

1. **SEAL aliases have their own address, distinct from their target's
   address, even in cases that matter for tracing** (e.g.
   `LOAD_DISPATCH_CORE` is itself a SEAL alias for `LOAD_DC_TLENCK` --
   checking "pc == addr(LOAD_DISPATCH_CORE)" will *always* read 0 hits,
   correctly, because nothing ever sets pc to the alias's own address;
   real execution flows through the dereferenced target instead). This
   produced a false "SEAL command dispatch is completely broken"
   conclusion for a good part of the session, before being caught and
   corrected.
2. **Symbols must come from the same build being traced.** Calling
   `load_symbols()` (reads whatever `reca.sym` is currently on disk)
   *before* triggering a fresh `freeze()` risks comparing addresses
   from an old build against a new one, especially after any source
   edit (which shifts nearly everything downstream). Always get
   symbols from the loader/build you're actually instrumenting.

Both are now understood and were the reason two separate "the whole
SEAL mechanism is fundamentally unreachable" scares turned out to be
nothing of the kind.

---

## 5. A reusable tool left in the project

**`check_htable_reentrancy.py`** (new, permanent, documented in its own
docstring) -- precisely brackets a *specific* `HT_LOOKUP`/`HT_INSERT`
call instance (using `RA_SP` call-stack depth, not just "some call to
this address happened somewhere"), to check whether the shared
token/name buffers it depends on change *during* that exact call
window. Has two modes: `reentrancy` (corruption-during-a-single-call
detector) and `sequence` (logs every call instance for a target name in
order, with phase and file index). Built specifically to investigate
the "same name gets a new address on every lookup" class of bug, but
written to be reusable for any future instance of it.

---

## 6. Current file state (working copy only)

Modified so far, all still in place:
- `wax.re` -- BLOCK/SPAN guards, 6 NOLINK fixes, `SK_BACKFILL_BUF`
  size, `LOAD_MAIN_SEAL_BOOT` write fix, `LOAD_DECLARE_TOKEN`/
  `LOAD_RESOLVE_TOKEN` split (from the earlier, more contained
  hashtable-safety work), `LOAD_CMD_NEWREF` Step 1 + Step 2, backfill
  buffer kind-tag format + `LOAD_APPLY_BACKFILL` rewrite,
  `SK_SEAL_NAMEPOOL` + its init.
- `htable.re` -- 2 NOLINK fixes (`HT_NAME_CMP`, `HT_NAME_STORE`).
- `bfs.re`, `flux.re`, `yaku.re` -- 1 NOLINK fix each.
- `intern.re` -- `BS_INTERN_NOVA` helper (from the earlier NOVA-presizing
  prepass work).
- `registers.re` -- one stale comment updated.
- `check_htable_reentrancy.py` -- new file.

Build status as of this log: `run_all_checks.py` still reports the ~5
`check_op_identity.py` mismatches from §2.1 (the pool-sizing/ordering
saga) as not-yet-fully-clean, and self-hosting completes roughly
22/42 files rather than 42/42 -- **this is expected and matches where
the investigation currently stands**, not a silent new regression.

---

## 7. Suggested next steps, in priority order

1. **`LOAD_RKVA_RESOLVE`'s own bootstrap self-reference (§2.6, confirmed
   still the sole remaining wall as of §2.7) is the real next question**
   -- and it's an architecture decision, not a bug hunt: implement the
   "two-phase Wave-A/Wave-B0 split" already mentioned in `wax.re`'s own
   comments, or find a narrower fix specific to this one recursive
   case (e.g. hand-compiling just this one `JEQ` with a Python-native
   escape hatch, similar in spirit to how `parser.re`'s `PS_MAIN` uses
   pre-computed values for its own fixed keyword set -- worth
   discussing whether that idea applies *here specifically*, even
   though it didn't generalize to the broader SEAL problem). Worth
   discussing the shape of the fix before attempting -- this is
   architecturally bigger than anything else in this log.
2. Investigate §3 (the comment-boundary bug) on its own -- separate
   from Pattern Z now that §2.4's `SWITCH`-argument confusion turned
   out to have a fully diagnosed and fixed cause (§2.5/§2.7), §3
   itself is still open and un-investigated to a root cause.
3. Consider extending the prepass to the remaining `_BOOTSTRAP_CMDS`
   entries once `ALIAS`/`OWN`/`FREE` get real self-hosted dispatch
   handlers (they don't yet) -- low priority until that happens.
4. Re-run `check_graph_equivalence.py` again once self-hosting reaches
   42/42 (or close), for an apples-to-apples comparison against the
   original 684.
5. Only then: chase the still-unexplained naming/sizing coincidences
   noted in §2.1 (the `ALIAS`-substring and `65536`-size effects) if
   they still matter.

---

## 8. Discussed but not (yet) in code -- the reasoning that risks being lost

Everything above records *what was changed*. This section records what
was **talked through** but never turned into a diff, because the
session kept getting pulled into the next concrete blocker. These are
not leftovers to discard -- several are more valuable than the fixes.

### 8.1 The `parser.re` idea, and what it actually taught us
The starting observation: `parser.re`/`PS_MAIN` contains special logic
(a pre-computed `PBKT` dispatch table) whose whole purpose is to
**avoid recursively loading the very machinery needed to do the
loading**. The obvious question was whether the same trick could break
`LOAD_RKVA_RESOLVE`'s bootstrap paradox (§2.6).

What was established by looking at it properly:
- **Literally, no.** `parser.re`'s table is hand-computed for a fixed,
  never-changing keyword set (`NEW`/`SET`/`NOVA`/...). It cannot
  generalize to arbitrary project names, because every source edit
  shifts addresses. Copying that shape directly would be a maintenance
  trap.
- **In spirit, yes -- and it worked.** The *principle* underneath it is
  "establish everything the machinery needs **before** the machinery
  runs, so it never has to resolve itself mid-flight." That principle
  already existed in this codebase at the Wave 0.5→Wave 1 boundary
  (scan all declarations across all files → one `LOAD_APPLY_BACKFILL`
  → only then allow use). §2.7 is exactly that principle applied one
  level earlier, to Wave 0. It permanently fixed the `SWITCH` class.
- **Its limit is now known experimentally, not by guess.** §2.6's
  `LOAD_RKVA_EXIT` is untouched by it, because that isn't an ordering
  problem at all -- it's genuine self-reference. No amount of
  "declare earlier" fixes "this function needs itself to compile
  itself."

So the `parser.re` idea was not a dead end: it produced a real fix, and
it produced a **negative result that is itself a finding** -- it drew
the exact boundary between "ordering gap" (solvable by prepass/
backfill, now solved) and "bootstrap paradox" (needs the Wave-A/Wave-B0
split, or a narrow Python-native escape hatch for one instruction).

### 8.2 The universal anti-recursion function -- discussed, not built, still worth building
The stronger form of the idea, raised explicitly: rather than
re-implementing "don't recursively load this" ad hoc in each place
(`parser.re`'s table, the prepass's own guards, whatever §2.6
eventually needs), make it **one reusable Reca function** — a single,
named mechanism for "I am already loading X; do not re-enter."

Arguments in favour, recorded so they don't evaporate:
- **De-duplication.** The shape already exists in at least two places
  in different disguises (`parser.re`'s precomputed table; the
  Wave 0.5→1 barrier). §2.7 made it three. Three instances of one idea
  is the point where a shared implementation stops being speculative.
- **Uniform behaviour at every depth.** This is the project's own
  stated preference: a few simple, universal primitives composed into
  the specific, complex cases -- rather than a fundamental mechanism
  here and a different ad-hoc one there. Right now the anti-recursion
  idea is expressed *differently at every level*, which is precisely
  the divergence-of-ideas smell worth collapsing.
- **It would make §2.6 tractable without a redesign.** A general
  "in-progress" guard is a much smaller thing to reason about than a
  full two-phase Wave-A/Wave-B0 split, and might make the split
  unnecessary — or, if the split still happens, the guard is the
  natural primitive it would be built out of.

Honest counterweight, also recorded: **it wasn't strictly needed for
anything fixed this session.** Every concrete blocker resolved above
was fixed by other means. So this is a "build it because the
architecture wants it," not a "build it or nothing works." That's a
real distinction and shouldn't be blurred.

**Not searched exhaustively.** A grep for an existing implementation
was intended and never completed before the session ended. Before
building anything: check whether some in-progress/re-entrancy guard
already exists (`check_htable_reentrancy.py` was written this session
around exactly this *shape* of question, on the data side — worth
reading first, it may already encode the right vocabulary).

### 8.3 The meta-pattern: fixing one bug **creates** the next one's visibility
This is the single most important observation of the session and it is
easy to lose, because it's a property of the *process*, not of any one
bug.

Repeatedly, throughout: a bug would be found, fixed, verified — and
immediately a **new bug of the same shape** would appear, in a place
that had previously been unreachable. Not a regression. Not the fix
being wrong. The fix *let execution get further*, into code that had
never once run before and therefore had never been testable.

Concrete instances from this session alone:
- **Stray `NOLINK` / broken auto-chain**: found once in `HT_NAME_CMP`,
  then systematically in 11 sites (§1.2/§1.3) — and then *again*, a
  twelfth time, in §2.7's own brand-new backfill call, which sent the
  file counter to `2774/42`. The same bug class bit the fix for the
  bug class.
- **Pattern Z victims, serially**: `HT_NAME_POOL_PTR` → `SPAN`'s
  dispatch → `HT_NAME_STORE` → `SK_NEWREF_NAMEBUF`. Each became visible
  only once the previous was cleared.
- **The §2 chain**: `len=0` runaway loop → (fixed) → `RA_FLUX_BOTTOM`
  corruption → (fixed) → `SWITCH` garbage stub → (fixed) →
  `LOAD_RKVA_EXIT` bootstrap paradox. Four distinct walls, each one
  invisible until the one in front of it fell.

**Why this matters practically, not just philosophically:**
1. **"A new problem appeared" is usually progress, not failure.** The
   instinct to read a dropping file count (26/42 → 14/42 → 19/42) as a
   regression was wrong more than once this session. Each drop was a
   *newly reachable* pre-existing bug. Judge fixes by whether the
   specific symptom died and the checks stay clean — not by the
   headline number.
2. **The bug classes are finite and recur.** Auto-chain breakage,
   Pattern Z ordering, self-ref-as-base-pointer (§2.4), register
   clobbering by callees (§2.1). When something new breaks, check the
   known classes *first*; the answer has been in that list most of the
   time.
3. **"Can't test it yet" is a real category.** Large parts of this
   codebase have never executed. Absence of evidence of a bug in
   unreached code means nothing. Expect a fresh crop after every real
   fix, and treat the arrival of new bugs as a signal the previous fix
   landed.

### 8.4 Threads explicitly acknowledged and set aside (not forgotten)
- **`RA_HT_PROBE` double-pass-through-hashtable** — one of the three
  original `BUGS.md` tasks this session was meant to address. Never
  started. The self-hosting investigation consumed everything.
- **§3, the comment-boundary bug** — confirmed real, root cause never
  found. Briefly hypothesised to be the same root as §2.4's
  `SWITCH`-argument confusion; that hypothesis is now **dead** (§2.5
  gave `SWITCH` a different, fixed cause), so §3 stands alone,
  un-investigated. `SK_BLOCK_CMT` (declared, never used, dead) remains
  the prime suspect and remains unconfirmed.
- **`ALIAS`/`OWN`/`FREE` prepass coverage** — deliberately not done:
  no self-hosted dispatch handlers exist for them yet, so
  pre-registering would connect to nothing.
- **Two unexplained coincidences (§2.1)** — the `ALIAS`-substring
  effect (~425 mismatches, fixed by rename, mechanism never
  understood) and the `65536` pool-size effect (worse than 4096;
  possibly a collision with `SK_HT_SIZE`, unconfirmed). Both were
  worked *around*, not explained. Either could be a symptom of
  something still live.
- **`AETHER_SIZE` 8M→16M** — kept, but it was **not** the fix for
  anything. It was a hypothesis (memory exhaustion) that direct
  measurement disproved (§2.2). Harmless; not evidence of anything.
- **`check_graph_equivalence.py`: 684 → 191** — real progress, but
  **not a clean comparison**: 684 was measured at 42/42 self-hosting,
  191 at 19/42. Part of the graph simply isn't built in a truncated
  run and therefore isn't checked. The honest number is unknown until
  self-hosting completes.
