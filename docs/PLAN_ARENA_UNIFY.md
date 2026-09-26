# PLAN: Unify arena (temporary rewind) and symbol-table switching into one mechanism

Status: DRAFT — for author review before implementation.

---

## The idea, in plain terms

Two things in the project look different but are actually the same action:

1. **Compiler temporaries** (not yet built): "remember where the free-space
   marker is, do some work, then either put the marker back (throw the work
   away) or leave it where it moved to (keep the work)."
2. **Self-hosted symbol table switch** (already built today, by hand, in
   `wax.re`'s `LOAD_MAIN` entry): "remember which symbol table Python was
   using, switch to a second, separate one so the self-hosted build never
   sees Python's names, and never switch back — the new one stays active
   forever."

Both are "save the current value of some register, then either restore it
later or don't." The only real difference is *which register* is being
saved, and *whether anyone ever calls the restore step*. Today's `arena.re`
already treats "not calling `ARENA_CLOSE`" as perfectly safe — it's just
never been used that way on purpose, and it's hardcoded to only ever touch
one specific register (`K_CURSOR`).

**The fix: make the existing arena mechanism work on *any* register the
caller names, instead of only `K_CURSOR`.** Then:
- A normal temporary arena = open on `K_CURSOR`, do work, close (restore).
- The symbol-table switch = open on `BS_HT_BASE`, point it at the second
  table, never close.

One mechanism, two honest uses of it — not two mechanisms pretending to be
one.

---

## What was ruled out, and why (so we don't re-litigate it)

- **A brand-new, separate stack just for arenas** — considered, rejected.
  The existing call stack (`RA_SP`) is already initialized before any Reca
  code ever runs (Python sets it at freeze time), so "the call stack might
  not exist yet" was never a real constraint — it was an unfounded worry.
  Building a second stack mechanism to solve a problem that doesn't exist
  would just be duplicating `RA_SP`'s own logic for no reason. **Decision:
  keep using `RA_SP`, exactly like the current `arena.re` already does**
  (frame slot 2). Nothing changes here from the existing design.
- **Forcing the symbol-table switch to literally reuse `ARENA_OPEN`'s
  existing code unchanged** — considered, rejected in its literal form
  (the current code can only ever touch `K_CURSOR`), but the underlying
  idea was right. The fix isn't "force two different things into one
  narrow function" — it's "widen the one function so both are honestly the
  same call with a different argument."

---

## Concrete design

### 1. `ARENA_MARK` / `ARENA_OPEN` / `ARENA_CLOSE` — add a target register parameter

Today, `ARENA_OPEN` always does `Read El1=K_CURSOR`, and `ARENA_CLOSE`
always does `Write El1=K_CURSOR`. Change both to take the register to
save/restore as an argument (`RA_MA1`, following the project's existing
macro-argument convention), instead of a hardcoded name.

- `ARENA_OPEN target` — reads `target`'s current value, pushes it onto the
  call-stack frame (slot 2, same as today), and remembers *which register*
  it saved so `ARENA_CLOSE` knows where to write back to (needs a second
  saved slot — see "frame layout" below).
- `ARENA_CLOSE` — pops the saved value and the saved target register,
  writes the value back into that register.
- `ARENA_SIZE` — only meaningful for the `K_CURSOR` case (measuring luces
  allocated); kept as-is, still specific to `K_CURSOR`, documented as such.

Callers that want today's exact behavior just call `ARENA_OPEN K_CURSOR`
instead of bare `ARENA_OPEN` — one extra word, no behavior change for
existing use.

### 2. Frame layout change

Current: slot 0 = saved return address (automatic), slot 1 = reserved,
slot 2 = arena mark (one value, always `K_CURSOR`'s value).

New: slot 2 = the saved *value*, slot 1 (currently "reserved," never used)
becomes the saved *target register address* (which register to restore
into). This uses a slot that already exists and was already set aside for
this — no frame growth needed.

### 3. `wax.re`'s `LOAD_MAIN` symbol-table switch — rewritten to use the arena mechanism

Right now `LOAD_MAIN` manually does:
```
Move SK_HTAB_000 -> SK_HT_BASE
Move SK_HT_BASE -> RA_HT_BASE (temp, for the HT_CLEAR call)
Move BS_HT_SIZE -> RA_HT_SIZE
RVOCA HT_CLEAR
Move SK_HT_BASE -> BS_HT_BASE
```
This becomes:
```
RVOCA ARENA_OPEN BS_HT_BASE     <- saves the Python table's base, opens arena
[clear + point BS_HT_BASE at SK_HTAB_000, same as today's last 2 steps]
```
`ARENA_CLOSE` is deliberately never called for this one — the self-hosted
table stays active for the rest of `LOAD_MAIN`'s run, which is exactly the
intended behavior. This documents *in the code itself*, not just in a
comment, that this is a permanent switch built from the same primitive as
a temporary one — the absence of a matching `ARENA_CLOSE` is the whole
point, not an oversight.

### 4. First real temporary-arena user: compiler scratch reclaim in `yaku.re`

`yaku.re`'s compiler entry point (`P0_NID`) currently does this by hand:
```
Read K_CURSOR -> RA_NEXT_ID              (snapshot, at P0_NID, compiler entry)
... BFS/VS/SSA/RT scratch gets allocated, using RA_NEXT_ID as a SIZE for
    two of them (RA_VS_WORDS is computed from RA_NEXT_ID; RA_SSA_BASE's
    table is sized RA_NEXT_ID luces) — not just as a rewind marker ...
Write RA_NEXT_ID -> K_CURSOR             (P0_PREAM_DONE, rewinds the cursor)
```

**Important correction found while double-checking this section (the
author asked exactly the right question about it):** `RA_NEXT_ID` is not
only a rewind marker. It is read as an ordinary number in two other
places — sizing the visited-bitset (`RA_VS_WORDS`) and sizing the SSA
direct-map table (`RA_SSA_BASE`'s allocation). Those reads have nothing to
do with the arena mechanism and must keep working exactly as they do
today. So `ARENA_OPEN` must NOT replace the `Read K_CURSOR -> RA_NEXT_ID`
step — it sits *alongside* it, taking its own separate snapshot (stored on
the call-stack frame, invisible to the rest of the code, exactly like
today's arena). `RA_NEXT_ID` keeps existing as a normal, readable register
for the size computations that depend on it. Only the *rewind* step
changes:
```
Read K_CURSOR -> RA_NEXT_ID              (unchanged, still needed as a number)
RVOCA ARENA_OPEN K_CURSOR                (new: arena's own snapshot, for the rewind)
... BFS/VS/SSA/RT scratch, unchanged ...
RVOCA ARENA_CLOSE                        (replaces "Write RA_NEXT_ID -> K_CURSOR")
```
This gives the arena mechanism a second, real, independent caller
(alongside the symbol-table switch), confirms the general-purpose version
works for its originally-intended purpose, and removes the hand-rolled
half of the "save a cursor, restore it later" pattern (the rewind) while
leaving the unrelated half (`RA_NEXT_ID` as a size input) untouched,
because it was never actually part of the same concern.

---

## What does NOT change

- `htable.re` (`HT_LOOKUP`/`HT_INSERT`/`HT_CLEAR`) — already fully
  general-purpose, parameterized by `RA_HT_BASE`/`SIZE`/`MASK` on every
  call. Nothing here needs to change; it already supports multiple table
  instances by construction.
- `BS_LOOKUP`/`BS_INTERN` (`intern.re`) — untouched. They resolve through
  `BS_HT_BASE`, and this plan only changes *how* `BS_HT_BASE` gets its
  value switched (via the arena mechanism instead of a hand-written
  sequence), not what they do with it.
- The two symbol tables themselves (`BS_HTAB_000`, `SK_HTAB_000`) — already
  built this session, unchanged by this plan.
- `RA_SP` / the call stack itself — unchanged, still the automatic
  push/pop mechanism it always was. The arena mechanism continues to ride
  on top of it exactly as today, just storing one more piece of
  information per frame (which register, not only what value).

---

## Verification plan

1. `check_comments.py` after the `.re` edits (cheap, catches comment-span
   breakage before a full build).
2. One `freeze()` after the full set of changes lands together (arena
   parameterization + `LOAD_MAIN` switch rewrite + `yaku.re` reclaim
   rewrite) — this is a genuinely linked set of changes, not routine
   busywork, so one combined self-compile is the right point to test, not
   three separate ones.
3. Confirm `freeze()` prints `LOAD_MAIN complete.` (no caught exception),
   self-hosting still 42/42, stable across repeated runs (matches the bar
   already established this session).
4. `diag.py --invariants` (no failures), `--lint` (clean), `test_macros.py`
   (7/7), `check_nova_sizing.py` (0 violations) — the same full suite used
   throughout this session.
5. Confirm the two symbol tables are still physically distinct after the
   rewrite (same check used earlier: `BS_HTAB_000` and `SK_HTAB_000`
   addresses, non-overlapping).
6. Confirm `yaku.re`'s compiler scratch reclaim still works after
   switching to `ARENA_OPEN`/`CLOSE` — the BFS/VS/SSA/RT scratch region
   must still be reclaimed correctly (same final state as the current
   manual `Write RA_NEXT_ID -> K_CURSOR`, just reached through the shared
   mechanism instead of a one-off line).

---

## Open items for the author (not blocking, flagged for awareness)

- `ARENA_SIZE` stays hardcoded to measuring `K_CURSOR` specifically (it
  doesn't make sense for the symbol-table-switch use case — there's no
  "how many luces did I allocate" question for a table swap). This is a
  case where *not* generalizing further is the right call — flagging so
  it's a visible decision, not a silent gap.
- This plan does not touch `ALLOC_FLUX_ZONE`'s own guard (`RA_FLUX_TOP`)
  or fold it into the same mechanism. That guard checks an upper bound on
  a *different*, separately-owned cursor (`K_FLUX_CURSOR`) — it isn't a
  "save and maybe restore" operation at all, so it doesn't fit this
  mechanism's shape. Left as-is, no change proposed.

---

## Follow-up discussion (author + assistant, after the draft above): why this plan is not yet complete

The draft above assumed one caller at a time, arriving one after another
(Wave 1 processing files in order, or Python finishing before the
self-hosted pass starts). Under that assumption, sharing one cursor with
"do the permanent thing first, then the temporary thing on top of it, then
optionally throw the temporary part away" is safe — there's never a moment
where two unrelated things reach for the same register at once.

The author pushed back on this, correctly: that safety only holds because
nothing today is *actually* running at the same time as anything else.
The project's own roadmap already names a real future user that breaks
this assumption — a thread scheduler (`scheduler.re`, `SCHED_FORK`,
already fully written and working, just never called from anywhere yet).
Once real OS threads exist, two of them genuinely can ask for a register
save/restore in the same instant, not one after another — and "save this
register's value, maybe restore it later" as designed above has no way to
tell two simultaneous savers apart. It would silently let the second one
clobber the first, the same class of bug this whole session has been
chasing (the symbol-table collision, `AC_NEW`'s corruption, etc.) — just
moved to a new place instead of fixed at the root.

**Conclusion reached:** the arena mechanism itself must be able to tell
which caller a given save/restore belongs to, not just "was there one
save open at a time." This is not a new, separate mechanism — the
underlying operation (register value in, later maybe out) stays exactly
as described above. What changes is that opening an arena must record
*whose* it is, so simultaneous callers never get confused with each
other's saved values, even if they call in at the exact same moment.

**Why this is being tested against a real scheduler instead of an
imagined one:** it would be cheaper to just imagine "what if two things
happen at once" and design around that guess, but a guess can be wrong in
ways that only show up once something real actually exercises it — and
building the scheduler was going to happen eventually anyway (it's on the
roadmap, not optional). Doing it now, before finishing the arena's final
shape, means the arena gets checked against genuine simultaneity instead
of an assumption about it, at no real extra cost since the work doesn't
disappear either way.

**A second, related idea surfaced in the same discussion, likely to
become the scheduler's actual mechanism for handing out parallel work:**
today a NOVA's single `next` slot can only ever hold one address (or 0).
The idea: let it optionally point at an ordinary Data lux whose first slot
(word) holds a fixed marker value (a plain register, resolved at load
time like any other name — never a hardcoded number in the interpreter),
identifying it as "a list of paths," followed by a plain sequence of
addresses (one slot per path, not a pair like a normal lumen — there's no
name to search for here, just destinations), ending in 0 exactly like a
normal lumen list does. An NOVA whose `next` leads to one of these is a
fan-out point: whatever reads it (the scheduler) takes the first path
itself and can hand the rest to new threads. This is deliberately not the
same thing as `flux.re`'s existing structure — flux is fully mutable,
described by a separate type lux, meant for "anything at all"; this is a
fixed, narrow, cheap shape meant for exactly one job (multiple next
addresses, nothing else), so anyone reading it knows immediately what
they're looking at without having to decode a type lux first to find out.
Full detail in `PLAN_SCHEDULER.md`.

**Updated scope for this arena plan:** the register-save/restore mechanism
described above (Sections 1-4) still stands as the right shape for the
mechanism itself. What's added, informed by the scheduler discussion, is
the requirement that it must safely support simultaneous callers before
it can be considered done — this will be verified directly against
`SCHED_FORK` once real parallel threads exist to test it with, not against
a guess about what parallelism might need. Implementation order: build
enough of the scheduler (per `PLAN_SCHEDULER.md`) to get two real,
simultaneous threads asking for something from the arena mechanism, then
finish the arena's design so it demonstrably handles that correctly, then
land both together.

---

## Clarification found while working the scheduler plan: "give each thread its own registers" is NOT this mechanism

`PLAN_SCHEDULER.md` surfaced a second problem: `RA_*` registers today are
ordinary shared luces, and two real threads racing on the same register
addresses will corrupt each other's in-flight values (see Pattern W in
BUGS.md for the full writeup). The fix is "give each new thread its own
copy of the collected register block" — allocated fresh, once, when the
thread is created, and never given back.

This is deliberately NOT the same thing as the arena mechanism above,
despite sounding similar ("give something its own region"). The arena is
about a temporary borrow-and-maybe-return on a *shared* resource one
caller uses at a time (a cursor moving forward, later maybe rewound). A
thread's register block is a *permanent, exclusive* allocation — nobody
else ever touches it, and it's never handed back while the thread lives.
The right general shape for "give me a new region of size N, right now,
that's genuinely mine" is closer to what `SK_HTAB_000` already is (a
second, separate, permanently-allocated region, built with `SPAN`, never
merged with the first) than to `ARENA_OPEN`/`ARENA_CLOSE`'s save-and-
maybe-restore shape. Three data points now exist for "allocate an owned
region, keep it forever, no rewind": the two symbol tables, and now
per-thread register blocks. That's a real signal (per the project's own
pattern-analysis principle: two might be coincidence, three is a pattern)
that this deserves its own named primitive rather than continuing to be
solved by hand each time it comes up — but it is a *different* primitive
from the arena's rewind mechanism, not an extension of it. Not designing
this primitive yet; noting it here so the distinction isn't lost, and so
this plan's own "arena" scope doesn't quietly grow to cover something it
was never meant to.
