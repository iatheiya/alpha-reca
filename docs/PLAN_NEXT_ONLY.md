# PLAN: Universal lux mutability — `next` at position 1, no `bound`, no sentinel

Status: APPROVED. Prerequisite "step A" DONE (see below) — the packed-name
leak into Data lux slot 1 has been removed, so the layout is now clean to
build on. This plan (`next` at slot 1) is the next step.

Supersedes: PLAN_BOUND_NEXT.md (rejected — see that file for why `bound`
was dropped).

---

## Prerequisite step A — DONE (packed name removed from the lux)

Before this plan could be built cleanly, a latent bug had to go: named Data
symbols stored a packed-name pointer in **slot 1**, the very slot the lumen
list starts at (`RA_LM_OFFSET = 1`). A named symbol that was also LINK'd had
its first lumen fight the name pointer — `ADD_LUMEN` read the name as an
occupied `rel` and skipped past it. Facts established this session:

- The htable stores only `(hash32 << 32) | lux_addr` — names are NOT in it.
  Lookup is by hash. So "name lives in the htable" is false; the name lived
  only in the symbol lux, redundantly.
- The only reader of the lux name was `_rebuild_symbols_from_aether` in
  loader.py, a crash-only fallback — and it was already broken by the
  conflict above. In normal runs Python owns all names (source of truth).
- The name-write was smeared across THREE places: `LOAD_CMD_NEW`,
  `LOAD_CN_INTERN` (LOAD_CNI), and the never-called `BS_INTERN_NAMED`.

Decision (with author): names never live in a lux. They belong to the symtab
layer (today Python's dict; eventually the htable widened to
`(hash → addr, name)` — a single universal structure, not a per-lux slot).
Removed this session:

- `wax.re LOAD_CMD_NEW`: dropped `LOAD_CN_SAV/PACK/RST(old)/N1A/N1W`.
- `wax.re LOAD_CN_INTERN` (LOAD_CNI): dropped `LOAD_CNI_PACK/N1A/N1W`.
- `intern.re`: deleted dead `BS_INTERN_NAMED` whole.
- `loader.py`: deleted `_rebuild_symbols_from_aether` + `_read_packed_name`
  and the crash-branch call.

Result: Data lux is now a clean `[word, ...lumens..., 0]`; slot 1 holds the
first lumen (not a name). Verified: lint clean, invariants FAILS: 0,
comment-checker clean, 7/7 macro tests. This is the clean base for `next`.

Note: self-hosting was 40/42 at the end of step A due to a pre-existing
`AETHER_SIZE` overflow. That overflow was later root-caused to the oversized
symbol hash table and FIXED (htable shrink + allocator guard) — self-hosting
is now 42/42 (100%). Full detail in "Open questions" #3 below. Not caused by
step A's changes; confirmed unrelated by reverting each file.

---

## Core insight

The prepass (`Wave 0` in `wax.re`) already counts exact lumen counts per
name via `LOAD_LCOUNT_GET` / `SK_NOVA_LCNT`. Both allocators use:

```
Data lux size = 1 + 2*lcount + (lcount>0 ? 1 : 0)
NOVA  lux size = 7 + 2*lcount + (lcount>0 ? 1 : 0)
```

The `+1` at the end is the `OVERFLOW_REL` sentinel slot. Since the prepass
guarantees exact allocation, overflow never happens — confirmed empirically:
`OVERFLOW_REL` is never written anywhere in the frozen aether (0 occurrences
at runtime). The sentinel mechanism is completely dead code. We can remove
the `+1` and use that slot budget for `next` instead.

`next` is needed for **all** lux for mutability — the ability to add lumens
after initial allocation. This is not about whether a lux has lumens now,
but about preserving the option to add them later. Without `next`, you
cannot extend a lux at runtime without reallocating and patching all
references to it.

---

## New layout

### NOVA lux (NOVA_SIZE: 7 → 6)

```
slot 0: word   (self-referential addr — NOVA invariant)
slot 1: next   (0 = no extension, else addr of continuation lux)  ← MOVED from slot 5
slot 2: op     (operation primitive addr)                          ← unchanged
slot 3: e1     (first operand addr)                                ← unchanged
slot 4: e2     (second operand addr)                               ← unchanged
slot 5: exit   (result register addr)                              ← was slot 4 before? NO —
```

Wait — current NOVA slots per symphony.re:
```
slot 0: word
slot 1: op
slot 2: e1
slot 3: e2
slot 4: exit
slot 5: next   (currently here — MOVE to slot 1)
slot 6: pad    (the "lishny slot" — REMOVE)
```

New NOVA layout:
```
slot 0: word
slot 1: next   ← moved here from slot 5
slot 2: op     ← shifted (was slot 1)
slot 3: e1     ← shifted (was slot 2)
slot 4: e2     ← shifted (was slot 3)
slot 5: exit   ← shifted (was slot 4)
```

NOVA_SIZE = 6 (was 7). Removing `pad` (slot 6) and moving `next` to slot 1.

Lumen pairs (if any) follow at slots 6, 7, 8, 9, ... (was 7, 8, 9, 10, ...)

### Data lux

After step A the Data lux is `[word, ...lumens.., 0]` with lumen offset 1.
This plan inserts `next` at slot 1, pushing the lumen list to offset 2:

```
slot 0: word   (the actual value / lux id)
slot 1: next   ← NEW (currently slot 1 is the first lumen after step A)
slot 2+: lumen pairs [rel, exit, rel, exit, ...]   ← offset 1 → 2
```

Data lux size = 2 + 2*lcount (was, after step A: 1 + 2*lcount + sentinel)

For lcount=0: size 2 → word + next (no lumens)
For lcount=N: size 2 + 2N → word + next + N pairs

**Data lumen offset moves 1 → 2.** Every caller that sets the Data-lux lumen
offset to 1 must become 2. Found this session (6 sites):
- `wax.re:~631` `LOAD_CMD_NEWSET`: `Move El1=C_1 Exit=RA_LM_OFFSET` → C_2
- `wax.re:~1083` `LOAD_APPLY_LINKS`: reads offset from the prefix table
  written by `LOAD_CN_PFW` (`El2=C_1`). The prefix stored for a Data lux
  must become 2 (NOVA stays NOVA_SIZE). Update `LOAD_CN_PFW` `C_1 → C_2`.
- `symphony.re:~121` `SR_GLL`: `Move El1=C_1 Exit=RA_SR_OFFSET` → C_2
- `symphony.re:~170` `SR_WALK_ONE`: `Move El1=C_1 Exit=RA_SR_OFFSET` → C_2
- `macros.re:196` `WALK_ONE` (`WALK_N3`): `NOVAE ... Move El1=C_1 Exit=RA_SR_OFFSET`
  → C_2. Direct form (no `_REF` suffix on either operand) — unlike
  `WALK_NOVA_N3` (macros.re:241), which uses `C_7_REF`/`RA_SR_OFFSET_REF`
  because that macro's synthesized Move is doubly-indirect. Do not "fix"
  WALK_ONE to use a `_REF` form — the two macros are intentionally
  different shapes (checked this session; a `DATA_LUMEN_OFFSET_REF` was
  drafted and discarded as unneeded speculative infrastructure once this
  was understood).
- `macros.re:183` `LH` (`LH_N0`): `NOVAE ... Move El1=C_1 Exit=RA_SR_OFFSET`
  → C_2. **Also found to be dead weight, independent of this plan**: `LH_N2`
  immediately calls `SR_GLL` (`symphony.re:118`, doc-comment: "Used by LH
  macro"), and `SR_GLL`'s own first instruction already sets
  `RA_SR_OFFSET=C_1` before falling into `SR_FIRST_LM`. So `LH_N0`'s write
  is unconditionally overwritten a few steps later by `SR_GLL` itself —
  it never has an observable effect. Update the literal (1→2) for now to
  keep the tree consistent during this plan's migration, but consider
  deleting `LH_N0` entirely in a follow-up cleanup (LH would then be 3
  synthesized NOVAs instead of 4, saving one lux per LH call site). Not
  fixed this session — flagged, not touched, per "don't fix unrelated
  things mid-plan".
Also update the `RA_LM_OFFSET` doc-comment in `alloc.re:61` ("1=Data lux" →
"2=Data lux") and the `RA_SR_OFFSET` doc in `symphony.re:36`.

---

## Memory impact

From empirical measurement of the current frozen binary (~55K live lux):

- NOVA lux: save 1 slot each (removing pad, net effect of next relocation)
  - ~24K NOVA lux (estimate) → save ~24K slots
- Data lux with lumen (~54% of data lux, ~3.5K): no change
- Data lux without lumen (~46% of data lux, ~3.1K): +1 slot each → +3.1K slots

Net: meaningful savings. The no-lumen Data overhead (+3.1K) is dominated
by NOVA savings (~24K). Total aether footprint shrinks.

---

## What changes in code

### symphony.re / symphony.py — slot constants

```
SLOT_NEXT = 1   (was 5)
SLOT_OP   = 2   (was 1)
SLOT_E1   = 3   (was 2)
SLOT_E2   = 4   (was 3)
SLOT_EXIT = 5   (was 4)
NOVA_SIZE  = 6   (was 7)
```

This is the single most impactful change — symphony.py exports these
constants and they are imported everywhere. Changing them here cascades
correctly to all Python consumers.

symphony.re defines the same constants as Reca NEWSETs — update to match.

### wax.re — allocators

Remove the `+1` sentinel from both allocator formulas:

```
/Old:  1 + 2*lcount + (lcount>0 ? 1 : 0)
/New:  2 + 2*lcount
LOAD_CMD_NEW:  RA_ALLOC_COUNT = 2 + 2*lcount

/Old:  NOVA_SIZE + 2*lcount + (lcount>0 ? 1 : 0)
/New:  NOVA_SIZE + 2*lcount   (NOVA_SIZE is now 6)
LOAD_CMD_NOVA:  RA_ALLOC_COUNT = NOVA_SIZE + 2*lcount
```

The `JZ LOAD_CI_TCKZ / LOAD_CN_TCKZ` branches (the `lcount>0 ? +1 : 0`
sentinel logic) are removed entirely.

### alloc.re — remove OVERFLOW_REL entirely

- Remove `NEW OVERFLOW_REL`
- Remove `ALLOC_LUX_N` (allocator that writes the sentinel — now dead)
- Remove the sentinel-write in `ANN_OVWR` path
- Remove `OVERFLOW_REL` checks in `ADD_LUMEN` (`AL_OVCK`/`AL_OVERFLOW`)
- Remove `OVERFLOW_REL` checks in `REMOVE_LUMEN` (`RL_OVCK`/`RL_SHOVCK`)

### symphony.re — SR_* lumen walkers

Remove `OVERFLOW_REL` checks from:
- `SR_FIRST_LM` / `SR_NEXT_LM` (SR_FL_OVCK, SR_NL_OVCK)
- `SR_GLX` (SR_GLX_OVCK)
- `SR_COUNT_LM` (SC_OVCK)

These walkers now simply scan consecutive pairs until rel==0. Clean.

### accord.re — SCAN_LUMEN_OF

The "was LX" inline advance can remain as-is (it never used OVERFLOW_REL
handling anyway — see comment added this session). The NOTE about
OVERFLOW_REL in that comment can be removed once this plan is implemented.

### loader.py — Python-side lux building

All Python code that builds NOVA lux (Wave-B wiring, NEWREF restoration,
etc.) uses the symphony.py slot constants — these update automatically when
symphony.py is updated. Double-check any hardcoded `+ 5` (old SLOT_NEXT)
or `+ 1` (old SLOT_OP) patterns.

### interpreter.py

Uses symphony.py constants via `_slot()` helper — updates automatically.
Check `_run` loop for any hardcoded slot numbers.

### diag.py / trace.py / check_nova_sizing.py

Use symphony.py constants — updates automatically. `check_nova_sizing.py`
specifically validates NOVA_SIZE: update its expected value to 6.

---

## What does NOT change

- `LR`, `LT`, `LH` macros: these access lumen pairs at `NOVA_SIZE + offset`
  — NOVA_SIZE changes from 7 to 6, so the offset changes too. But the
  *formula* (`NOVA_SIZE + 2*N`) stays the same, so if these macros use
  `NOVA_SIZE` symbolically (not hardcoded 7), they update automatically.
  Check each one.
- `WALK_ONE` / `WALK_NOVA`: same as above — use `NOVA_SIZE`/`C_7` for
  offset. `C_7` is currently used as a proxy for NOVA_SIZE in WALK_NOVA
  (`El1=C_7_REF` in macros.re:241). After this change, NOVA_SIZE=6, so
  C_7 is wrong. WALK_NOVA needs `C_6` or a new `NOVA_SIZE_REF`. This is
  one place that needs careful auditing.
- `LINK_OP` / `UNLINK_OP`: call `ADD_LUMEN` / `REMOVE_LUMEN` which are
  updated above — these call sites don't change.
- All `.re` user-facing code (`fmt.re`, `io.re`, etc.): they don't touch
  slot offsets directly.

---

## Implementation order (when the time comes)

1. Update `symphony.py` constants (SLOT_NEXT, SLOT_OP, SLOT_E1, SLOT_E2,
   SLOT_EXIT, NOVA_SIZE). This is the single source of truth.
2. Update `symphony.re` NEWSETs to match.
3. Update `wax.re` allocator formulas (remove sentinel branch).
4. Update `alloc.re` (remove OVERFLOW_REL, ALLOC_LUX_N, overflow paths).
5. Update `symphony.re` lumen walkers (remove OVERFLOW_REL checks).
6. Shift the Data-lux lumen offset 1 → 2 at the 6 sites listed under
   "Data lux" above (LOAD_CMD_NEWSET, LOAD_CN_PFW, SR_GLL, SR_WALK_ONE,
   WALK_ONE/WALK_N3, LH/LH_N0), plus the two doc-comments.
7. Audit `WALK_NOVA` / `C_7_REF` usage — fix to use NOVA_SIZE (now 6).
8. Run `python3 -c "from loader import freeze; freeze()"` — expect
   many issues first time, fix systematically.
9. Run full diagnostic suite: `diag.py --lint`, `--invariants`,
   `--block-lint`, `--unwrapped`, `test_macros.py`, `check_nova_sizing.py`,
   `check_comments.py`.
10. Self-hosting must stay at 42/42, with `freeze()` printing `LOAD_MAIN
    complete.` (no caught exception at all -- this is the real bar, not
    just the file count). Reached stably this session via the separate
    self-hosted symbol table (`SK_HTAB_000`, see "Update" section above) --
    the htable shrink + allocator guard alone were not sufficient (self-
    hosting held at 40/42 with those two fixes only; confirmed by
    reverting each individually). Any drop = the offset/slot cascade
    was incomplete; use `diag.py --lost` and `--load-main` to localize.

---

## Open questions (resolve before implementing)

1. **`WALK_NOVA` and `C_7_REF`**: currently hardcodes `C_7` as NOVA_SIZE.
   After this change, NOVA_SIZE=6. Need `C_6_REF` or symbolic `NOVA_SIZE_REF`.
   The `C_6` constant already exists in `constants.re` — just need the REF.

2. **`preamble_template.ll` and `gen_compiler.py`**: generated LLVM IR uses
   NOVA layout. Check if slot offsets are hardcoded in the codegen.

3. **The pre-existing `AETHER_SIZE`-overflow `IndexError` is now FIXED** (this
   session, after the htable+guard work below). Root cause was found to be
   the symbol hash table: `BS_HTAB_000` was sized at 262144 slots but only
   ~6.7K are ever used (~2.6% load), and `BLOCK` allocates 2 luces/element,
   so the table alone consumed ~524K luces. Combined with the intentional
   two-graph bootstrap (Python + self-hosted `LOAD_MAIN` both build full
   graphs in one aether), that pushed the cursor past `AETHER_SIZE`.
   Two fixes landed:
   - `intern.re`: `BS_HT_SIZE`/`BS_HT_MASK`/`BLOCK BS_HTAB_000` shrunk
     262144 → 65536 (still ~10x headroom; frees ~393K luces).
   - `alloc.re` `ALLOC_LUCES`: added a real bounds guard (halt via `End`
     if the new cursor would reach `RA_FLUX_BOTTOM`), self-disabling when
     `RA_FLUX_BOTTOM` is still 0. Mirrors `ALLOC_FLUX_ZONE`'s existing
     `FLUX_TOP` guard — one bounds-check shape, two users.
   Result: **self-hosting reached 42/42 (100%) for the first time** this
   session (final cursor 221018, ~1.74M luces of headroom below the flux
   zone). NOTE: the guard's halt branch has not yet been exercised by a
   real run (cursor never reaches the limit now); it is written to mirror
   the proven flux guard, but a deliberate low-limit test of the halt path
   is still an open verification item.

   The deeper architectural point stands (author's call, Variant A): flat,
   unbounded, single-cursor memory has no notion of ownership/bounds. The
   htable shrink + guard fix the symptom; the real fix is bounded, owned
   memory regions (zones/arenas) so each graph — including self-hosted
   verification — lives in its own bounded region. `arena.re` exists but is
   currently DEAD CODE (no caller) and only marks/rewinds the cursor; it has
   no upper bound. Elevating bounded regions to a universal principle is the
   root fix; htable-size and the guard then become per-zone details. This is
   a fundamental shift pending author direction — see below.

4. **A stale duplicate `NEW P0_COLLECT_BODY`** was found and removed from
   yaku.re's "Stubs for unimplemented functions" block (was declared twice:
   once as a real, wired NOVA with real logic near the top of the file, and
   again as a leftover stub-placeholder near the bottom, from before the
   real implementation existed). Confirmed harmless before removal --
   Python's Wave-1a only allocates on first `NEW` of a name
   (`if name not in self.symbols`), so the second `NEW` was already a
   guaranteed no-op. Unrelated to this plan; fixed in passing because it
   was trivial and proven safe.

---

## Update: the real root cause behind the pre-existing crash — FIXED (Variant A, applied to the symbol table)

The htable shrink + allocator guard above (item 3) removed one specific
`AETHER_SIZE` overflow, but self-hosting still crashed with `IndexError`
after that fix — self-hosting held at 40/42, not 42/42, on every honest
re-run (the earlier "42/42" was margin-dependent, not a real fix; confirmed
by reverting each session file individually, the crash persisted every
time regardless of htable size, SPAN vs BLOCK, or the guard's presence).

**Root cause, traced to the exact mechanism** (via `trace.py --find-writer`,
per TOOLING.md's own recommended first step -- not hand-rolled scripts):
self-hosted `LOAD_MAIN`, building its own copy of `alloc.re`'s `NOVA AC_NEW
...`, called `BS_INTERN` for the name `"AC_NEW"` and found the
**already-existing Python-bootstrap lux** for it (both builders shared one
`BS_HTAB` symbol table). `LOAD_CMD_NOVA`'s "find or create" logic (`wax.re`)
read that hit as "already defined, update it" and overwrote the existing,
correctly-wired Python lux's operand slots with addresses that only make
sense in the self-hosted pass's own address space -- corrupting working
code. This is the exact same mechanism BUGS.md documents for `AC_WM_CUR` in
an earlier session (`LOAD_CMD_NOVA`'s find-or-create clobbering an
already-correct core lux) -- same root, different specific lux, found again
because it was never actually fixed, only reasoned about.

**Fix applied (Variant A, the same "bounded, owned region" principle
already chosen for memory, now applied to the symbol table):**
`htable.re`'s `HT_LOOKUP`/`HT_INSERT` were already fully parameterized by
`RA_HT_BASE`/`SIZE`/`MASK` on every call -- nothing in the generic hash
table needed to change. `intern.re` gained a second, physically separate
instance of the same table (`SK_HTAB_000`, same size as `BS_HTAB_000` by
symmetry, built with the same `SPAN` primitive). `wax.re`'s `LOAD_MAIN`
entry point now clears that table and repoints the one mutable register
(`BS_HT_BASE`) to it before Wave 0 starts -- `BS_LOOKUP`/`BS_INTERN`
themselves are untouched, every existing caller across the whole project
keeps working unchanged, because they always resolve the table through
that one register. From that point on, LOAD_MAIN (Wave 0 and Wave 1 alike)
only ever sees its own entries; a Python-bootstrap-era name is invisible to
it, so every name it encounters is either something it already built
itself earlier in its own pass, or genuinely new -- never a false "already
exists" hit against the other builder's work.

This mirrors the documented "INTENTIONAL DUPLICATION" bootstrap strategy
in `loader.py`'s own `freeze()` docstring exactly: *"Python is the source
of truth, Reca runs in parallel... once LOAD_MAIN reliably builds the graph
identically to Python — the Python layer is removed."* Two symbol tables
is what makes that duplication actually independent instead of
accidentally shared. It is also self-erasing by construction (rule:
fundamental additions must be reversible/limitable) -- once Python is
removed, there is only ever one builder, so `SK_HTAB` and the switch-over
in `LOAD_MAIN` become dead weight to delete, not a second mechanism to
maintain forever.

**Verified:** `freeze()` now prints `LOAD_MAIN complete.` (no caught
exception at all, not even a suppressed one) on every re-run, self-hosting
42/42 stable across repeated runs. `AC_NEW`'s slots confirmed correct
post-freeze (`op=Add, e1=addr(RA_ALLOC_RESULT), e2=addr(RA_ALLOC_COUNT),
exit=addr(RA_ALLOC_TMP)`) -- not corrupted. `BS_HTAB_000` and `SK_HTAB_000`
confirmed physically distinct, non-overlapping regions (`65536` luces
apart, exactly the first table's size). Full suite clean: `diag.py
--invariants` (no failures), `--lint` (clean), `check_comments.py` (clean),
`test_macros.py` (7/7).

**Not yet done / still open:** the allocator guard added earlier
(`alloc.re` `ALLOC_LUCES`) still has an unexercised halt branch (cursor
never reaches `RA_FLUX_BOTTOM` in current builds) -- worth a deliberate
low-limit test at some point, not urgent. The broader "everything should
live in an owned, bounded region" principle (Variant A) has now been
applied to two resources (the bump allocator's flux-zone guard, and the
symbol table) as two separate, hand-fitted instances rather than one
single generalized "region" primitive that both are instances of --
`arena.re` remains dead code with no upper bound. Whether to generalize
further (a single region/ownership abstraction with the flux zone, the
symbol table, and the temp-allocation-arena use case as three call sites of
one mechanism) is an open question for the author -- not required for
correctness right now (each of the two current uses is independently
correct and proven), but worth revisiting if a third such need appears
(that would make it three data points instead of two, a stronger signal
for "this is the same pattern, unify it").

**Also fixed in this session (unrelated, trivial, verified safe):**
`macros.re`'s `LH` macro dropped its dead first step (`LH_N0`, a
`Move C_1 → RA_SR_OFFSET` that was unconditionally overwritten a few steps
later by `SR_GLL`'s own identical first instruction, and whose output
register, `RA_MA5`, was never read again anywhere in the macro). `LH` now
synthesizes 3 NOVAs per call site instead of 4. `NEWREF LH LH_N0` become
`NEWREF LH LH_N1`.

---

## Related note (found this session, not acted on — see below)

While auditing allocation sites for a *different* reason (systematic
search for known bug-pattern classes, BUGS.md Pattern Z), one genuine
instance of "fixed-size allocation where the lcount formula should have
been used" was found and fixed: `LOAD_CN_INTERN` originally allocated a
hardcoded 2 luces regardless of a name's own real lcount, matching
`LOAD_CMD_NEW`'s correct `1 + 2*lcount + (lcount>0?1:0)` formula only by
luck for the common case. Fixed by making it call the same formula
`LOAD_CMD_NEW` uses. Given this plan is about to touch the *exact same*
sizing formulas project-wide (removing the `+1` sentinel, changing
`NOVA_SIZE`), it's the natural place to also do a full, systematic sweep
for any *other* remaining "hardcoded instead of lcount-aware" sites —
rather than doing that sweep separately, right now, on formulas this
plan is about to change anyway. Fold it into this plan's own
implementation-order step 3 ("update wax.re allocator formulas") as an
explicit sub-item: audit every allocation site touched by that step for
the same class of mistake, not just the two already-known allocators.
