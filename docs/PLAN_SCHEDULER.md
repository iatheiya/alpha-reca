# PLAN: Bring the scheduler to life (minimal real parallelism, to test the arena mechanism honestly)

Status: DRAFT — for author review before implementation.

Related: `PLAN_ARENA_UNIFY.md` — this plan exists because the arena
mechanism can't be honestly finished without a real, simultaneous second
caller to test it against, and imagining one risks designing around a
wrong guess instead of a real constraint.

---

## What already exists (found this session, not invented)

`scheduler.re`'s `SCHED_FORK` is fully written and does real work: given a
flux with multiple `Next` slots, it launches a real OS thread (via
`clone`) for each extra one, current execution takes the first. It already
knows it shares the aether with the new thread (`CLONE_VM` — same address
space) and says so plainly in its own comment: *"Synchronisation: caller's
responsibility (futex-based mutex/barrier)."*

Nobody calls it. `bfs.re` names the intended real caller (a frontier walk
over the compiled program's own graph: find nodes with no unprocessed
dependencies, run them, when one finishes check if that unblocks others,
repeat — "possibly in parallel via SCHED_FORK") but that walk itself is
not finished yet (only one small piece, `FW_IS_FRONTIER`, exists; the
actual queue-driven traversal `bfs.re` describes in its own header comment
is not written).

`flux.re` (the thing `SCHED_FORK` reads) only has read-side functions
today — nothing in the project builds a flux from scratch. Something has
to write one by hand for `SCHED_FORK` to have anything to read.

---

## The narrow goal here

Not "finish the parallel compiler." Just: get two real OS threads racing
to touch the same aether at the same time, on purpose, so the arena
mechanism (`PLAN_ARENA_UNIFY.md`) can be checked against genuine
simultaneity instead of a guess about it. Finishing the real BFS
frontier-walk integration is a separate, larger job for later — building
it now would be answering a much bigger question than the one currently
blocking anything.

---

## Plan

### 1. Hand-build one minimal flux, just for this test

A flux is: `[word = type_lux_addr, slot1, slot2, ...]`, where the type lux
is `[any_word, code1, code2, ..., 0]` and codes `1..7` describe what each
flux slot means (`6` = "this slot is a Next address"). To get two real
threads, the smallest possible flux is one with a type lux of just
`[any_word, 6, 6, 0]` (two Next codes, nothing else) and a flux body
holding two real addresses to jump to. This does not require BFS, does not
require the compiler, does not require anything else in the project to be
finished — it's a handful of `NEW`/`SET` lines placed somewhere reachable,
built by hand specifically to give `SCHED_FORK` something real to fork.

### 2. Give `SCHED_FORK` a real caller

Somewhere reachable from a real run (not buried in dead code the way the
old `arena.re` was for months), call `SCHED_FORK` on that hand-built flux.
Both resulting paths of execution should do something simple and
observable — cheapest option: each thread tries to open/use the arena
mechanism on a shared register at close to the same moment, so the test
directly exercises the thing we actually need to check.

### 3. Confirm real simultaneity, not just "it ran twice"

Two threads finishing one after another (even fast) isn't the same as
"asked for something at the same instant." Before trusting the result,
confirm via `trace.py`/direct inspection that both threads are alive and
executing at overlapping times (not just sequentially fast) — otherwise
the test would silently degrade back into the same "one at a time"
assumption the whole point was to get past.

### 4. Only then: finish the arena mechanism's "whose is this" tracking

Per `PLAN_ARENA_UNIFY.md`'s updated scope — the arena's save/restore must
record which caller owns a given saved value, so two simultaneous savers
never get confused with each other's. This step is real design work, not
routine, and comes after step 3 gives something real to check it against.

### 5. Land arena + this minimal scheduler wiring together

Both are one linked change at that point (the arena's correctness claim
depends on the scheduler test proving it), so one combined self-compile
and verification pass at the end, not one after each.

---

## What this plan deliberately does NOT do

- Does not finish `bfs.re`'s real frontier-walk / dataflow-parallel
  traversal. That's the actual, full integration named in `bfs.re`'s own
  comments, and it's a separate, larger piece of work for later — this
  plan only needs *a* real fork, not *the* real, final one.
- Does not add thread join/barrier logic beyond whatever the minimal test
  needs to observe both threads ran. `scheduler.re`'s own comment already
  flags join/barrier as a known gap ("use futex manually for now") —
  out of scope here unless the minimal test can't be verified without it.
- Does not touch `flux.re`'s read-side functions — they're already correct
  and general-purpose; this plan only needs one flux built by hand to feed
  them, not a new way to build fluxes in general.

---

## Related idea surfaced alongside this (documented here, belongs to a later, separate plan)

While discussing why `next` can only ever hold one address today, the idea
came up of letting a NOVA's `next` optionally lead to a small, fixed-shape
"list of paths" instead of `flux.re`'s fully-mutable structure — cheaper
(no type lux to describe it, no per-slot code to decode) and more
readable (its shape alone tells you what it is, instead of needing to
decode a type lux to find out whether you're looking at a simple fan-out
or something arbitrary). This is deliberately a different, narrower thing
than flux, not a replacement for it — flux stays the tool for "anything at
all," this would be the tool for exactly one job. Marked as author-facing
naming still undecided (`LINKS` was proposed; alternatives like `FORK`,
`SPLIT`, `BRANCH` were checked and are equally free — author's call). Not
part of this plan's minimal scheduler test; noted here so it isn't lost,
to be designed properly once the scheduler and arena work above is done
and there's a real multi-path caller to design it against instead of a
guess.

---

## Open questions for the author

1. Naming for the new "list of paths" NOVA-next shape (see above) —
   `LINKS`, `FORK`, `SPLIT`, `BRANCH`, or something else entirely.
   **Deferred by author.**
2. Whether the minimal hand-built flux test (step 1-3) should live
   somewhere permanent as a real regression test for "the scheduler and
   arena work correctly together," or be temporary/throwaway once it's
   confirmed working once. **Author decided: keep it permanent, add to an
   existing file rather than a new one.**

---

## Update: neither runtime actually supports `SYS_CLONE` yet — real work needed before step 2-3 can run at all

Checked both places code actually executes, rather than assuming
`SCHED_FORK` would just work once called:

- **Python interpreter** (`interpreter.py`'s `_do_syscall`): handles only
  write, write_packed, read, openat, close, exit. `SYS_CLONE` (220) falls
  through to the default `return 0` — silently pretending to be the child
  every time, never actually spawning anything. Confirmed by reading the
  function directly, not by running and guessing from behavior.
- **Compiled-binary runtime** (`preamble_template.ll`'s `@_sys`): same
  narrow syscall switch (write/read/openat/close/exit), same gap. This is
  hand-written LLVM IR (the file's own header says "Python is a BLIND
  ASSEMBLER... edit that file, not this one" — meaning this needs a real,
  careful manual edit, not generated code).
- `clang`/`llc` were not installed in the working environment at all
  (only the `libllvm20` library was present, no actual compiler binaries).
  Installed `clang-20`/`llvm-20` via apt to make the compiled-binary path
  usable — this is now available going forward.

**Author's decision on scope, given this: do it properly in both places,
not a narrow/partial version.** A cheaper, incomplete path was considered
(e.g., only wiring up enough for the Python interpreter to fake a second
thread well enough to exercise the arena mechanism) and rejected — the
real work has to happen eventually regardless, so there's no actual
savings in doing it partially now.

**A second, more fundamental gap found while working out how `clone`
would actually behave: registers have no per-thread ownership.** `RA_*`
registers are ordinary shared luces today — safe with one thread, unsafe
the moment two real threads run at once and touch the same scratch
register. This is not specific to the scheduler's own bookkeeping
registers; it's every general-purpose register in the project. Full
writeup in `BUGS.md` Pattern W and the rejected alternatives in
`DECISIONS.md`. The adopted fix — collect registers into one contiguous,
compact block so a fresh copy can be handed to each new thread cheaply,
without changing any existing instruction — is now a prerequisite for this
plan's step 2-3 (there is no honest way to run two real threads doing real
work without it; a test narrow enough to avoid the collision entirely
would not actually prove the arena mechanism works under real
simultaneity, defeating the point of building a real scheduler instead of
imagining one).

**Also found, specific to the compiled-binary path:** in compiled LLVM IR,
each Reca entry point (`LINK X Entry Yaku`) becomes its own separate LLVM
function (`@main` for the first one found, `@L<id>` for the rest) — there
is no single "resume execution at address N" the way the interpreter's
`pc` provides. A forked thread's target must be a real, already-compiled
LLVM function, not an arbitrary NOVA-graph address the way `SCHED_FORK`'s
current flux-based design assumes. This needs to be accounted for when
`clone` is wired into `preamble_template.ll` — the "list of paths"
mentioned above will likely need to carry a function to call, not just an
address to jump to, for the compiled-binary case specifically. Not yet
resolved; flagging so it isn't lost.

**Revised order for this plan, given the above:**
0. Land the unified `NEW`/`REF`/`OWN`/`FREE`/`SET` declaration syntax
   first (see `DECISIONS.md`'s "Pending: unify..." note) — every later
   step here depends on it. In particular, marking a register `OWN`
   (step 1 below) needs this syntax to exist before anything can be
   marked at all; this is foundational and must land before, not
   alongside, the rest of this plan. Includes settling the `REF`
   replacement word (`DECISIONS.md`, author's call, not yet finalized) —
   the runtime macro labels (`NEWREF_START` and siblings in `macros.re`)
   get renamed together with it, once chosen.
1. Mark the ~251 genuine scratch registers (`RA_TMP`, `RA_I`, ...) `OWN`
   (author-confirmed name — see `BUGS.md` Pattern W's "Resolved: default
   stays shared" note), using the syntax from step 0. Ordinary `NEW`
   keeps its current meaning unchanged (one shared aether slot) — this
   matches the aether's stated default nature ("only Aether, one shared
   space" per `GLOSSARY.md`); nothing about existing `NEW` declarations
   changes, and the ~35 registers that must stay genuinely shared
   (`BS_HT_BASE` and similar base-address pointers) are simply left alone.
   Then collect everything marked `OWN` into one contiguous block
   (prerequisite for cheap per-thread copying) — not optional, both
   remaining steps depend on it.
2. Wire real `SYS_CLONE` into the Python interpreter (`_do_syscall`) via
   real OS threads, using the now-per-thread register block.
3. Wire real `SYS_CLONE` into `preamble_template.ll` by hand, accounting
   for the compiled-binary function-boundary issue above.
4. Then proceed with the original steps 1-3 (hand-built test flux, real
   caller, confirm genuine simultaneity) against whichever runtime is
   being exercised.

