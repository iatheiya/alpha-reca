# PLAN: tide — one parse, in the Aether, same waves on both sides

## Two corrections to the framing, checked in the code first

**1. Reca's waves are not a broken duplicate of Python's.** `loader.py`
says it outright at the LOAD_MAIN call site: *"Builds BS_HTAB
independently. Not yet the source of truth."* and *"Python Wave-B =
scaffolding only."* Reca's waves are the **goal**; Python's are the
scaffolding that should eventually be deleted. Both sides build the
graph in parallel today, and `check_graph_equivalence.py` exists to
compare the two results. So this work is not "delete Reca's prepass
because Python already has it" — it's **grow Reca's side until the two
structures match**, which is the precondition for switching Python off.

**2. Wave-A and Wave-A2 cannot be merged.** They look like the same
job (both allocate luces for names), but Wave-A2's body guards every
allocation with `name not in self.symbols`. That guard only works if
Wave-A has already run **over every file**. Example: `JEQ FOO ...` in
file 1 refers to `FOO`, declared as `NOVA FOO` in file 40. Merged into
one wave, file 1 is processed before file 40 exists, so the A2 half
would allocate `FOO` itself — as an empty stub — and file 40's real
declaration would collide with it. They are separated by a
**cross-file barrier**, not by the kind of work they do. (This is the
same reason Reca grew its own Wave 0.5.)

## What each stage does today (verified)

Python:

| stage | what it does |
|---|---|
| `_read_re_file` + `_strip_comments` | read file, cut `//...//` blocks and `/` lines, cache |
| Wave-A | allocate luces: `NEW`, `NOVA`, `NOVAE` label + generated nodes |
| Wave-A2 | allocate luces: `RVOCA`/`RREDI` names, macro-call arguments |
| Wave-B0 | wire `SEAL` cross-refs, `NEW`'s deferred `ALIAS=`, mark `OWN`/`FREE` |
| Wave-B | wire operands, lumina, Next pointers; run macros |

No deallocation wave exists. Checked directly.

Reca:

| stage | what it does |
|---|---|
| Wave 0 ("prepass") | count LINK lumina per src name. Does *not* register names (standing TODO at `wax.re:571` says it should) |
| Wave 0.5 ("declare") | register declarable names across all files — exists only because Wave 0 doesn't |
| Wave 1 ("load") | build the real graph |

Comments: Python strips them once, before any wave. Reca never strips
them — it detects them live, mid-parse, in three duplicated places,
while `STRIP_COMMENTS` (`comments.re`) sits unused.

## Target: identical wave structure on both sides

| wave | job |
|---|---|
| **Wave 0 (tide)** | read file; cut `//...//` blocks and `/` lines; statically dispatch each clean line to identify the command and bound its arguments; drop everything past the arguments; write the parsed result into the Aether; cache it |
| **Wave 1** | allocation — `NEW`, `NOVA`, `NOVAE` label + generated nodes |
| **Wave 2** | further allocation — `RVOCA`/`RREDI` names, macro-call arguments (separate wave: needs Wave 1 complete across all files) |
| **Wave 3** | wire `SEAL` cross-refs, `NEW`'s deferred `ALIAS=`, mark `OWN`/`FREE` |
| **Wave 4** | wire operands, lumina, Next pointers; run macros |

Both sides use these numbers. The word "prepass" is removed everywhere
— it currently names Reca's Wave 0, which counts lumina and has
nothing to do with reading text.

Waves 1–4 read tide's parsed luces. They never open a file and never
call `BS_READ_TOKEN`.

## The AST question, settled

Reca already has an AST: `_parse_line` builds `(cmd, parts)` per line
and discards it, three times per file per freeze. "No AST" was never
true. The goal is to minimise it, not to pretend it's absent. Keeping
one parse instead of rebuilding it twice more is strictly less AST
work. **tide's output lives in the Aether as luces** — it's data, data
is luces, and Reca can inspect it.

## Staged plan

### Stage 1 — I/O moves to io.re
- Move `BS_INIT` and `BS_READ_BYTE` out of `lexer.re` into `io.re`.
  `io.re` is constants only today (fds, open flags, perm masks) — no
  behaviour at all, despite the name.
- Generalise while moving: the current code is shaped around being the
  lexer's private chunk reader (4096-byte buffer, `RA_LOAD_RPOS`/
  `RA_LOAD_RLEN` positions, plus the redirect-mode override). tide
  needs whole-file reads. Decide the shape once, in `io.re`, so both
  callers use one implementation.
- Update every reference; keep the names short.
- `lexer.re` keeps **only tokenisation**: `BS_READ_TOKEN`,
  `BS_TOKEN_VALUE`, `BS_SCAN_EQ`, `BS_PACK_TOKBUF`, `BS_PARSE_INT`.

### Stage 2 — comments consolidate into comments.re
- Rename `inscriptions.re` back to `comments.re` (one command; the
  earlier rename was a mistake — `INSC` as a prefix is cryptic, and
  the file strips comments and nothing else).
- Verify `STRIP_COMMENTS` can actually replace what `wax.re`/
  `lexer.re` do live today. It already implements the full
  `NORMAL`/`BLOCK`/`STRING` state machine and is Python-equivalent, so
  it should — but confirm against the real cases first (quoted `"//"`,
  `///` runs, a `//` opener later on a line whose first word isn't a
  command).
- Once confirmed, delete every duplicated site: `BS_BLOCK_SKIP`,
  `BS_SKIP_TO_EOL_CW` (added this session as a stopgap), and the two
  comment checks patched into `BS_READ_TOKEN` this session. Update all
  references to go through `comments.re`.
- `BS_SKIP_TO_EOL` survives Stage 2 (28 callers use it to drop a
  line's tail after a command has read its arguments — not a comment
  concern) and dies in Stage 3, when tide makes tails impossible.

### Stage 3 — tide.re, both sides
**Written, verified standalone, NOT wired in. This is where work stopped.**

`tide.re` exists and does what it should. Verified directly, not by
inspection:

- Run against a real open file it slurps 3765 bytes, strips them to
  2113, and sets the redirect — 113797 steps, correct cleaned output.
- `STRIP_COMMENTS` (comments.re) was parity-tested against
  `loader.py::_strip_comments` over 35 real project files, driven
  through the actual interpreter: **all 35 match**.
- Stepping `TIDE_FILE` by hand walks exactly the intended path:
  TIDE_FILE → TIDE_INIT → TIDE_INIT1 → TIDE_INIT_RET → TIDE_F_SLURP →
  TIDE_SLURP → TIDE_SL_RD → BS_READ_BYTE, and `TIDE_RAW_BASE` ends up
  correct (1494643).

Wiring it into `LOAD_FILE` and `LOAD_PREPASS_FILE` collapses the build
from 22/42 to 1/42. Two real bugs were found and fixed along the way:

1. Buffers declared with `BLOCK` instead of `SPAN`. BLOCK names every
   lux it allocates, so two 262144-lux buffers added ~524k symbols —
   a symbol table eight times its proper size. Large regions use SPAN
   (HT_NAME_POOL_BASE, BS_HTAB_000); BLOCK is for small ones.
2. `STRIP_COMMENTS` collapsed dropped lines away instead of leaving a
   blank line behind. Python's stripper emits a blank placeholder per
   dropped line, preserving line numbers and blank-line boundaries —
   both of which the loader depends on, since CHAIN/SWITCH/FOR/NOVAE
   bodies are delimited by indentation. Fixed; Reca's output is now
   line-for-line identical to Python's, blanks included.

Neither fix restored the build. The remaining symptom: in the real
run `TIDE_INIT` writes 47 into `TIDE_RAW_BASE` instead of 1494643,
i.e. `Move El1=TIDE_RAW_000` reads something other than the SPAN's
self-ref — even though the self-ref is intact in the frozen image
(`word == addr`, checked), the chain is wired correctly (checked
against BS_INIT), the call sites are wired correctly (checked), the
buffers overlap nothing (checked), and the identical sequence works
when stepped by hand.

**Not found. Do not resume by re-checking the above — all of it is
confirmed.** The contradiction is between hand-stepping (works) and
the real LOAD_MAIN run (does not), so the next move is to find what
differs about the *context* of the call, not the call itself.

- New file `tide.re`. Not inside `wax.re`, which is already 147KB.
- Remove Reca's existing prepass (Wave 0 / Wave 0.5) from `wax.re`;
  its jobs move into tide, which walks every file before any wave
  anyway. This dissolves the `wax.re:571` TODO about Wave 0 not
  registering names.
- tide, per file, once:
  1. read the whole file (via `io.re`),
  2. strip comments (via `comments.re`'s `STRIP_COMMENTS`, unchanged),
  3. statically dispatch each clean line — identify the command, bound
     its arguments — **reusing the loader's existing dispatch**, in a
     mode that identifies without executing. This must not become a
     second parser.
  4. drop the tail past the arguments,
  5. write the parsed form into the Aether as luces, and cache it so no
     later wave re-dispatches anything.
- Python gets the same restructure so both sides match.
- Wire tide into `LOAD_MAIN` as Wave 0.

Steps 3–5 above are not started. Only steps 1–2 (slurp + strip) exist.

### Stage 4 — waves read parsed luces
- Waves 1–4 stop opening files and stop calling `BS_READ_TOKEN`.
- Delete `BS_SKIP_TO_EOL` — with tails dropped by tide, nothing skips
  to end-of-line.
- Renumber both sides to Wave 0–4. Remove "prepass" everywhere.
- Expected: today's ~1.15M byte-reads per freeze (~45% of them inside
  `BS_SKIP_TO_EOL` walking comment text) collapse to one read pass per
  file.

### Stage 5 — verify
- `run_all_checks.py`, `test_macros.py`, `diag.py --lint`,
  `check_graph_equivalence.py` all clean.
- Self-hosting must not regress. It sits at 19/41, stalled in
  `intern.re` — a separate, pre-existing bug, not in scope.

## Status

Stages 1–3 done, Stage 4 partly done.

**Done:**
- I/O moved to `io.re` (`BS_READ_BYTE`, read buffer). `lexer.re` is
  tokenisation only.
- `inscriptions.re` renamed back to `comments.re`.
- `STRIP_COMMENTS` fixed to Python parity, then replaced entirely by
  the lone-marker rule (see COMMENTS.md). 604 lines across 33 files
  migrated to the new style.
- `tide.re` written and wired into `LOAD_FILE` and `LOAD_PREPASS_FILE`
  as Wave 0. It reads a file whole, strips it via `STRIP_COMMENTS`, and
  redirects the byte source at the cleaned text.
- All five live comment checks in the loader deleted, plus
  `BS_BLOCK_SKIP`. Comment handling now exists in exactly one place per
  side.
- Trailing `/comments` are now cut by Wave 0 on both sides. They were
  previously left for the dispatcher to fail-parse away — 622 lines of
  pure waste, and the reason ~45% of all byte reads went through
  `BS_SKIP_TO_EOL`.
- Waves renamed to a shared numeric scheme. Python:
  `_wave1_alloc_line` / `_wave2_alloc_line` / `_wave3_seal_line` /
  `_wave4_wire_line`, labelled "Wave 1".."Wave 4". Reca: Wave 0 (tide),
  1 (lcount), 2 (declare), 3 (load). The word "prepass" is gone from
  `wax.re` entirely; the remaining uses in `loader.py` refer to
  genuine global pre-scans (`_lumen_prepass`, the OWN prepass), not
  waves, and are correct as they are.

**Not done:**
- The two sides' waves are numbered consistently but still do different
  work: Python allocates in two waves (1, 2) split by a cross-file
  barrier, then seals (3), then wires (4). Reca counts lumina (1),
  declares (2), then builds (3). Aligning the *work* — not just the
  names — is what "grow Reca's side until the two structures match"
  actually means, and it is still ahead.
- Reca's Wave 1 still does not register names — the reason Wave 2
  exists at all is only to cover that gap. The fix is extending Wave 1
  to register names itself (the exact command set Python's
  `_BOOTSTRAP_CMDS` treats as "declares a name"), at which point Wave 2
  has nothing left to do and is deleted outright.
  **The barrier itself is not architecture — checked against BUGS.md's
  Pattern Z, its own history: the "declare before use" barrier was
  built to work around a real, since-fixed bug (`LOAD_CN_INTERN`
  allocating a fixed 2-lux stub instead of sizing it like `LOAD_CMD_NEW`
  does, accounting for lcount). `wax.re`'s comment calling it "the
  deliberate seam for a future scheduler" was written mid-debugging,
  before the real cause was known — a rationalisation, not a decision.
  Reca has always had `BS_INTERN_NOVA`, the same correctly-sized-stub
  mechanism that let Python drop its own equivalent barrier via lazy
  resolve. There is no reason to keep this one either.**
- Waves still re-read files. tide parses each file once, but its output
  is bytes, not parsed commands, so waves 1–3 still tokenise. Keeping
  the parse (Stage 3's "write the parsed form into the Aether") is not
  done.

**Open question:** self-hosting is 19/42, down from 22/42 before tide,
stalling in `lexer.re`. The three-file difference is unexplained.

## Wave alignment — the map and the plan

Read out of the code, not remembered.

### Python — 4 waves, plus 2 scanners that are not waves

| wave | handles | lines |
|---|---|---|
| Wave 1 | `NEW`, `SEAL`, `SET`, `NOVAE` — allocate names and labels | 145 |
| Wave 1b (was Wave 2) | `SPAN`, `NOVA`, `RVOCA`/`RREDI`, macro-call arguments | 116 |
| Wave 3 | `SEAL` cross-refs, `ALIAS`, `OWN`/`FREE` | 49 |
| Wave 4 | `NOVA`, `LINK`, `SET`, `LAY`, `JEQ`, `JZ`, `CLEAR`, `RVOCA`, `RREDI`, `NOP`, `NOVAE`, `NOLINK`, `ALLOC_TO` — all wiring | 296 |

Outside the waves: `_scan_deferred_names` (sweeps every file for names
`SPAN`/`ALIAS` will define) and `_lumen_prepass` (sweeps every file for
`LINK` counts).

### Reca — 4 waves

| wave | handles |
|---|---|
| Wave 0 | tide — read, strip comments |
| Wave 1 | lcount — count LINK lumina per name |
| Wave 2 | declare — register every declarable name (`SK_DECLARE_ONLY` gate) |
| Wave 3 | load — build the whole graph |

### Why they differ: one root cause

Python's `_resolve` **raises** on a miss — a name must already exist. That
single fact is what its extra scanners and its extra allocation wave exist
to guarantee.

Reca's `BS_INTERN_NOVA` **allocates a correctly-sized stub** on a miss and
carries on. Its own header describes exactly the case Python uses a
barrier for: *"a reference to the name, read before its real `NOVA name op
...` declaration is reached (e.g. from another file, given LOAD_MAIN's
interleaved scheduling), doesn't get handed a too-small 1-lux stub"*.

So Python's barriers are compensation for not having lazy interning — not
a separate need. Every merge below follows from fixing that one thing.

### On the Wave 3/4 boundary

Wave 3's docstring claims it *"must run before Wave 4 so macro entry
points are correct when `_call_reca_macro` executes macros during Wave
4"*. Both halves of that are false: there is no `_call_reca_macro` in the
project (the name appears in that docstring and nowhere else), and Wave 4
does not execute macros — its own comment says it wires them *"Python-side
so LOAD_MAIN can execute"*, building the NOVA luces by hand via `_nova_lux`.
Execution happens later, in Reca.

A naive merge (calling Wave 3's handler inline at the top of Wave 4's
pass) drops self-hosting to 0/42 — but that proves nothing except that
removing a barrier without replacing it breaks things. Between the two
waves today there is a full sweep over every file; calling them back to
back on the same line removes that sweep and puts nothing in its place, so
`SEAL X Y` runs while Y is still unallocated.

With lazy resolve, that case resolves itself: `Y` gets a correctly-sized
stub at first reference, `SEAL` records **that address**, and the real
declaration later fills the same address in. The address never moves, so
what `SEAL` wrote stays correct. The merge is a *consequence* of lazy
resolve, not an independent change — and must not be attempted before it.

## Status

Done.

| step | outcome |
|---|---|
| Lazy resolve in Python | `_resolve` allocates a correctly-sized stub on a miss, sized by `_nova_names` exactly as ordinary allocation is |
| `_scan_deferred_names` | deleted — the barrier it enforced no longer exists |
| Merge the two allocation waves | done, one wave |
| Merge Wave 3 (seal) into it | done — `SEAL` only needs its target's address, and a stub provides it |
| `_lumen_prepass` | kept; it is Reca's lcount by another name, and a genuine need |
| `wax.re` cleanup | "prepass" and "Wave 0.5" both gone |

**Python went from four waves plus two global scanners to two waves plus
one scanner:**

    tide -> lcount -> alloc+seal -> wire

which is the same shape Reca runs. Self-hosting held at 19/42 throughout;
11/11 tests, lint clean, full check suite clean.

### What made it possible

One thing: `_resolve` used to **raise** on a miss. Every extra wave and
every global scanner existed to guarantee that never happened. Reca never
needed any of it because `BS_INTERN_NOVA` has always allocated a
correctly-sized stub at first reference — the address never moves, so
whatever recorded it stays correct, and the real declaration fills the
same lux in later.

### Three mistakes worth recording

**Merging before the thing that makes merging safe.** The first attempt
called one wave's handler from the other's, on the same line, and called
it a merge. It removed a full sweep over every file and put nothing in its
place, so `SEAL X Y` ran while Y was unallocated. It broke, and the
breakage got recorded as "the boundary is real, tested" — a false
conclusion from a broken experiment. The merges only work *after* lazy
resolve, and that ordering is not optional.

**Half-reverted work.** `tide.re` was left mid-conversion to a cache design
that was then abandoned: the buffer had been renamed `TIDE_CACHE_000` while
the code still said `TIDE_CLEAN_BASE`. That dropped self-hosting to 1/42
and cost a long hunt through `LAY` and `SPAN`, both innocent.

**Sizing stubs wrong.** Lazy resolve first stubbed *everything* as NOVA
(7 luces). That oversized every plain register 3.5x, and for the ~280 that
are also OWN it overflowed the OWN block — which had been reserved earlier
using `_nova_names`, the same answer the stub should have consulted. Same
bug shape appeared a second time in the macro-argument pre-allocation,
which also forced NOVA on 80 OWN-marked registers.

## Wave 0: keep the parse — the decision and why

**Terminology, fixed:** `tide` is the whole process, all waves. **Wave 0**
is the first wave inside it: read, strip, statically dispatch. Calling
tide "Wave 0" was wrong and caused real confusion.

### Not a cache — keep the parse

A cache was proposed and rejected. A cache stores *text*; every wave then
re-tokenises it. Keeping the parse stores the *result*, and the waves read
that.

Measured, on this project:

| | luces |
|---|---|
| cleaned text | 255,546 |
| parsed records | 30,524 |

**The parse is 8.4x smaller than the text it replaces**, and the text can
be dropped once parsed — nothing reads it again. So a cache loses on both
counts: slower *and* bigger.

The second reason is more important than the numbers: Python caches
(`_strip_comments_cache`, keyed by path+mtime) because it has a native
`dict` and `mtime` to hand. Copying that shape into Reca would be copying
a convenience Python happens to have, not building the thing Reca needs.
Reca builds its own.

### The record's shape

Positional, mirroring the lux itself:

    [cmd, op, el1, el2, exit, next, at]     empty = 0

Not key/value pairs. Measured why: `key=value` arguments exist only for
`NOVA` (4203), `NOVAE` (150) and `CHAIN` bodies (245, all indented; zero at
top level). And the five keys are not a vocabulary — they are **slot
names**: `El1`→SLOT_E1, `El2`→SLOT_E2, `Exit`→SLOT_EXIT, `Next`→SLOT_NEXT.
A general key/value mechanism would describe what the lux's own shape
already fixes. `At` is the one outlier (it names *where*, not a slot) and
takes the seventh field — zero for everything else. One shape, not two.

### What keeping the parse resolves on its own

This is why it goes first — everything below follows from it, and doing
any of them first means redoing them after:

- **Wave 0 stops running per wave.** Today `TIDE_FILE` is called from
  inside `LOAD_FILE` and `LOAD_PREPASS_FILE`, so every wave re-reads and
  re-strips every file. Measured: 104 calls for 35 files. With the parse
  kept, the waves read records, not files.
- **Unreadable junk disappears.** The third cut (after `//` and `/`):
  prose like `──` and `====` — 552 lines — simply never becomes a record.
  Today it survives to the dispatcher, which silently ignores it.
- **`BS_SKIP_TO_EOL` becomes pointless.** Its 25 callers use it to walk to
  end-of-line after a command took its arguments. With no byte stream to
  position in, there is nothing to skip. (An earlier note in this plan
  called it "not a comment concern, so it stays" — that was reasoning from
  the current state, not the target one.)
- **The waves' input changes.** Which is exactly why merging waves must
  wait: a merge built against a byte stream would be rebuilt against
  records.

### Order

1. **Keep the parse** (this section). Foundation.
2. **Merge Reca's Wave 1 (lcount) + Wave 2 (declare).** After, not before
   — the waves' input changes underneath it.
3. **Unify Reca's backfill.** Last, deliberately. `LOAD_INTERN`'s three
   callers (`LOAD_CRV_SLK`, `LOAD_RKVR_LK`, `LOAD_RS_IMPL`) read a
   *value* by token name and defer the write if the name is not there yet
   — a different job from `BS_INTERN_NOVA`, which mints a *declaration*.
   Whether that difference survives keeping the parse is a question that
   changes shape once 1 and 2 land. Deciding it now would be guessing at a
   problem that may not exist by then.

## Still open

- **19/42 vs 22/42.** Self-hosting dropped three files when tide landed and
  stalls in `lexer.re`. Unexplained.
- **Waves still re-read files.** tide strips each file once per wave rather
  than once per freeze, because its output is bytes, not parsed commands.
  Keeping the parse — the static dispatch this plan opened with — is not
  done.
- **Reca Wave 1 (lcount) still does not register names.** Wave 2
  (declare) exists only to cover that gap. Same fix shape as Python's
  own merge, and for the *same* reason on closer inspection, not a
  different one: both were working around a stub-sizing bug in their
  respective lazy-allocation mechanisms (Python's `_resolve`; Reca's
  `LOAD_CN_INTERN`), and both mechanisms are now fixed. Extend Wave 1
  to register names itself, then delete Wave 2 entirely — no barrier
  to preserve. The "deliberate seam for a scheduler" framing in
  `wax.re` predates knowing the real cause (BUGS.md Pattern Z) and
  should be corrected there, not treated as a constraint here.

## Out of scope

- The `intern.re` self-hosting stall (19/41). Pre-existing, unrelated.
- The two open BUGS.md items (double hash sweep; 681 `macros.re`
  mismatches in `check_graph_equivalence.py`) — unverifiable until
  self-hosting reaches 100%.
