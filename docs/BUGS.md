# BUGS.md — Bug Patterns and Debug Lessons

Records concrete bugs found during self-hosting debugging and the patterns behind them.
The main value is not the bugs themselves — it is the patterns they reveal.

---

## [!] CURRENT STATUS (updated 2026-06-30, later session) — 33%→95%, real IndexError now open

**This supersedes the "14/42" status note below it. Read this first.**

Starting point this session: a clean `freeze()` gave **14/42 (33%)**, matching
the prior session's documented regression, with `fmt.re`'s `SWITCH` parked
mid-body (see the superseded note below for that session's findings —
`val > dest` syntax fix and the `LOAD_RL_IMPL` un-read fix, both correct and
kept). The prior session's "next step" was to trace why the loop-back after
one `SWITCH` case doesn't correctly return to `SWITCH_RDLINE`.

**Root cause found and fixed: a true infinite loop, not a stall.**
`SWITCH`'s per-token reader (`SWITCH_LINE` in `macros.re`) treats an empty
token (`RA_LOAD_TLEN==0`) as "blank line, retry" unconditionally — including
when the empty token means **genuine EOF** (`RA_LOAD_BYTE==0`). At true EOF
this loops forever: `LOAD_READ_TOKEN` → `BS_READ_BYTE` re-confirms EOF →
empty token again → retry `SWITCH_LINE` → repeat, with the real official
`execute_aether(..., max_steps=100_000_000)` call genuinely burning the full
100M-step budget every `freeze()` (confirmed: `_run` returned exactly
`100000001` steps, no exception, ~70s wall time). The previous session's
"parked mid-SWITCH" diagnosis was real but incomplete — the actual failure
mode past that point is this loop, not a stall.

**Fix** (`macros.re`): `SWITCH_T1EJ`'s empty-token branch now goes through a
new check, `SWITCH_T1_EOFCK` (`Equal El1=RA_LOAD_BYTE El2=C_0`), before
deciding to retry. True EOF → `SWITCH_DONE`. Blank line (not EOF) → retry
`SWITCH_LINE` as before. **Verified**: `freeze()` now completes in ~8s
(steps well under the cap), and self-hosting jumped to **40/42 (95%)**.

**Second bug, found while validating the first fix:** after the loop fix,
`fmt.re`'s `SWITCH` (3 simple `val > dest` cases) ran clean, but `diag.py
--lint` showed a NEW phantom ITO: `EMIT_PACKED_STR @ 4622` in `output.re` —
a file completely unrelated to `SWITCH`, never touched this session. Traced
via `trace.py --find-writer` (after fixing that tool, see Tooling below):
`output.re`'s `EMIT_PACKED_STR` lux was getting clobbered by a `JEQ` write
during `yaku.re`'s `ETH_*` `SWITCH` (a *different* `SWITCH` block, using the
**2-token no-arrow form**, e.g. `C_1 ETH_V1`, no `>`). Root cause: per
`loader.py`'s own Python `SWITCH` expansion (`_expand_indent`, mode==
'switch'), the canonical rule is **no arrow → exactly 2 tokens, value then
dest** (3rd+ tokens are silently ignored on that path) — **arrow is required
for 3+ values, confirmed by the project author**. The self-hosted `SWITCH`
never implemented the no-arrow case at all: when T2 isn't an arrow, it
unconditionally treats T2 as *another value* and keeps reading T3, T4...
For a true 2-token line this reads past EOL into the next case (or, worse,
keeps an earlier case's stale `RA_SW_DEST`), corrupting whatever JEQ chain
gets built and clobbering unrelated memory wherever the bogus comparison
values happen to resolve to.

**Fix** (`macros.re`): new `SWITCH_T2_NOARROW` handler — when T3 (read
speculatively after a non-arrow T2) comes back empty, T2's already-resolved
value (captured in `RA_MA3` before the speculative T3 read) is the
destination, not a second value. Lands directly in `SWITCH_EMIT1`'s body
(past its own stale `RA_LOAD_RESULT`-based dest move) with `RA_SW_DEST`
already set correctly.

**Third bug, found chasing the same `EMIT_PACKED_STR` clobber further:**
even after the no-arrow fix, the symptom persisted. Root cause: `macros.re`
re-checks indentation only **once**, at the very start of `SWITCH`
(`SWITCH_RDLINE`, which calls `LOAD_READ_LINE` + checks
`SWITCH_EOFCK`/`SWITCH_INDCK`). Every subsequent case line loops back
straight to `SWITCH_LINE` (raw `LOAD_READ_TOKEN`, no line/indent awareness
at all) instead of back through `SWITCH_RDLINE`. `loader.py`'s Python
`SWITCH` expansion, by contrast, re-checks indentation on **every** raw
line it reads (it's driven by the same per-line indent tracking as the rest
of `_expand_indent`). This is the actual fix the user prompted directly:
*"why not just make it work like Python but in Reca"* — the
answer turned out to be exactly that: do the per-line recheck Python
already does. Compounding bug found in the same check: `SWITCH_INDCK` (and
`NEXO`'s equivalent, `SN_INDCK`) compared `LD_INDENT_DEPTH` against `C_0` —
`LD_INDENT_DEPTH` is a `NEWREF`-style alias whose own `.word` holds
`addr(SK_IND_DEPTH)` (a large, never-zero pointer), not the depth value
itself. The check was **permanently dead** — `LD_INDENT_DEPTH` is correctly
dereferenced through `Voca`-style jump targets, but `Equal El1=` only does a
single dereference, so this usage never worked.

**Fix** (`macros.re` + `saku.re`): all 5 `SWITCH` loop-back points
(`SWITCH_E1_JMP`..`SWITCH_E4_JMP`, `SWITCH_T1_RETRY`) now jump to
`SWITCH_RDLINE` instead of `SWITCH_LINE`. `SWITCH_INDCK`/`SN_INDCK` now
compare `SK_IND_DEPTH` directly. The now-fully-unused `LD_INDENT_DEPTH`
alias was removed from `saku.re`. **Verified**: `diag.py --lint` is fully
clean (`EMIT_PACKED_STR` no longer clobbered); confirmed by re-testing the
fix against the *original* (pre-fix) `comments.re`/`fmt.re` via
`/mnt/project` that the underlying detection logic — see `diag.py
--unwrapped` below — correctly flags a synthetic in-block divider as
risky and correctly leaves real-world (out-of-block) dividers alone.

**Audited all 5 indentation-aware body readers** (`FOR`, `SAVE`, `CHAIN` via
`LOAD_READ_BODY`; `NEXO` via direct `LOAD_READ_LINE` self-looping; `SWITCH`,
now fixed) — only `SWITCH` had this bug. The other four were already
correct; `FOR`/`SAVE`/`CHAIN` because they reuse the shared
`LOAD_READ_BODY` primitive (which itself correctly re-checks indent every
line), `NEXO` because its own loop (`SN_LINE_LOOP`) already self-loops
through the indent-checked entry point on every iteration. **Considered and
rejected**, by explicit author decision: unifying `LOAD_READ_BODY` /
`NEXO`'s loop / `SWITCH`'s loop into one shared higher-order primitive. Only
3 call sites use the indent-check pattern, all now consistent; the
Voca-indirection cost of a real callback abstraction outweighs the benefit
at this scale. Revisit only if a 4th place needs the same pattern.

**Smaller, related bug:** `constants.re` had `NEWREF C_7_REF C_7` (a
cross-ref) sitting physically between `NEWSET C_7 7` and `NEWSET C_8 8`,
breaking the `addr(C_N) = addr(C_0) + 2*N` formula `diag.py --invariants`
relies on (cascading WARNs all the way to `C_5381`), compounded by
`C_10, C_13, C_11, C_12` being declared out of numeric order. **Fixed**:
`C_7_REF` moved after the full `C_N` sequence; order corrected.

**Root-cause-adjacent fix in `loader.py`:** the post-`LOAD_MAIN` restore
pass that fixes `NEWREF X Y` cross-refs corrupted by the builder (documented
below as "L. Hash-table..." class of issue) was hardcoded to scan
`macros.re` only. Generalized to scan every `.re` file's `NEWREF` pairs —
this is what let the `C_7_REF` fix actually take effect, and is itself the
same "universal over special-cased" principle applied to the restore logic.

**Source typo, unrelated class:** `comments.re` had a stray literal `ITO `
prefix on the last line of two separate `CHAIN` blocks (4 sibling lines
without it, 1 with it, also over-indented) — misparsed as a fresh,
malformed item instead of `CHAIN`'s expected bare `NAME op args` form.
**Fixed**, both occurrences.

**Naming bug:** `macros.re`'s `NEWREF SWITCH_RESOLVE_NAME SW_RN_IMPL`
declared an entry point name, `SW_RN_IMPL`, that no command in the file was
actually labeled with (the real first command was named `SW_RN_ANON`) —
`SW_RN_IMPL` sat permanently unwired (`op=0`) until something jumped to it.
**Fixed**: renamed `SW_RN_ANON` → `SW_RN_IMPL`.

### Remaining open bug: `IndexError` in `Read`, blocking the last ~5%

With all of the above fixed, self-hosting holds at **40/42 (95%)** and
`freeze()` now fails fast and cleanly with a real Python exception instead
of silently stopping or looping:
```
File "interpreter.py", line 559, in _read
    if exit and a1: aether[exit] = aether[aether[a1]]
IndexError: array index out of range
```
Traced (via a class-level `Interpreter._run` patch, watching `K_WATERMARK`'s
real address `88` vs the alloc-count check `ALLOC_LUCES` performs): the
crash is inside `ALLOC_LUCES` itself (`alloc.re`, `AC_WM_CUR`), at step
~6.86M of ~15.8M total. `AC_WM_CUR`'s own `e1`/`exit` slots (which should
permanently hold `K_WATERMARK`'s and `RA_ALLOC_TMP2`'s real addresses, 88
and 106) were found overwritten with `614519` (a fresh/anonymous bump-area
address) in **both** slots — i.e. `AC_WM_CUR`, an already-correctly-wired
core lux, gets clobbered mid-run.

Mechanism identified precisely: `saku.re`'s `LOAD_CMD_ITO` "find or create"
logic (`LOAD_CI_NINT BS_INTERN` → `LOAD_CI_EWGET Read El1=SK_TMP
Exit=SK_ITO_ADDR` → `JZ ... LOAD_CI_OPTOK_NEW`) interns *some* name token,
reads its `.word`, finds it non-zero (`6061` = `AC_WM_CUR`'s own address),
and concludes "this name already maps to an existing ITO" — then proceeds
to **overwrite that existing ITO's slots** as if updating it. The name
token actually being interned at that point, sampled at the earliest
possible point (`LOAD_CI_NADDR`, right after `BS_INTERN`, before the
op-token re-read that would otherwise clobber the sample), is **not valid
text** — repeating non-printable byte + NUL patterns (`'^\x00\x00\x00^...'`
this run; `` '`\x00\x00\x00`...' `` in the earlier `EMIT_PACKED_STR`
investigation, different byte, same shape). `RA_LOAD_RPOS`/`RA_LOAD_RLEN`/
`RA_LOAD_FD` sampled at the same moment point at a *clean*, ordinary
position in `accord.re`'s real file content (`RPOS=660` lands on
`"...ACCORD_RULE ACCORD_STABLE..."`, completely normal text) — i.e. the
garbage token does not correspond to whatever the file-position registers
claim is being read, suggesting those registers are stale relative to
whatever actually produced the bad token, not that the file itself is
corrupt. A `Write`-based write-watch on the token buffer's own address
range (`BS_TOKBUF_BASE`, 64-lux window) caught **zero** writes leading up
to the corruption, even though the buffer's content visibly changed —
meaning either the token buffer is being written through some path other
than a plain `Write` op (e.g. via `Move` with a computed `Exit`, untracked
by that probe), or the apparent "garbage" is itself a red herring from a
sampling/timing mismatch in the probe rather than the buffer's real state
at the moment of interning.

**This was not resolved this session** — three consecutive differently-angled
probes (sample-after-the-fact, sample-at-earliest-point,
write-watch-on-the-buffer) each surfaced new, partially contradictory
detail without converging on a root cause, which is itself the signal to
stop rather than keep refining probe precision. The thread is at a clean,
well-documented handoff point, not abandoned mid-confusion:

**Next steps for a fresh session:**
1. Verify the write-watch probe's own correctness first — it may simply be
   watching the wrong op type (Write vs Move-with-computed-Exit) or the
   wrong address range, given it caught zero writes despite an observed
   content change. Don't trust "zero writes" as a finding until that's
   ruled out.
2. Once the real write path is found: is `BS_INTERN`'s hash lookup
   returning the wrong existing lux for a *different*, legitimate name
   (a genuine collision/probing bug in `HT_INSERT`/`HT_LOOKUP` — the
   correctly-fixed DJB2 algorithm from earlier this session should rule
   out the hash function itself, but the *insert* path wasn't re-audited
   with the same rigor as the lookup path was), or is the token being
   interned already garbage *before* `BS_INTERN` sees it (a `BS_READ_TOKEN`
   bug, not a hash table bug)?
3. `accord.re` (fi context at the time) is a normal, clean file — this is
   very likely cross-file address bleed (something processed earlier,
   possibly while still working through `alloc.re`'s own self-hosted
   pass, left `K_CURSOR`/the hashtable in a state that a later file's
   processing reads incorrectly) rather than anything wrong with
   `accord.re`'s content itself.

### Tooling fixed/added this session

- `trace.py --loadmain`: crashed outright (`AttributeError` — `Interpreter`
  uses `__slots__`, so `l.interp._do_syscall = bound_method` style instance
  monkeypatching silently fails). **Fixed**: patch at the class level
  (`type(l.interp)._do_syscall = unbound_func`) for all 4 patched methods.
  Also fixed a **separate, pre-existing double-`self`** bug in the same
  function: each wrapper captured `orig_X = l.interp.X` (already a *bound*
  method) then called `orig_X(self, ...)`, passing `self` a second time —
  fixed by capturing `type(l.interp).X` (unbound) instead. This mode is
  still architecturally awkward (calls `freeze()` twice, see the dead
  `l2 = freeze()` code) and wasn't relied on after the class-level
  `Interpreter._run` patching approach (see Next Steps) proved more direct
  for this kind of investigation — worth a proper rewrite, not done here.
- `check_comments.py`: `--threshold N` and `--spans-only` were documented
  in the module docstring but never actually read from `sys.argv` — pure
  no-ops, plus `THRESHOLD`/`_HERE`/`all_re` were defined twice in the same
  file. **Fixed**: both flags now work; duplicate definitions removed.
- `diag.py`: added `--block-lint` (heuristic finder for indent/style
  outliers inside `CHAIN`/`SWITCH`/`FOR`/`WAVE` bodies — generalizes the
  `comments.re` stray-`ITO` bug pattern; reuses `loader._SPECIAL_OPENERS`
  rather than a new hardcoded list) and `--unwrapped` (finds non-ASCII
  decorative lines, e.g. `── Section ──` dividers, and classifies them as
  safe (outside any tracked block — the top-level dispatcher's
  `LOAD_CMD_UNKNOWN: word==0 → skip rest of line` already handles these,
  mirroring `loader.py`'s own Python-side unmatched-`if`-chain fallthrough)
  or risky (textually inside a block body — the exact class of bug that
  broke `fmt.re`'s `SWITCH`). Currently 0 risky lines project-wide;
  synthetically verified the detector correctly flags a divider placed
  inside a real `SWITCH` body. Also added `_load_fast()` (uses
  `loader.load_or_freeze()`, now staleness-aware — see below — instead of
  always paying full `freeze()` cost) for the 6 modes that only need
  `interp`+`symbols`, not full `Loader` build internals
  (`mode_strings`, `mode_macros`, `mode_load_main`, `_mode_wiring`,
  `_mode_htable`, `mode_invariants`); the other 4 modes
  (`mode_health`, `mode_graph`, `mode_lint`, `_mode_broken`) genuinely need
  `Loader._ito_addrs`/`_bump`/etc. and still call the heavier `_load()`.
- `loader.py`: `load_or_freeze()` previously thawed `reca.bin` whenever it
  merely *existed*, with no staleness check — risking silently stale
  results after any source edit. Added `_bin_is_stale()` (generalizes
  `gen_compiler.py`'s existing `_needs_regen()` mtime-check pattern rather
  than inventing a new one): compares `reca.bin`'s mtime against every
  `.re` file and `loader.py`/`interpreter.py`/`symphony.py`. `load_or_freeze`
  now only thaws when genuinely fresh.
- Note for the future: `diag.py --htable` is **broken** (uses XOR to
  combine the running hash instead of the project's actual `hash*33+byte`
  ADD-based DJB2, and doesn't account for the runtime hashtable's packed
  `(hash32<<32)|lux_id` slot format with linear probing) — confirmed via a
  from-scratch correct reimplementation that every symbol checked (`C_7_REF`
  → `CMT_LOOP`, `BS_READ_TOKEN`, etc.) resolves fine through the *real*
  algorithm. `--htable` itself was not fixed this session (out of scope at
  the time it was found) — anyone debugging hashtable issues should not
  trust `diag.py --htable`'s output until it's fixed to match
  `htable.re`/`intern.re`'s actual `BS_LOOKUP`/`HT_LOOKUP` implementation.

### Documentation convention established this session

`check_comments.py` flags prose that legitimately starts a line with a
command-like word (e.g. "Set via Move before Exire...", or example syntax
like "NEW MyNote") as a possible missing-`//`-bug. Convention: wrap the
word in quotes — `"Set" via Move...`, `"NEW" MyNote` — to silence the
false positive without weakening the check (it's a literal first-word
string match; quoting breaks it without changing what the prose says).
Every flagged line in the codebase was individually verified safe this way
(read the surrounding `//` block, confirm it's genuinely closed) before
quoting — documented in both `check_comments.py`'s own docstring and
`CONVENTIONS.md` under "Command-like words inside prose: quote them".

---

## [!] CURRENT STATUS (updated 2026-06-30) — UNDOCUMENTED REGRESSION: 42/42 → 14/42 — HISTORICAL, SUPERSEDED ABOVE


**This supersedes every "current blocker" note below it. Read this first.**

`ROADMAP.md` (D3, written before the 2026-06-24 project snapshot) states as
settled fact: *"B6 still says '33/42' — actual is 42/42. Update to done."*
This means **full self-hosting (42/42 files) was reached at some point**,
documented as a completed fact in ROADMAP.md, but never folded into BUGS.md's
running narrative.

**As of this session (2026-06-30), a clean `freeze()` gives 14/42 (33%)** —
worse than even the 33/42 (78%) baseline from the 2026-06-19 session below,
let alone the claimed 42/42. **No commit, fix, or note anywhere in this repo
explains the regression from 42/42 → 14/42.** This is the single most
important open question for the next session: something broke full
self-hosting and nobody wrote down what or when.

**What this session confirmed, concretely:**
- `fi=14` (`fmt.re`) is the current hard stop. Only 8 real dispatches happen
  for this file (`NEWREF`, 4×`NEW`, 2×`ITO`, `SWITCH`) before the file loop
  silently stops advancing `SK_FIDX` — no crash, no hang, `freeze()` returns
  cleanly, it just never reaches files 15–41 in the main pass (the Python
  prepass *does* reach all 42, confirming the files themselves are readable;
  this is a main-pass/self-hosted-compiler-only bug).
- `fmt.re`'s `SWITCH` body cases were written as `val dest` but the
  self-hosted `SWITCH` macro (`macros.re`) expects `val > dest` (it literally
  checks for the `>` byte between tokens). **Fixed** — `fmt.re` now reads
  `0 > FMT_DONE`, `1 > FMT_IS_INT`, `2 > FMT_IS_STR`. This is a genuine bug
  fix and should be kept regardless of what else is found, but it alone did
  **not** move the score off 14/42.
- `LOAD_READ_LINE` (`LOAD_RL_IMPL` in `saku.re`) peeks one content byte per
  line to detect indent depth, then consumed that byte permanently instead
  of un-reading it before returning — meaning the first byte of every
  terminating (non-indented) line was silently dropped, corrupting the next
  token read by the caller. **Fixed**: added an `RA_LOAD_RPOS -= 1` un-read
  step before every non-EOF return path in `LOAD_RL_IMPL` (depth=0 and
  depth≥1 cases; EOF paths correctly skip the un-read since there's no byte
  to give back). Verified byte-for-byte against `fmt.re`'s actual file
  content that `SWITCH`'s body reader now reads correctly. This is also a
  genuine, real bug — independently worth keeping — but did **not** alone
  fix the score either.
- With both fixes applied, `SWITCH` in `fmt.re` reads its first case line
  correctly (confirmed via `diag.py --invariants`, which now reports the
  frozen state as `RA_LOAD_BYTE=10(LF), SK_IND_DEPTH=1, RA_LOAD_RPOS=1009,
  SK_FIDX=14` — i.e. execution is parked mid-`SWITCH`, having read at least
  part of the case list, never reaching `SWITCH_DONE`/the file loop again).
  **Next step for a fresh session:** trace `SWITCH_RDLINE`/`SWITCH_DONE` in
  `macros.re` specifically — does the loop that's supposed to go back to
  `SWITCH_RDLINE` after processing one case line actually fire for a
  3-case `SWITCH` block? This looks like the first time a self-hosted
  multi-case `SWITCH` (`val > dest` syntax, several case lines) has actually
  executed end-to-end through `LOAD_MAIN`, so treat this as **possibly
  unexplored territory**, not a regression of previously-working code.

**Tooling bug found and fixed in passing:** `diag.py`'s entire CLI dispatcher
(the `if '--lint' in els: ... elif '--invariants' in els: ...` block) was
physically nested *inside* `mode_invariants()`, **after** its `return`
statement, and the file had no `if __name__ == '__main__':` guard at all.
Running `python3 diag.py <any flag>` silently did nothing (exit 0, no
output) — meaning `--invariants` (the most useful postcondition checker in
the repo) has been **non-functional for an unknown stretch of time**. This
plausibly explains how a 42/42 → 14/42 regression could happen unnoticed:
the one tool designed to catch this class of problem immediately wasn't
running. **Fixed**: moved the dispatcher to module level under a proper
`if __name__ == '__main__':` guard. Verified working — see TOOLING.md.

---

## Context (updated 2026-06-19, end of "deep dive 2" session) — HISTORICAL, SUPERSEDED ABOVE

**Goal:** LOAD_MAIN (Reca itself, not the Python loader) successfully processes
all 42 project files end-to-end — this is the self-hosting milestone.

**Result this session:** self-hosting progress went from **4% (4/42 files) to
78% (33/42 files)**, via a long chain of systemic root-cause fixes (see
"Session 2026-06-19: Self-Hosting Deep Dive — Systemic Fixes" below for full
detail on each). The previous "Pattern 1–Q" findings below this section are
from an **earlier, separate session** with a different hypothesis
(`LOAD_MA_READARG`/`RA_LOAD_BYTE=LF`) that turned out not to be the real
blocker chain — kept for historical record, but **do not treat the old
"Root Cause Chain (hypothesis)" section below as current** — it was never
confirmed and is superseded by this session's empirically-verified findings.

**Currently blocking further progress:** a call-stack imbalance (`RA_SP`
underflow/overflow) while LOAD_MAIN processes `saku.re` (file index 33).
Full detail, hypotheses, and a recommended next-step plan are in the new
session section below ("Open issue: call-stack imbalance around RREDI's
self-referential generic-macro definition").

---

## Session 2026-06-19: Self-Hosting Deep Dive — Systemic Fixes

This section documents a long, single-session investigation that moved
self-hosting from **4% (4/42 files) to 78% (33/42 files)**. Every fix below
was found via empirical tracing (custom Python scripts instrumenting
`Interpreter._run`/`_wave2_line`, not guessing) and confirmed via a clean
`from loader import freeze; freeze()` re-run after each change. All fixes
are applied in the working tree; **none have been delivered to the user as
downloadable files yet** — this file (and the source `.re`/`.py` changes
listed below) is the handoff point for a fresh session to pick up.

**How to resume:** read this whole section once, then re-run
`python3 -c "from loader import freeze; freeze()"` in the project root to
confirm the baseline (should print "Self-hosting: [...] 33/42 files (78%)"
with no Python exception — if it prints an `IndexError` traceback from
`interpreter.py:_redi`, that's the open issue described at the end of this
section, not a regression). Diagnostic scripts referenced below are reusable
and still present alongside the source files.

### A. Allocator under-sizing (`loader.py`) — FIXED

Wave-1's generic "Unknown command" pre-allocation gave names that are *later*
RVOCA/RREDI/ITO targets a small 2-slot layout instead of the required 7-slot
(`ITO_SIZE`) layout, because `_define`'s "already exists → return as-is"
guard silently no-ops any later resize attempt. Fixed via a new
`_scan_ito_names(filepath)` prepass (scans every file *before* any wave
allocates anything) feeding `self._ito_names`; `_alloc_data` now checks this
set first and redirects to `_alloc_ito`. Originally covered RVOCA/RREDI/ITO
target names; **this session extended it to also cover JZ/JEQ/CLEAR/NOP/
ALLOC_TO target names** (see section H below) — same hazard, different
trigger (Wave-B0's NEWREF alias resolution caching a stale pre-`_ito_lux`
address instead of the final one).

### B. Systemic "missing closing `//`" comments in `yaku.re` — FIXED

Pervasive author habit of writing `//remark//` self-closing comments but
periodically dropping the trailing `//`, causing huge stretches of *real
code* to be silently swallowed as comment (including, ironically, `yaku.re`'s
own LLVM-IR template table at its own file header). ~37 individual closures
fixed; verified via `check_block_comments.py` (faithful re-implementation of
`loader.py`'s `_strip_comments`) + `classify_spans.py` (flags spans whose
swallowed content starts with a real command keyword). **Project-wide
re-check confirms `yaku.re` is now clean.** One candidate outside `yaku.re`
(`aria.re:139`) was flagged by the classifier as suspicious but was
**directly verified via exact byte-level simulation of `BS_BLOCK_SKIP`'s
own toggle logic to be a false positive — it closes correctly** (see section
F below for the *actual* `BS_BLOCK_SKIP` bugs that were found, which are
unrelated to this specific span). No other files were exhaustively
re-checked beyond `yaku.re`; rerun `classify_spans.py` against the whole
project if a similar symptom resurfaces elsewhere.

### C. "OUTER register" dead-code pattern (`yaku.re`) — FIXED

A pre-automatic-call-stack idiom (manually saving a return address into a
named `RA_xxx_OUTER` register before `Jump`ing to a subroutine) was dead —
the register was written but never read by anything (no `JumpReg`-style
consumer exists). Replaced 4 instances with the proper, already-existing
`RCALL_AT name sub landing` mechanism (`macros.re`), which integrates
correctly with the automatic call stack. Also removed one related dangling
`LINK EB_SAVE_RET Next BFS_EMITJ` line that never worked in the first place
(`LINK` always writes to extra lumina via `_add_lumen`, never to the compact
`Next` slot — confirmed via direct code reading of `loader.py`). A second,
more ambiguous instance (`EM_VS_OUTER`, a save/restore of `S_RA_LINK` around
a `VS_TEST_SET` call) was investigated **this session** and also confirmed
dead (project-wide grep: nothing else ever writes `S_RA_LINK`) — removed,
with the now-stale caution comment updated.

### D. `P0_ALLOC_EPBUF` missing label (`yaku.re`) — FIXED

Stale forward-reference from a refactor (likely when `EP_BUFFER` allocation
was generalised into the `ALLOC_RAW` macro, which generates auto-counter
names that can't match a fixed target). Fixed with a `NOP P0_ALLOC_EPBUF`
landing marker right before `NEWSET EP_BUF_MAX 32` in `P0_COLLECT`.

### E. `LOAD_PREPASS_FILE`'s `LOAD_PP_SKIP` swallows the next line (`saku.re`) — FIXED

**This was the actual cause of the original "4/42, stuck on file index ~4
(aria.re)" blocker, and of several subsequent "stuck on file N" symptoms.**

`LOAD_PP_LOOP` → `LOAD_PP_TOK` collects a token via `LOAD_DL_COLLECT`, which
stops exactly at LF for single-token lines with no embedded space (e.g. a
bare `====...====` separator line). If the collected token doesn't start
with `LI` (not a LINK line), the code unconditionally called
`LOAD_PP_SKIP → BS_SKIP_TO_EOL` — but `BS_SKIP_TO_EOL` *always* scans
forward to the *next* LF with no awareness that the cursor might already be
sitting on one. Result: it silently consumed the **entire following line**
(including any comment-opening `//` on it), corrupting the prepass's view of
file structure for every subsequent line.

Fix applied in two layers (see section M-prelude below for why two):
1. (later superseded) An initial guard was added locally in `LOAD_PP_SKIP`
   itself (`JZ`/`JEQ` checking `RA_LOAD_BYTE` before calling
   `BS_SKIP_TO_EOL`).
2. **The real, systemic fix**: `BS_SKIP_TO_EOL` itself (`lexer.re`) was
   given the guard instead (see section M-actual below), making it a safe
   no-op when already at LF/EOF. This fixed *every* call site project-wide
   (~20 of them) in one place, not just this one — the local guard in
   `LOAD_PP_SKIP` was then **removed again** as redundant, restoring it to a
   plain `RVOCA LOAD_PP_SKIP BS_SKIP_TO_EOL` call.

### F. Five label-name typos in `BS_READ_TOKEN`/`BS_BLOCK_SKIP` (`lexer.re`) — FIXED

A `JEQ`/`JumpIf` destination must match a name that's actually *defined*
elsewhere as a label — these five didn't (presumably a copy-paste typo
propagated 3 times across near-identical comment-detection blocks):

| Wrong destination (referenced, never defined) | Should be (the real label) |
|---|---|
| `BS_RT_SL2`   | `BS_RT_SL2B` |
| `BS_RT_BLKSK` | `BS_RT_BLK` |
| `BS_RT_SLCK`  | `BS_RT_SLRB` |
| `BS_RT_SLBL`  | `BS_RT_SLBK` |
| `BS_BLK_SL2`  | `BS_BLK_SL2B` |

Before this fix, `BS_BLOCK_SKIP`/`BS_READ_TOKEN`'s comment-detection would
crash (op=0, "no handler") the first time it needed to take these specific
branches — i.e. whenever a lone `/` (not part of a `//` pair) appeared
inside or adjacent to scanned content in a way that exercised these exact
paths. Found via direct empirical isolation (writing a known byte chunk
into `BS_READBUF_BASE` and repeatedly calling `BS_READ_TOKEN` via
`interp.execute_aether`, observing each produced token) after a pure
byte-level *simulation* of the same algorithm (in Python) failed to
reproduce a symptom that real execution showed — the simulation matched the
*intended* algorithm, not the *actual* (typo'd) wiring, which is what
revealed the typos.

### F2. `aria.re:129` — unclosed `//` swallowed `NEW LOAD_START` as comment — FIXED (found earlier in the same investigation, undocumented until now)

A genuine instance of the same comment-swallowing pattern as section B, but
in `aria.re`, not `yaku.re`. `//word = return Lux ID (caller sets before
Jump LOAD_START)` (line 129) was never closed on its own line. Verified via
exact byte-level toggle simulation of the *original* (`/mnt/project/`)
file: the next `//` pair (inside the following section's own opening
comment) closed it instead, which meant the **next** `//` after *that*
opened a **new**, unintended span that swallowed `NEW LOAD_START` itself
(the actual declaration of the `LOAD_START` stub label) as if it were
comment text. Fixed by closing line 129's comment on its own line
(`...LOAD_START)//`). Confirmed via the same byte-level simulation re-run
against the fixed file: the span now closes at exactly 61 bytes (the
intended single-line comment), and `NEW LOAD_START` no longer appears
inside any comment span.

### G. `ASCII_EQ` does not exist — should be `EQUALS` (`lexer.re`, 2 places) — FIXED

`BS_TOKEN_VALUE` (strips a `"key="` prefix before recomputing a hash) and
`BS_SCAN_EQ` both compared a byte against a constant named `ASCII_EQ`, which
is never `NEWSET` anywhere in the project (project-wide grep confirmed). The
real `'='` constant is named `EQUALS` (`ascii.re:43`, value 61) — a simple
naming mismatch with the same root pattern as section F. Because the
referenced name was unresolved, Wave-1's generic "Unknown command"
pre-allocation gave `ASCII_EQ` a fresh stub lux with a effectively-arbitrary
self-ref address (*not* the value 61) — meaning the comparison silently
always failed, making `BS_TOKEN_VALUE` a no-op for every input (confirmed
via isolated test: feeding it `"El1=RA_TMP2"` returned the string
unchanged instead of stripping to `"RA_TMP2"`).

### H/I. `ALLOC_TO` needed Python-level scaffolding, like JZ/JEQ/CLEAR/NOP/RCALL_AT already have (`loader.py`) — FIXED

`ALLOC_TO name dest count` is a generic Reca-level macro (`macros.re`), not
hand-coded in any `.re` file's bytecode directly — it's used by `BS_INTERN`'s
lazy-allocation path (`intern.re`) and by `SAVE_EMIT_ONE_SAVE` (`saku.re`),
both of which are needed *very early* in LOAD_MAIN's own bootstrap (e.g. to
intern the very first never-before-seen identifier it reads). But unlike
JZ/JEQ/CLEAR/NOP/RCALL_AT, `ALLOC_TO` had **no dedicated Python fast-path**
in `_wave2_line` — meaning its target instructions were *only* ever wired by
LOAD_MAIN's own runtime execution of `macros.re`'s text (file index 25),
which is far too late for callers in earlier-processed files. Added a
Python-side fast-path mirroring the existing JZ/CLEAR pattern: recognises
`ALLOC_TO name dest count`, derives `name_J`/`name_K` exactly as the Reca
macro does, and wires the 3 chained ITOs (`Move(count→RA_ALLOC_COUNT)`,
`Voca(ALLOC_LUCES)`, `Move(RA_ALLOC_RESULT→dest)`) directly via `_ito_lux`.
Also extended `_scan_ito_names` (section A) to register `ALLOC_TO`'s name/
`_J`/`_K` triple for the same address-stability reason JZ/JEQ needed it.

### J. `SWITCH_RESOLVE_VALUE` — Reca-level numeric-literal fix for `SWITCH` (`macros.re`) — FIXED, but turned out to be a secondary fix

Added a drop-in replacement for `LOAD_INTERN_TOKEN` inside `macros.re`'s
generic `SWITCH` macro definition, so that a bare numeric case-value (e.g.
`"0"`) resolves to a fresh register *holding that value* instead of being
interned as an arbitrary identifier string (which would compare against an
unrelated address at runtime). **Important caveat discovered later:** the
`SWITCH reg \n VAL DEST` *inline, indented* syntax used throughout the
project (e.g. in `lexer.re`'s `BS_READ_TOKEN`) is actually **pre-expanded by
Python's own `_expand_indent`** (`loader.py`, `mode == 'switch'`) directly
into `JEQ name reg VAL DEST` text lines — it never goes through this
Reca-level macro at all for that syntax form. This Reca-level fix is still
correct and worth keeping (it fixes the *generic, Voca-called* form of
`SWITCH`, used e.g. by `LOAD_READARG_KV`'s `SWITCH SK_POS_SLOT \n 1 ... \n
2 ...` style calls), but **the actual production blocker (section K) was in
the Python-side `_expand_indent`/`_resolve` path, not here.**

### K. `_resolve()` resolving bare numeric literals to their literal value instead of a register address (`loader.py`) — **the big one** — FIXED

**Root cause of the original `SWITCH`-based infinite loop (BS_READ_TOKEN
never recognising EOF, endlessly re-reading the same exhausted file
buffer).** Every ITO field (`e1`/`e2`/`exit`) is read by the interpreter
through a dereference — `aether[a1]`, `aether[a2]`, etc. `_resolve(name)`,
when `name` wasn't a known symbol but parsed as a plain non-negative
integer, simply **returned that integer directly** as if it were an
address. Two compounding effects: (1) at runtime this makes the interpreter
compare against *whatever happens to live at that low address* instead of
the intended constant; (2) for literal `0` specifically, several ops'
truthiness-gated logic (e.g. `Equal`'s `a1 and a2 and ...`) treats a *zero
address* as "no operand", silently skipping the comparison rather than
performing it. The project's own established convention already avoids
this by hand for the single most common case — `JZ`'s definition
(`macros.re`) never compares against bare `"0"`, it uses `RA_C0_REF` (the
*address* of the `C_0` constant register) — this fix simply generalises
that same pattern to `_resolve` itself, so it applies uniformly to *every*
bare numeric operand resolved through it (including ones synthesised by
`_expand_indent`'s `SWITCH`-to-`JEQ` expansion, which is exactly the path
that produced the original bug).

Fix: `_resolve` now looks up (or lazily allocates and memoizes, in a new
`self._lit_const_cache: dict`) a `"C_<value>"` register for any bare
non-negative integer literal not already a known symbol, reusing an
existing `C_N` constant where the project already defines one. Verified
safe against every other call site of `_resolve` (the one place that
*does* want a literal value directly, `_wire_word_value`, already has its
own `try: int(parts[2])` branch *before* ever calling `_resolve`, so it's
unaffected).

This single fix alone took self-hosting from a hard infinite loop
(file-buffer-boundary EOF never detected) straight through to **25/42
files (59%)** once combined with section M below.

### L. Hash-table architecture bug — premature 18-bit masking defeats collision resolution (`lexer.re`, `intern.re`, `loader.py`) — FIXED

**A second, independent, and more fundamental hash-collision bug**,
discovered while chasing a crash where `BS_LOOKUP("PS_LX_SFUNC")` returned
the address of a completely unrelated symbol (`PBKT_103`, a large packed
numeric constant in `parser.re`).

`htable.re`'s own design (see its file header) stores `(hash32 << 32) |
lux_id` per slot and is *supposed* to receive a wide (≥32-bit) hash, masking
only internally (`RA_HT_MASK`, fed from `BS_HT_MASK`) to pick the slot
index — while *keeping the full hash* for the upper 32 bits, so that two
names which happen to collide on slot index (inevitable in any reasonably
full hash table) can still be told apart by comparing the *stored* hash.

But `lexer.re`'s own hash-computation routines (`BS_RT_HASH0`/`BS_TV_HASH0`/
the parser.re-mirrored ones) **masked `RA_LOAD_HASH` down to the narrow
18-bit `BS_HT_MASK` after every single byte**, before the hash ever reached
`HT_LOOKUP`/`HT_INSERT`. This makes the "stored hash" comparison meaningless
— for any two names that collide on the (narrow) slot index, their stored
"distinguishing" hash is *also* the same narrow value, so the comparison
always matches, silently returning the wrong symbol. Confirmed empirically:
`djb2("PS_LX_SFUNC")` and `djb2("PBKT_103")`, masked to `BS_HT_MASK`'s 18
bits, are both exactly `88681`.

**`BS_HT_MASK` itself is correct and unchanged** (`DECISIONS.md`'s "Stable
Invariants" entry, `0x3FFFF`/262144 slots, still holds — it's still the
right size for *slot-index* masking). The bug was applying that same narrow
mask a second, premature time, baking it into the hash *value* itself
before it could be used for genuine wide collision comparison.

Fix, in two parts that must stay in sync:
1. **`lexer.re`**: removed the `And ... BS_HT_MASK ... Exit=RA_LOAD_HASH`
   step from every inline hash-accumulation loop (6 locations: `lexer.re`'s
   `BS_RT_MASK`/`BS_TV_HMASK`, and `saku.re`'s `LOAD_MAJ_JU2`/`LOAD_MAJ_JJ3`/
   `LOAD_MZJ_JU2`/`LOAD_MZJ_JK3`/`LOAD_PP_H2`/`LOAD_DL_CO_HMSK` — note that's
   actually 8 sites once `saku.re`'s are counted; see the diff for the exact
   list). The hash now stays a full 64-bit running value (natural register
   wraparound only, same as `DECISIONS.md`'s existing "DJB2 hashing" entry
   already describes).
2. **`intern.re`**: `BS_LOOKUP`/`BS_INTERN`/`BS_INTERN_NAMED` now mask to
   `MASK_LOW32` (`math.re`, `0xFFFFFFFF` — matches `htable.re`'s own
   "hash32" format) at the point they hand the hash to `RA_HT_HASH`, instead
   of a bare `Move`. Added `math.re` to `intern.re`'s `DEPENDS ON` header.
3. **`loader.py`**: the Python-side `BS_HTAB` pre-population (inside
   `freeze()`) was computing the *exact same* narrow-then-narrow mistake
   (`_h_masked = _h & _ht_mask` used for *both* slot index *and* the stored
   comparison value) — updated to mirror the Reca-level fix: `_h & _ht_mask`
   for the slot only, `_h & _M32` (`0xFFFFFFFF`) for the stored value.

This fix alone took self-hosting from 59% to **66% (28/42)** before section
M's fix (below) pushed it further.

### M-actual. `LOAD_EXPAND_TEMPLATE` has no termination path — infinite loop (`saku.re`) — FIXED

A second, *genuine* infinite loop (distinct from section K's), found after
section K/L were fixed and self-hosting got far enough to actually reach
`saku.re`'s `LOAD_EXPAND_TEMPLATE` (the `FOR`-loop template-body expander).
`LOAD_ET_LOOP`'s only exit condition was `Equal(SK_ET_PTR, SK_BODY_PTR)` →
`JumpIf → LOAD_ET_FLUSH`. The problem: **neither `SK_ET_PTR` nor
`SK_BODY_PTR` ever changes again once they become equal**, so once the body
is exhausted, *every* subsequent loop iteration re-takes the same branch,
re-running `LOAD_ET_FLUSH_DISPATCH` (dispatching an increasingly pointless,
empty "line") forever. The function's own `RREDI LOAD_ET_DRET` (its
intended return point) was **completely unreachable** — nothing in the
function body ever jumped to it; it sat right after an unconditional `Jump`
back to `LOAD_ET_LOOP`, permanently dead.

Fix: added a new register `SK_ET_DONE` (cleared at function entry). The
end-of-body branch now goes to a new `LOAD_ET_ENDCK` guard: if
`SK_ET_DONE` is already 1, return immediately (`RREDI LOAD_ET_DRET`, now
genuinely reachable); otherwise set it to 1 and proceed to flush exactly
once. This is the same "do at most one final action, not infinitely many"
shape as section E's fix to `BS_SKIP_TO_EOL`.

This fix alone took self-hosting from 66% to **78% (33/42)** — the current
checkpoint.

### N. `LOAD_CMD_BLOCK`'s `NOLINK` blocks a `JZ`'s own fallthrough auto-chain (`saku.re`) — FIXED

A third, distinct bug class (broken auto-link, not infinite loop), found
immediately after section M when self-hosting progressed far enough to
reach a *second* `BLOCK name count` declaration for the same already-stubbed
name (`SK_PLINK_BUF`, declared via `BLOCK SK_PLINK_BUF 2048` then `SETREF
SK_PLINK_BUF SK_PLINK_BUF` right after it in `saku.re`'s own header area).

`LOAD_CMD_BLOCK`'s body: `JZ LOAD_CB_EXCZ SK_FLAG LOAD_CB_SKLS` followed
immediately by `NOLINK` then `RVOCA LOAD_CB_DONEW BS_READ_TOKEN`. The
`NOLINK` is *necessary* (it stops `LOAD_CB_DONEW` from being silently
auto-chained into by whatever happens to precede it in the file — it's also
the explicit jump target of an *earlier* `JZ` a few lines up) — but it has
a side effect nobody accounted for: it *also* blocks the **immediately
preceding** `JZ`'s own fallthrough (the case where `SK_FLAG != 0`, i.e. "no
jump taken") from reaching `LOAD_CB_DONEW` automatically. Without an
auto-link target, that fallthrough defaults to `pc + ITO_SIZE` — i.e.
*whatever instruction happened to be allocated immediately afterward in
memory*, which (by allocation-order coincidence) turned out to be deep
inside `LOAD_CMD_NEWREF`'s own internal labels. Execution would silently
jump into the middle of an unrelated handler, eventually writing through a
garbage/zero pointer (`RA_BS_EL0` never set by the path it actually took)
and corrupting **Aether address 0** (the system's "always zero" sentinel) —
which then caused a *separate*, very confusing downstream symptom (`SK_TMP`
reading `aether[0]` as a "valid" macro address whenever some *unrelated*
later `SK_CMD_ADDR` lookup genuinely missed).

Fix: added an explicit `ITO LOAD_CB_EXFALL Jump Exit=LOAD_CB_DONEW`
immediately after the `JZ`, before the `NOLINK` — so the fallthrough path is
explicit and doesn't depend on memory-allocation coincidence.

**This is very likely a systemic pattern, not a one-off** — any `JZ`/`JEQ`
immediately followed by a `NOLINK` (intended to protect the *next* label
from incoming auto-link) has the same risk for its *own* fallthrough unless
it also gets an explicit redirect. This was **not** exhaustively searched
for elsewhere in the project this session — worth a project-wide grep for
`JZ ... \n NOLINK` / `JEQ ... \n NOLINK` patterns where the preceding
instruction's fallthrough isn't separately redirected.

### Diagnostic scripts (all in the project root, reusable)

`trace_voca_pc.py`, `check_ito_sizing.py`, `check_block_comments.py`,
`classify_spans.py` — from the earlier (pre-compaction) part of this
investigation; still valid. This session added: `check_infinite_loop.py`
(samples `SK_FIDX`/file-read-position at intervals and detects genuine
`op_id`-not-found halts vs. step-budget exhaustion — **use this whenever
something "hangs"**, per the project's own rule against blindly raising
`max_steps`), `trace_callstack_depth.py` (tracks `Voca`/`Redi` push/pop
balance over the whole run, flags the moment depth goes negative — this is
what found the open issue below), `trace_stack_contents.py`,
`trace_int_alloc_token.py`/`trace_int_ck_precise.py` (decode the token
being looked up at a specific `BS_INTERN`-family checkpoint), and several
narrower one-off scripts (`trace_sktmp_writes.py`, `trace_addr_writes2.py`,
`trace_cmd_addr.py`, `trace_full_path.py`, `trace_setref_ma.py`,
`trace_newref_el0.py`, `trace_bcall_crash.py`, `trace_tmp2_assign.py`,
`test_bs_token_value.py`, `test_bs_read_token_chunk.py`) kept for reference
but mostly superseded once their specific bug was found — the *technique*
(monkeypatch `Interpreter._run`, walk the real instruction stream, log on a
specific PC/condition) is the reusable part, not the specific script.

**Update (2026-06-20):** all narrow one-off scripts named above (and
~20 more from later in this same session, including `trace_voca_pc.py`)
were consolidated into 5 general-purpose, parameterized tools — see
`TOOLING.md`'s "Reusable diagnostic tools" section. `trace_callstack_depth.py`
is the one name that carried over unchanged (it was already general). The
specific narrow filenames in this paragraph no longer exist on disk; the
*findings* they describe are still accurate history.

### Open issue: call-stack imbalance around `RREDI`'s self-referential generic-macro definition

**Status: NOT FIXED. This is the current blocker, sitting at 33/42 (78%).**

**Symptom:** `IndexError: array index out of range` in `interpreter.py`'s
`_redi` (`aether[a1] = aether[sp]`) — `RA_SP` has been popped past its
starting value (started at `2097151`, observed reaching `2097159` — i.e.
more pops happened across the whole run than pushes, somewhere). Confirmed
via `trace_callstack_depth.py` (tracks every `Voca`-with-`Exit=RA_LINK` as
a push and `Redi`-with-`E1=RA_LINK` as a pop): a single `pc` address,
`RREDI_RET` (the final `Redi` inside `macros.re`'s own generic definition
of the `RREDI` macro, i.e. `macros.re:86`, `RREDI RREDI_RET`), fires **4
times in a row with no intervening pushes**, taking the tracked depth from
2 down to -1, which is what finally exhausts the stack.

**What's confirmed, precisely:**
- The crash happens while LOAD_MAIN processes `saku.re` (`SK_FIDX == 33`).
- `macros.re`'s `RREDI` macro definition (lines 75–86) ends with the
  shorthand `RREDI RREDI_RET` for its *own* return — i.e. it uses the very
  primitive it's defining, to define itself. The author already
  *explicitly* avoided this exact trap for two lower-level, closely-related
  mechanisms — see the comments at `macros.re:227` ("raw ITO Redi — this is
  the implementation of the autolink mechanism. All users of this mechanism
  use RREDI. The mechanism itself cannot.") and `macros.re:246` (same note
  for `AUTOLINK_RESET`) — both of those correctly use a raw `ITO X Redi
  El1=RA_LINK` instead of the `RREDI X` shorthand. `RREDI`'s own definition,
  and `WIRE_AUTOLINK_RESET`'s (`macros.re:58`, `RREDI WAR_RET`), do **not**
  follow that same already-established rule.
- **However:** directly verified that bypassing the *runtime text dispatch*
  entirely (calling `RREDI_SPN` straight via a hand-wired `Voca`, with
  `RA_MA0` set manually, skipping `LOAD_CMD_UNKNOWN`'s generic builder
  machinery completely) **still reproduces the exact same symptom** — 4
  firings at `RREDI_RET`, stack still underflows. This means the original
  hypothesis ("processing `RREDI RREDI_RET`'s *text* at runtime recursively
  re-invokes the `RREDI` macro, which ends the same way, recursing") is
  **not the full story** — something about `RREDI_SPN`'s *own execution*
  (or something it calls, e.g. `WIRE_AUTOLINK_RESET`/`AUTOLINK_RESET`) is
  unbalanced even when invoked directly, exactly once.
- A first attempted fix — adding `ASCII_R` to `LOAD_DISPATCH_LINE`'s
  first-byte `SWITCH` (mirroring the existing `N`/`S`/`L`/`I`/`B` fast paths)
  with a new `LOAD_CMD_R_GROUP` handler distinguishing `RVOCA`/`RREDI` by
  their second character and calling `RVOCA_SPN`/`RREDI_SPN` directly
  (reading both tokens into scratch *before* touching `RA_MA0`/`RA_MA1`, to
  avoid them being clobbered by a nested `BS_INTERN`→`ALLOC_TO` call) — was
  implemented, tested, and made things **measurably worse** (regressed from
  33/42 to 1/42, crashing almost immediately instead of at file 33). This
  was **reverted** (`saku.re`'s `SWITCH SK_TMP` table and the new
  `LOAD_CMD_R_GROUP` block were both removed again — confirmed back at
  33/42 baseline before writing this note). The regression strongly
  suggests RVOCA/RREDI's runtime dispatch has *more* subtlety than a simple
  "skip the generic mechanism" fix can safely address in one step — likely
  something about `LOAD_RKVA_RESOLVE`'s argument-reading machinery doing
  *something else* load-bearing besides just populating `RA_MA0`/`RA_MA1`
  (worth checking what else it touches before retrying this approach).
- `RA_LINK_REF` (used by `RREDI_SPN`'s own body to wire the *target*
  instruction's `e1` field) is confirmed correct — it holds `RA_LINK`'s own
  address (3029), and the resulting wired `RREDI_RET` instruction does have
  `e1=3029` exactly, so the automatic pop-on-Redi condition
  (`a1 == self._ra_link`) is satisfied correctly. This is **not** where the
  bug is.
- Stack contents at the 4 repeated firings show the values being popped are
  genuine, *different*, legitimate-looking return addresses from
  increasingly outer scopes (`LOAD_DL_RET` → `LOAD_FL_EOFCK` →
  `LOAD_MAIN_INC` → `0` → `0`) — i.e. the unwinding looks like it's
  genuinely walking back up through `LOAD_DISPATCH_LINE` → `LOAD_FILE` →
  `LOAD_MAIN`'s own loop frames, not spinning on one fixed bad value. This
  is consistent with a **cumulative** imbalance built up gradually over the
  whole 15-million-step run (one or a few extra, unbalanced pops/pushes
  somewhere earlier that only manifests once the stack finally runs out
  while genuinely unwinding), rather than one single, sharply localised
  bug at `RREDI_RET` itself.
- A red herring ruled out: `SK_FIDX` *does* reach 42 at one point during
  the run, but that's `LOAD_MAIN_P0` (the prepass loop) finishing
  correctly and handing off to the *second*, real loop (`LOAD_MAIN_W1:
  CLEAR SK_FIDX` correctly resets it to 0 right after) — directly traced
  and confirmed this is expected behaviour, not a clue toward "self-hosting
  secretly already completed".

**Hypotheses for the next session to check, roughly in order of how cheap
they are to test:**
1. Use `trace_callstack_depth.py`'s full-run history (or extend it to log
   *every* push/pop pair with enough context to diff) to find the **first**
   point in the entire 15M-step run where cumulative depth deviates from
   what a hand-traced expectation would predict — rather than only looking
   at the final crash. The current investigation only examined the last ~40
   events before the crash; the actual root push/pop mismatch is likely far
   earlier and unrelated to `saku.re`/file 33 specifically.
2. Check whether `WIRE_AUTOLINK`/`WIRE_AUTOLINK_RESET`'s manual
   save-into-`RA_MC_TMP_RL`-then-restore pattern (`macros.re:54`/`57`,
   `RREDI`'s own body at `macros.re:82`/`85`) is **redundant with**, rather
   than complementary to, the automatic stack push/pop — i.e. whether this
   manual mechanism was designed for an *earlier* interpreter that didn't
   yet have the automatic `RA_SP`-based call stack (see
   `DECISIONS.md`'s "Call stack: CS_PUSH/CS_POP" entry — automatic stack
   handling was added at some point; this manual `RA_MC_TMP_RL` dance might
   predate it and now be firing *alongside* the automatic mechanism,
   silently double-bookkeeping in a way that's usually harmless but can
   compound into a real imbalance under specific nesting patterns). If
   confirmed redundant, the fix may be to remove the manual
   save/restore entirely (trusting the automatic stack alone), rather than
   patching the `RREDI X` self-reference specifically.
3. As a narrower, more targeted alternative to hypothesis 2 and to the
   reverted broad `LOAD_CMD_R_GROUP` fix: apply the same minimal,
   already-proven-safe pattern the original author used for
   `AUTOLINK_RET`/`AUTORST_RET` (raw `ITO X Redi El1=RA_LINK` instead of
   `RREDI X` shorthand) to *just* the two other self-referential spots:
   `macros.re:86` (`RREDI`'s own `RREDI RREDI_RET`) and `macros.re:58`
   (`WIRE_AUTOLINK_RESET`'s `RREDI WAR_RET`). This was *designed* but not
   yet tried in isolation (the broader `LOAD_CMD_R_GROUP` dispatch fix was
   attempted instead, on the theory it fixed the same issue more
   completely — but given hypothesis 2's discovery that the bug reproduces
   even via direct `Voca` invocation bypassing runtime text dispatch
   entirely, this narrower fix may not be sufficient by itself either,
   *unless* the real issue is specifically the double-RREDI-via-shorthand
   inside `WIRE_AUTOLINK_RESET`'s own call into `AUTOLINK_RESET`, which a
   direct-`Voca`-bypass test wouldn't have ruled out since it still went
   through `RREDI_SPN`'s real body, which still calls `WIRE_AUTOLINK_RESET`
   the normal way).
4. Double check whether `RA_FRAME_SIZE` (the per-push stack slot count) is
   genuinely constant and correctly initialised for *every* call site, or
   whether some path pushes/pops with a different frame size than another
   (would also produce a depth-tracking-looks-fine-but-`RA_SP`-is-actually-
   wrong symptom).



### What happened
Several names in `.re` files were used as LINK/EXIT targets but had no corresponding
ITO definition. The lux stayed with op=0 → terminate on execution.

### Specific instances
- `LOAD_CU_SKIP` → correct: `LOAD_CU_SKPL`
- `LOAD_CB_SKL` → correct: `LOAD_CB_SKLS`
- `LOAD_RB_LOOP` → correct: `LOAD_RB_RL`
- Malformed: `LOAD_RB_LC_JMP2LOAD_RB_` → correct: `LOAD_RB_LC_JMP2RL`
- 1000+ comment lines without `/` prefix → read as commands

### Lesson
**A name in EXIT/LINK must exactly match the ITO definition.** A one-character typo
creates a phantom ITO that is invisible when reading the code but kills execution.
Static linter needed: for every EXIT addr, check that `aether[addr+1] != 0`.

### Diagnostics
```python
# Quick way to find phantom labels:
zero_ops = [addr for addr in ito_addrs if aether[addr+1] == 0]
```

---

## Pattern 2: Name Conflicts Between Files

### What happened
Identical names in different `.re` files → one lux overwrites the other on intern.

### Specific instances
- `AC_WRCUR` in `alloc.re:67` conflicted with `AC_WRCUR` in `arena.re:50`
  → arena.re renamed to `ACL_WRCUR`
- `SC_LOOP`, `SC_INC`, `SC_DONE`, `SC_RET` in `comments.re` conflicted
  with identical names in other files → renamed to `CMT_LOOP/CMT_INC/CMT_DONE/CMT_RET`

### Lesson
**Local labels must be prefixed with the file or module name.**
`LOAD_*` for saku.re, `CMT_*` for comments.re, `ACL_*` for arena.re, etc.
Global names (accessible from other files via LINK) — no prefix.

---

## Pattern 3: JZ Builder Writes to a System Lux Address

### What happened
JZ macro builder allocated an ITO (via ALLOC_LUCES) and received address 328.
By coincidence: `BS_FILE_COUNT` lives at address 328 → its word = 45 (file count)
was overwritten by the ITO value.

### Lesson
**System luces (BS_FILE_COUNT, K_CURSOR, etc.) live in the range ~80–400.**
The bump allocator can reach them under certain conditions. Protection needed:
- Guard in ALLOC_LUCES: if `new_bump < SYSTEM_RESERVED_TOP` → halt
- Or: system luces should live above the initial bump

### Fix
In `loader.py`: after LOAD_MAIN, restore `a[bs_file_count] = len(all_files)`.

---

## Pattern 4: SETREF Dispatch Bug (SET vs SETREF)

### What happened
In `saku.re`, LOAD_DISPATCH_LINE mapped 'S' + second_byte='E' → LOAD_CSS_SETCK.
LOAD_CSS_SETCK checked tlen==3 for SET, but if tlen!=3 — fell into LOAD_CMD_UNKNOWN.
For SETREF (tlen=6) this gave the correct path but the path was hidden.

Added an explicit NOLINK path: `LOAD_CSS_SEUCMD` for tlen != 3, which correctly
delegates to LOAD_CMD_UNKNOWN.

### Lesson
**Every branch in dispatch must explicitly handle all cases.**
The missing fallthrough path for "non-SET SE-tokens" created undefined behaviour.

---

## Pattern 5: K_CURSOR / K_WATERMARK Corruption

### What happened
LOAD_MAIN uses K_CURSOR to track the current bump allocator position.
Under certain conditions something overwrote K_CURSOR.word, setting aether[K_CURSOR]
to an incorrect value.

Symptom: after LOAD_MAIN, K_CURSOR.word ≠ expected → next ALLOC_LUCES returns
an address in already-occupied space.

### Temporary fix
In `loader.py` after LOAD_MAIN:
```python
a[k_cursor] = k_cursor + 1
a[k_cursor + 1] = loader._bump  # restore bump position
```

### Lesson
Root cause not found. This is a workaround, not a cure.

---

## Pattern 6: Write Protection as a Hack on Top of a Hack

### What happened
LOAD_MAIN wrote a RVOCA addr (764) into NEWREF.word, then 3000 (RA_LINK addr).
This broke dispatch: NEWREF.word = data lux → Voca → op=0 → terminate.

Attempted fix: add runtime write-protection in `loader.py`, intercepting all
`_write` operations and blocking "suspicious" writes to command luces.

### Why it did not work
Protection was either too aggressive (blocked legitimate writes) or too weak
(missed other corruption paths). Every tightening of protection opened a new
corruption vector.

**This is the "symptom → hack → new symptom" pattern.**

### Lesson
**Write protection in Python is diagnostics, not a fix.**
If LOAD_MAIN writes garbage into system luces — the bug is in LOAD_MAIN itself.
Find the source of the bad write in the Reca code and fix it there.

**Correct approach:** find which line in which file triggers a write via
LOAD_CMD_SET_IMPL with name=NEWREF, and understand why that line is processed
as SET instead of NEWREF.

---

## Pattern 7: Tracking Error — Wrong Phase Trigger

### What happened
During diagnostics, `oc[0] == 71` was used to detect when `saku.re` opened.
In reality, saku.re opens at `oc[0] == 72` (linux_generic.re = 71).
All tokens analysed as "from saku.re" were from linux_generic.re.

### Lesson
**Diagnostics must capture the file name via openat, not assume a counter.**
Always: on openat → decode path from Aether → `current_file[0] = fname`.

---

## Pattern 8: LOAD_MA_READARG and RA_LOAD_BYTE (open)

### What is known
LOAD_MA_READARG checks `RA_LOAD_BYTE == LF (10)` before reading MA1.
If LF — returns 0 without reading. Meaning: if the previous read left
RA_LOAD_BYTE = 10, MA1 will not be read.

**Normal:** after BS_READ_TOKEN for the MA0 token, RA_LOAD_BYTE = delimiter (space=32, not LF).
**In practice:** RA_LOAD_BYTE = LF before the MA1 check.

### Hypotheses (unverified)
1. Some intermediate Voca calls BS_READ_BYTE and reads LF
2. BS_READ_TOKEN for MA0 reads up to LF (not space) — tokeniser bug
3. RA_LOAD_BYTE is overwritten by something between MA0 and MA1 reads

### Next step
Add a tracker that prints on every LOAD_MA_PRELF trigger:
- Call stack (last 5 Voca/Redi)
- Current RA_LOAD_BYTE
- Current file position (via LOAD_RPOS or equivalent)
- Last read token (tokbuf + tlen)

---

## Root Cause Chain (hypothesis)

1. LOAD_MAIN processes saku.re
2. LOAD_MA_READARG for some NEWREF command returns MA1=0 (even though the ref argument exists)
3. NEWREF handler works only with MA0 (name) — ref is skipped
4. LOAD_DISPATCH_LINE reads the ref argument as the next command
5. That "command" (e.g. "LOAD_CMD_NEWREF") triggers LOAD_CMD_NEWREF handler
6. That handler reads the next line as its arguments — one line shift
7. Cascade: every NEWREF line is now processed incorrectly
8. Eventually NEWREF.word = garbage → Voca → op=0 → terminate

**Presumed root cause:** RA_LOAD_BYTE = LF at MA1 check in LOAD_MA_READARG.
**Source:** not established.

---

## What Was Fixed

[x] Phantom labels (~5 specific instances)
[x] 1000+ broken comment lines
[x] Name conflicts: AC_WRCUR, SC_LOOP/SC_INC/SC_DONE/SC_RET
[x] SETREF dispatch (LOAD_CSS_SETCKJ fallthrough)
[x] BS_FILE_COUNT corruption (JZ builder hit address 328)
[x] K_CURSOR/K_WATERMARK double-deref setup in loader.py
[x] Post-LOAD_MAIN restoration in loader.py (command handlers, system luces)

## What Remains Open

[ ] LOAD_MAIN pass2 incomplete — stalls at saku.re
[ ] Root cause of RA_LOAD_BYTE = LF at MA1 check
[ ] 93 phantom ITOs in files after saku.re (likely disappear once pass2 completes for all files)

---

## Recommendations

### 1. Clean before debugging
Before deep debugging — clean the architecture:
- Remove duplication (identical patterns in different places)
- Verify all comment lines start with `/`
- Add static lint: all EXIT/LINK addresses → `aether[addr+1] != 0`
Clean architecture makes bugs more visible.

### 2. Diagnose correctly
Rule: find the source of the problem via tracing first, then fix.
Do not add a fix until the exact cause is known.

For LOAD_MA_READARG:
```python
# On LOAD_MA_PRELF trigger:
# capture: RA_LOAD_BYTE, last_5_vocas, tokbuf_content, file_byte_offset
```

### 3. Python write-protection — remove or minimise
Current protection in loader.py is too complex and interacts badly.
If needed — only for K_CURSOR, K_WATERMARK, BS_FILE_COUNT (those that should
never change during LOAD_MAIN pass2). Everything else — remove.

### 4. Treat LOAD_MAIN as Reca code
LOAD_MAIN is a Reca program. Its bugs must be found in `.re` code, not in Python.
Every time the urge arises to add a Python hack — ask: "What in the Reca code makes this wrong?"

---

## Pattern N: FOR macro expansion bug (fixed) → root cause of 27 op=0 issues

**Status: FIXED.** Root cause found and fixed in `loader.py` (`_expand_indent`).

### What was broken

`FOR elem0 elem1 ... / body with {N} {X}` was not registered in `_INDENT_LEADERS`
and fell through to the generic `mode='prepend'` handler. That handler prepends
`ctx['name']` (= `parts[1]` of the leader line = the FIRST element of the FOR list)
to every body line:

```
FOR RA_MA0 RA_MA1 RA_MA2 ...
    CLEAR LOAD_MAS_CLR{N} {X}
```

became (via prepend with name='RA_MA0'):

```
CLEAR RA_MA0 LOAD_MAS_CLR{N} {X}
```

Python's `CLEAR name target` handler took `parts[1]='RA_MA0'` as the ITO **name**
and called `_alloc_ito('RA_MA0')`, allocating a fresh ITO at address 601114 and
overwriting `symbols['RA_MA0']`.

`macros.re` is compiled first in Wave-B (macro_first ordering). By that point
`RA_MA0` was still the correct data-lux at address 842. So all `_SPN` macros
(RCALL_AT_SPN, LH_N0_W0, LR_SPN, LT_SPN, WALK_ITO_SPN, RVOCA_SPN, CLEAR_SPN, ...)
baked in `e1=842`. Then saku.re's FOR/CLEAR ran and corrupted `symbols['RA_MA0']`
to 601114. `LOAD_MAS_0_SET: Move El1=LD_MA_RESULT Exit=RA_MA0` (saku.re) wrote
the resolved token address to 601114 — but the `_SPN` macros read from 842
(`aether[842]=0`), so `RA_MC_LUX` always became 0, and all Write operations
targeted offsets from address 0 instead of the actual target lux.

The same bad expansion affected bootstrap.re's `FOR RA_MA1..RA_MA7` blocks
(bootstrap.re was later split into lexer.re/intern.re — see ARCHITECTURE.md),
and parser.re / yaku.re FOR-based initialisation sequences.

### Why only 27 of the 170 issues were fixed

The other 143 remaining `op=0` issues are builder-class macros (`LH`, `LR`, `LT`,
`WALK_ITO`, `RCALL_AT`, `EMIT`, `EMITI`, `PUTBYTE`, etc.) whose Python Wave-B
handler does nothing (they're Reca macros, not in `_BOOTSTRAP_CMDS`). They are
supposed to be wired by `LOAD_MAIN` — but `LOAD_MAIN` stalls at step 1 (see
ROADMAP B6 and Pattern 8). The FOR fix is a prerequisite: now that `RA_MA0`
resolves correctly, the builder-macro dispatch chain is internally consistent and
will work correctly once `LOAD_MAIN` is unblocked.

Verified: manually invoking `LH_N0_W0` (LH's entry point) with MA0=LIM_BODY,
MA1=RA_I, MA2=RA_TMP now correctly wires `LIM_BODY.op=Move, e1=RA_I`. The
mechanism works end-to-end; only LOAD_MAIN activation is missing.

### The fix

`loader.py`, `_expand_indent`: added `mode='for'` handling. When `FOR` is
encountered as a leader line, a `'for'` context is pushed with `elements=parts[1:]`.
For each body line, N copies are emitted with `{N}` → index and `{X}` → element:

```python
if mode == 'for':
    elements = ctx['elements']
    for idx, elem in enumerate(elements):
        expanded = stripped.replace('{N}', str(idx)).replace('{X}', elem)
        result.append((lineno, expanded))
    continue
```

The FOR blocks in `saku.re`, `bootstrap.re`, `parser.re`, `yaku.re` are unchanged
(they were always syntactically correct — just never correctly expanded).

---

## Pattern N+1: PB_FLUSH reads from RA_OB_BASE but PUT_BYTE writes starting at RA_OB_BASE+1 (off-by-one, output is always wrong/zero for first lux)

**Status: FIXED.** Root cause was in PUT_BYTE: PB_ZERO_LUX advanced RA_OB_ADDR before writing, skipping OB_BASE.

Fix applied in output.re:
- Advance of RA_OB_ADDR moved to hot path (PB_ADV_LUX), triggered after writing 8 bytes (shift reaches 64).
- PB_ZERO_LUX now only clears the current lux and jumps to write — no advance.
- Invariant: RA_OB_ADDR always points at the CURRENT lux to write into.
- First PUT_BYTE writes into OB_BASE directly (RA_OB_ADDR initialized to OB_BASE at freeze time).

Also fixed: EMIT_PACKED_STR stride was +2 (DATA_LUX_MIN) but packed strings use stride=1 (consecutive luces, 8 bytes each). Fixed to +1, matching Python _pack_string and saku.re LOAD_PS_EMIT_BYTE.

Verified: `NEWSET STR "Hello, World!\n"` + EMIT_STR_ENTRY + FLUSH correctly outputs the string.

---

## Pattern O: LIMITER_ADOPTED scans all Lux (O(N) at exit) — FIXED

**Status: FIXED.** `K_LIMITER_ADOPTED` flag set at freeze time by loader.py.
`LIMITER_ADOPTED` now reads the flag in O(1) instead of calling `SCAN_ALL_LUX`.

`LIMITER_ADOPTED` (limiter.re) calls `SCAN_ALL_LUX` to determine whether
any Lux has `ACCORD_USE → LIMITER_ACCORD`. This is O(N) over all allocated
Lux. Called from `EXHALE_REPORT` (shioreru.re) once at exit.

For large programs (millions of Lux) this causes a perceptible pause at exit.

**Proper fix:** add a `K_LIMITER_ADOPTED` flag lux (or a small registry).
Set it to 1 when `LINK X ACCORD_USE LIMITER_ACCORD` is first executed.
`LIMITER_ADOPTED` then becomes O(1): just read the flag.

Requires: a hook in accord.re or saku.re when ACCORD_USE lumina are added.

---

## Pattern P: LINK command — different lumen storage model in Python vs Reca

**Status: NOT FIXED. Blocks self-hosting correctness.**

Python `_add_lumen` writes `(rel, exit)` lumen pairs directly into the source
lux at slots `1 + count*2` and `1 + count*2 + 1` (embedded model).

`LOAD_CMD_LINK_IMPL` in `saku.re` allocates a separate 2-luce block for each
`(rel, exit)` pair, then adds it to the source lux (standalone model).

These produce different memory layouts for the same LINK command.
When LOAD_MAIN runs, it will build lumina differently than Python loader,
which will break any code that reads lumina by offset (e.g. `SR_FIRST_LM`,
`SR_NEXT_LM`, `ADD_LUMEN`, `REMOVE_LUMEN`).

**Root cause:** Python `_alloc_data` pre-allocates lumen slots based on
`_lumen_prepass` count. Reca allocates each lumen pair on demand.

**Proper fix:** either align Python to allocate lumen pairs separately,
or align Reca to write lumina embedded in the source lux (pre-allocated).
The latter matches the current Python model and avoids fragmentation.

---

## Pattern Q: Yaku Voca/Redi templates missing call-stack operations

**Status: NOT FIXED. Affects compiled output correctness.**

The Yaku LLVM IR templates for `Voca` and `Redi` do not emit the automatic
call-stack push/pop that the interpreter performs via `RA_SP`.

`Voca` template: emits `store RA_LINK=NXT_N; br` — missing `RA_SP -= FRAME_SIZE;
aether[RA_SP] = old_RA_LINK` (push).

`Redi` template: emits `load RA_LINK; br` — missing `RA_SP += FRAME_SIZE;
RA_LINK = aether[RA_SP]` (pop).

**Impact:** compiled functions with nested calls will corrupt the call stack,
causing incorrect returns for any call depth > 1.

**Fix needed:** add `RA_SP_ID` and `RA_FRAME_SIZE_ID` escape bytes to the
template substitution system, then update Voca/Redi templates to emit
the stack push/pop using inline GEP reads/writes.

See: `compile/yaku.re` LATENT ISSUE comment lines 98-105.

## Pattern R: Python's Wave-B only natively expands ~16 primitive commands —
## everything else is unbuilt content until LOAD_MAIN runs it

**Status: confirmed, not a bug per se — an architectural limit of the
bootstrap scaffold, worth understanding precisely.**

`loader.py`'s `_wave2_line` (Python's "source of truth" content-writing
pass) only has native handling for: `NEW, NEWSET, NOLINK, SET, NEWREF,
SETREF, LINK, ITO, BLOCK, RVOCA, RREDI, CLEAR, NOP, JEQ, JZ, ALLOC_TO`.
`SWITCH/FOR/SAVE/NOITO/CHAIN` are handled too, but earlier — as a separate
*text*-preprocessing pass that expands them into the primitives above
before `_wave2_line` ever sees them.

Everything else — `LR, LT, RCALL_AT, EMIT, EMITI, PUTBYTE, WALK_ONE,
LINK_OP, WALK_ITO, UNLINK_OP, LX, LH`, and any future custom macro written
the same way — is **not** expanded by Python at all. These are only ever
expanded by LOAD_MAIN itself, at runtime, via generic command-word
dispatch (the command's own symbol value points at its `_SPN` handler;
see the "Restore command handler words" block in `loader.py`'s `freeze()`).

**Consequence:** any name defined via one of these macros has *no content*
in Python's "source of truth" build — not wrong content, literally
unwritten (the lux exists, sized correctly since the `_scan_ito_names`
prepass fix below, but its word/op/e1/e2/exit fields are whatever the bump
allocator's zero-init left them) — *unless* LOAD_MAIN's own pass actually
reaches and processes that line. Since LOAD_MAIN currently stalls at file
33/42 (saku.re), nothing in any file alphabetically after it — including
all of `yaku.re` — gets this content filled in by either build path right
now. This is the precise, complete reason yaku.re's own top-of-file note
says "the compiler entry `P0_NID` produces 0 bytes of output": it is not
only that `P0_NID` is never invoked, but that the bulk of the Yaku
compiler's own internals (most of it built via `EMIT/EMITI/PUTBYTE/
RCALL_AT/LR/LT/WALK_ONE`) is itself unbuilt content right now, regardless
of invocation.

Discovered while investigating why naming `RTB_LUX_BODY` directly via `LR`
(instead of a separate `NOP RTB_LUX_BODY` placeholder) produced a
non-self-referential (unwritten) entry even after fixing the *separate*,
real `_scan_ito_names` sizing gap below. Confirmed by direct regression
test, twice (with and without the sizing fix) — see `yaku.re`'s
`RTB_LUX_BODY` comment for the full trail.

**Not a bug to fix now** — reimplementing every custom macro's expansion
in Python would defeat the purpose of self-hosting (Python is meant to
shrink toward minimal scaffolding, not grow to match LOAD_MAIN's
capabilities 1:1). The actual fix is self-hosting progress itself: once
LOAD_MAIN reaches and correctly processes `yaku.re`, this stops mattering
for that file. Until then, any place that needs a *guaranteed-correct*
entry point in `yaku.re` for a name with nothing left to manually save
should use one of the ~16 natively-Python-expanded primitives (most often
`NOP name`) rather than a higher-level macro like `LR`/`EMIT`/etc, even
when the higher-level macro would otherwise be the more "direct" choice
with zero extra instructions.

**Addendum (2026-06-20): quantified via `diag.py --lint`.** Ran the lint
check (PHANTOM ITOs / DEAD REFERENCES / self-ref violations) after this
session's other changes and found **144 phantom ITOs (op=0) and 19 dead
references**, none of them caused by anything changed this session
(verified: the 7 ALLOC_TO conversions made this session — see Pattern T
addendum below — do not appear anywhere in the phantom list, since
`ALLOC_TO` *is* one of the natively-Python-expanded primitives, unlike
the macros responsible for the phantoms).

Spot-checked a sample of the phantom names against their source: every
one checked is built via a macro in the Pattern R "not natively expanded"
category — `LINK_OP`/`UNLINK_OP`/`WALK_ONE` in `ring.re` (`RI_LINK`,
`RING_PUSH`, `RP_GR_UNL`, `RP_GR_LK1`, `RP_GR_LK2`), `EMIT`/`RCALL_AT`-
family macros in `yaku.re` (`EM_EPRELEQ_J`, the `EB_*` family, `PA_ISNEW_J`,
the `PGB_*` family), `LR`/`LT` in other files (`RTB_FTK_J`), and similar.
This confirms Pattern R's scope empirically: it isn't a one-off
(`RTB_LUX_BODY`) or limited to `yaku.re` specifically — it's systemic,
affecting *any* file using these macros that LOAD_MAIN hasn't reached yet,
which today is most of the project past file 33/42. The 19 "dead
references" are the `JumpIf`/`Jump` targets pointing *at* these phantom
ITOs — i.e. real control-flow edges that currently land on unbuilt
content. Not actionable beyond what Pattern R already says (self-hosting
progress is the real fix); recorded here as the quantified baseline for
comparison once that progress happens.

## Pattern S: `_scan_ito_names`'s ITO-target sizing prepass was a
## hand-maintained list — now derived automatically from macros.re

**Status: FIXED.**

`_scan_ito_names` (the prepass that forces full `ITO_SIZE=7` allocation on
a name's *first* mention, so a later RVOCA/RREDI/ITO/etc. definition
doesn't silently get stuck with whatever smaller layout an earlier
reference picked) used to recognize only a hand-maintained list of command
keywords: `RVOCA, RREDI, NOP, ITO, JZ, CLEAR, ALLOC_TO, JEQ`. This silently
missed every other macro following the exact same "first argument becomes
a self-referential ITO" convention: `LX, LH, WALK_ONE, LINK_OP, LR, LT,
WALK_ITO, UNLINK_OP, RCALL_AT, EMIT, EMITI, PUTBYTE` — 12 more, found by
checking which `NEWREF X X_ENTRY` macros in `macros.re` have an `ITO`
instruction somewhere in their body doing `Move El1=RA_MA0 ...
Exit=RA_MC_LUX` (the universal signature for this convention).

This is what broke `RTB_LUX_BODY` (named via `LR`, referenced earlier in
`yaku.re` as a plain register before its own `LR`-based definition) —
`RTB_LUX_BODY` got pinned to a small, non-ITO allocation by the earlier
reference, and `_define`'s "already exists" guard silently kept it that
size when `LR`'s definition was reached.

**Fix:** `loader.py`'s `_discover_ito_naming_commands` now scans
`macros.re` itself once (cached) for this exact pattern, building the
recognized-command set automatically instead of by hand. Any future macro
written the same way is covered with zero changes to this prepass. `ITO`
itself stays a separate hardcoded case (it's the base primitive, not a
`NEWREF`-defined macro). `ALLOC_TO` keeps its own extra `_J`/`_K` suffix
handling layered on top.

This fix alone does *not* make `LR`-style macros safe to use directly for
naming a `yaku.re` entry point right now — see Pattern R above for the
separate, deeper reason `NOP` is still the correct choice there.

## Pattern T: Deferred items from the 2026-06-20 cleanup session

**Status: catalogued, none fixed yet — for a future session.**

These were found in passing during the cleanup-before-bugfixing pass
(Patterns Q/R/S above) but deliberately not acted on, either because the
fix is genuinely harder than it looks or because the value right now is
low (code not yet exercised by self-hosting). Listed here so none of them
get silently lost.

1. **`SAVE_EMIT_SAVES`/`SAVE_EMIT_RESTORES` (saku.re) don't reject RA_LINK
   the way the Python-side `SAVE` preprocessor does.** Not currently buggy
   (each call gets a fresh `ALLOC_TO`'d slot, no sharing risk like the old
   `S_RA_LINK` bug), and no current `.re` file asks it to save RA_LINK
   (all such call sites were fixed at the source this session). But it's
   asymmetric with the Python side's unconditional rejection. The reason
   it's not a quick fix: `SAVE_EMIT_SAVES`/`RESTORES` are a 4-way *unrolled
   shift* (`RA_MA1` checked and processed, then `RA_MA2 → RA_MA1` shifts
   for the next check, etc.) — naively "stop at RA_LINK" would incorrectly
   skip every register *after* it in the list (e.g. `SAVE X RA_SCAN_BODY
   RA_LINK RA_SCAN_STOP` would lose `RA_SCAN_STOP`). A correct fix needs a
   filter-and-reshuffle at each of the 4 unrolled positions, not a simple
   early-exit. Low priority: this code path isn't exercised yet anyway
   (Pattern R — LOAD_MAIN hasn't reached a file that would invoke it).

2. **`_wave1_line`'s RVOCA/RREDI-specific "re-allocate if it was a data
   stub" check has no equivalent in the generic unknown-command fallback**
   (used by LR/EMIT/etc.). Investigated and empirically verified clean —
   `check_ito_sizing.py` reports 0 violations across all 3784 names
   needing full ITO_SIZE, confirming the global `_ito_names` prepass
   (Pattern S) makes this distinction moot: by the time *any* command
   first references a name, `_alloc_data` already checked `_ito_names`
   and sized it correctly, so there's nothing left for a stub-reallocation
   safety net to catch. No action needed unless a future regression in
   the prepass reintroduces a gap here.

3. **The exact root cause of *why* LOAD_MAIN stalls at file 33/42 in the
   first place, beyond the specific `LOAD_RKVA_J{N}` fall-through bug
   already being chased** — not pursued as a separate question. Decided
   (with the author) that this is a meta-question with lower expected
   value than following the concrete, already-confirmed empirical lead
   (`SK_POS_SLOT` reaching 8) to wherever it leads; if another root cause
   exists behind it, the same empirical method that found this one will
   find the next one.

4. Possible future targets for the *same* "auto-discover from macros.re
   instead of a hand-maintained list" treatment, not yet audited:
   `test_macros.py`, `repl.py`, `interpreter.py`'s dispatch tables, and
   any other script that imports specific command names from `loader.py`
   rather than discovering them. Not checked this session — only
   `check_ito_sizing.py`, `classify_spans.py`, and `diag.py`'s
   `mode_macros()` were found and audited.

5. **Macro-usage audit, round 2 (raw ITO duplicating an existing macro).**
   Searched for the manual "`RVOCA name ALLOC_LUX/ALLOC_LUCES` + `ITO
   name2 Move El1=RA_ALLOC_RESULT Exit=dest`" pattern (duplicates
   `ALLOC_TO name dest count`) project-wide. Found ~15 candidates;
   converted the 7 that were a *clean* match (simple constant count,
   single unconditional save, nothing else in between):
   `PS_NW_ALLOC`/`PS_BLK_ALLOC1`/`PS_MOF_NEW`/`PS_DEF_STR` (parser.re),
   `RING_INIT`/`RP_GR_NEW` (ring.re), `LOAD_CNI_ALLOC` (saku.re). The
   other ~8 (e.g. `PS_ST_FW_SAVEFIRST`, `BPT_ALLOC` in lexer.re,
   `LOAD_CN_SAV`/`LOAD_CI_SAVEADDR` in saku.re) were *not* converted —
   each has either a multi-step computed count, a `Write` into the
   allocated lux before the save, a conditional (not unconditional) save,
   or multiple destinations needing the result — genuinely distinct logic
   `ALLOC_TO`'s "allocate + always save to one place" shape doesn't cover,
   not a missed macro opportunity. Verified via `freeze()`, `diag.py
   --lint` (no new phantoms from the conversions), and
   `trace_callstack_depth.py`.

   Only `ALLOC_TO`'s signature was checked this round. Still unaudited
   for the same kind of manual duplication: `WRITE_ITO_SLOTS`, `LR`/`LT`,
   `EMIT`/`EMITI`/`PUTBYTE`, `WALK_ONE`/`WALK_ITO`, `LINK_OP`/`UNLINK_OP`,
   and — per the author's specific request — `FOR`/`SWITCH` (the two
   considered most universal/foundational). To do next.

---

## Pattern P: UPDATE (2026-06-23) — Actually Closed

**Status: CLOSED. The "different model" was a misread of old docs.**

Verified in session 2026-06-23: Python `_add_lumen` and Reca `ADD_LUMEN` both
use the embedded model. `alloc.re` line 11 explicitly states "There are NO
separate lumen blocks. Lumina are inline in the lux." `_alloc_data` sizes
correctly via prepass (`1 + 2*N + 1`). The layouts match. Pattern P is not a
bug. BUGS.md status above is stale.

---

## Pattern U: NEWSET integer values wrong in runtime LOAD_MAIN (TAB=18, LF=20)

**Status: PARTIALLY FIXED. Root cause found, full fix deferred.**

### Symptom
After freeze, `aether[TAB]=18` (should be 9), `aether[LF]=20` (should be 10).
Only TAB(9) and LF(10) doubled; CR(13)/ESC(27)/SP(32) correct. Breaks runtime
PS_MAIN parser (LF never matches newline → lines never split).

### Root cause (fully traced)
`NEWSET TAB 9` goes through the generic macro path → `LOAD_READARG_KV` resolver
→ `LOAD_RKVR_INT` → computes `addr(C_0) + N*2` for the value. Before the
session fix, `RA_C0_REF` bug meant this gave raw `N*2 = 18` instead of
`addr(C_0)+18 = addr(C_9) = 390`. Then `NEWSET_START Write El2=RA_MA1` wrote
`aether[MA1] = 18` (not `aether[aether[MA1]] = aether[390] = 9`).

Two sub-bugs found and partially fixed:
1. **RA_BS_TMP2 register collision** (FIXED): BS_PARSE_INT wrote result to
   `RA_BS_TMP2`, which is also scratch in BS_READ_TOKEN hash loops. Introduced
   dedicated `RA_BS_PIVAL` register in `lexer.re`. All 5 call sites updated
   (4 in saku.re, 1 in macros.re). This is safe and kept.
2. **RA_C0_REF vs C_0 in resolver** (FIXED): `LOAD_RKVR_CN Add El1=C_0 ...`
   used C_0's VALUE (=0) instead of C_0's ADDRESS. Changed to `RA_C0_REF`.
   This is safe and kept. Now resolver gives correct `addr(C_0)+N*2`.

### Remaining problem (DEFERRED)
Even with fix 2, resolver gives `addr(C_9)=390` in `MA1`. But `NEWSET_START`
does `Write El2=RA_MA1` which stores `aether[MA1]=390` (the address), not 9
(the value). Needs a `Read` dereference. But adding Read breaks large values
(e.g. `NEWSET SK_BODY_BUF_SIZE 4096`): `addr(C_0)+4096*2` points to garbage
since C_N luces are SPARSE (only C_0..C_19 contiguous, then C_32, C_48...).

### Full fix requires (DEFERRED to next session)
Unify the integer resolver contract: instead of `addr(C_0)+N*2` formula, look
up or lazily allocate a lux holding the literal value (matching Python
`_resolve`). This requires synchronous Python-side change too (Python pre-alloc
must reserve those luces). Estimated scope: saku.re `LOAD_RKVR_INT` path +
`loader.py _resolve` call-site audit. Tracked in ROADMAP.md.

### Impact
Only affects runtime PS_MAIN parser (not the 42/42 LOAD_MAIN self-hosting
path). LOAD_MAIN processes ascii.re via Python pass1a (which writes correct
values). The 42/42 self-hosting score is unaffected.

---

## Pattern V: PS_MAIN runtime parser reads 0 lines from file

**Status: ROOT CAUSE FOUND, fix blocked on Pattern U.**

### Symptom
`PS_MAIN` (the pure-Reca runtime parser in `parser.re`) completes without
crash but processes 0 meaningful lines. A clean test file produces only 2
loop iterations, both with empty/wrong content.

### Root cause
`LF` constant = 20 instead of 10 (Pattern U). The line-split check in `PS_RL`
(`JEQ PS_RL_NL PR_BYTE LF PS_RL_DONE`) never fires because `PR_BYTE` holds
the actual byte 10 (LF) but `LF` symbol holds 20. Lines are never terminated,
everything runs together as one "line."

### Fix
Fix Pattern U → PS_MAIN will work correctly.

### Note
PS_MAIN is NOT part of the 42/42 self-hosting path (0 references in
loader.py). It is the NEXT frontier after self-hosting: pure-Reca loading of
.re files at runtime without Python bootstrap.

---

### O. PS_MAIN dispatch: NOLINK and YAKU_NEXO use wrong handlers — OPEN

**Status:** Confirmed bug. Not causing 42/42 failure today because NOLINK in PS_MAIN
does nothing visibly harmful (RA_MC_PREV unused in PS_MAIN context), but semantically wrong.

**Root cause:** macros.re loads before parser.re. macros.re defines:
- `NEWREF NOLINK NOLINK_START` → NOLINK.word = NOLINK_START (clears RA_MC_PREV, for LOAD_MAIN)
- `NEWREF YAKU_NEXO YAKU_NEXO_START` → YAKU_NEXO.word = YAKU_NEXO_START (Reca compiler macro)

parser.re tries to override these for PS_MAIN dispatch:
- `NEWREF NOLINK PS_LINE_NOLINK` → no-op (already defined)
- `NEWREF YAKU_NEXO PS_LINE_SAKU` → no-op (already defined)

**Effect:**
- PS_MAIN on "NOLINK" token → jumps to NOLINK_START → clears RA_MC_PREV (wrong register; PS_MAIN uses PR_LAST_INSTR for this purpose)
- PS_MAIN on "YAKU_NEXO" token → jumps to YAKU_NEXO_START (Reca compiler, wrong context entirely)

**Correct fix options:**
1. Use `SET NOLINK PS_LINE_NOLINK` in parser.re (SET overwrites word at runtime, after load order)
2. Give PS_MAIN dispatch table its own separate htable, not sharing intern's BS_HT_BASE
3. Use different symbol names for PS_MAIN handlers (PS_CMD_NOLINK etc.) and build a separate dispatch table

**Note:** This is a low-priority bug for now because PS_MAIN is not on the critical path
(42/42 uses LOAD_MAIN via Python bootstrap). Becomes important when PS_MAIN is used for
hot-loading at runtime.
