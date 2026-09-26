# TOOLING.md — Reca Developer Tools

All tools are Python scripts in the project root. Run with `python3`.
Infrastructure files (loader.py, interpreter.py, symphony.py) are not run directly.

`iris.py` is the hub for every static/build check and diagnostic report.
`trace.py` is the separate, standalone tracer for live execution investigation
(it does not go through the hub — a trace owns its own patched run and can't
share a build the way hub checks do). A handful of other standalone tools
(`long_run.py`, `fast_profile.py`, `gen_compiler.py`, `repl.py`,
`check_graph_shape.py`, `check_overlap.py`) are not part of the hub either.

---

## Quick reference

| Task | Command |
|------|---------|
| Build / self-host | `python3 -c "from loader import freeze; freeze()"` |
| Every check, one shared build | `python3 tools/iris.py --checks` |
| Static lint | `python3 tools/iris.py --lint` |
| Invariant check (run after every change) | `python3 tools/iris.py --invariants` |
| List every hub command | `python3 tools/iris.py --list` |
| Remove build artifacts (reca.bin/.sym, `__pycache__`) | `python3 tools/iris.py --clean` |
| Find who wrote a bad value | `python3 tools/trace.py --find-writer SYM` |
| Run all macro tests | `python3 tools/test_macros.py` (or `iris.py --macro-tests`) |
| Trace a halt | `python3 tools/trace.py --steps 5000000` |
| Watch what writes to X | `python3 tools/trace.py --write-watch X` |
| Watch file-index transitions | `python3 tools/trace.py --fidx` |
| Run PS_MAIN on a file | `python3 tools/trace.py --psmain example.re` |
| Check comment spans | `python3 tools/check_comments.py` (or `iris.py --comments`) |
| Inspect one file's comment spans, unfiltered | `python3 tools/check_comments.py --spans FILE.re` |
| Structural graph snapshot / diff | `python3 tools/check_graph_shape.py save out.json` |
| Check allocated bodies for overlap | `python3 tools/check_overlap.py` |
| Resumable, checkpointed multi-billion-step self-hosted runs | `python3 tools/long_run.py --steps N` |
| Fast hotspot profiler for long runs (finds what's actually slow) | `python3 tools/fast_profile.py --steps N` |
| Regenerate compiler files | `python3 gen_compiler.py` |
| Run a program | `python3 repl.py --load example.re` |

---

## iris.py — the diagnostic hub

> "iris is the hub. It owns nothing but the contract: what a command is,
> how a command gets the build it needs, and how the aggregate `--checks`
> run shares one build across everything. The actual commands live in
> theme modules (`iris_source`, `iris_graph`, `iris_trace`) — iris only
> dispatches." — iris.py's own docstring

```bash
python3 tools/iris.py                     # default: health overview + lint
python3 tools/iris.py --checks            # every check, one shared build
python3 tools/iris.py --<command> [args]  # run one command
python3 tools/iris.py --list              # list every registered command
python3 tools/iris.py --clean             # wipe reca.bin/.sym + __pycache__
```

### Why a hub

A command declares one of four **NEEDS** levels and nothing else:

```
text  ⊂  fresh  ⊂  freeze          (trace is off this ladder — never shared)
```

- `text` — reads `.re` source only, no build.
- `fresh` — `load_or_freeze()`: thaws `reca.bin` (rebuilds only if a `.re`
  source is newer), gives symbol table + aether, no Wave-1 bookkeeping.
- `freeze` — full `freeze()`: every wave, all bookkeeping. Satisfies
  `fresh` and `text` too.
- `trace` — needs its own patched run; can never share a build with
  anything else.

`--checks` groups every check by NEEDS, builds the single strongest
resource any of them requires **once**, and runs every text/fresh/freeze
check against that one build — this is the entire reason the old
`run_all_checks.py` existed; the same logic now lives here, generalised
to every command instead of five hand-picked ones.

### Commands

Reported by `--list`, tag = check (pass/fail, exit 0/1) or report (prints,
always exits 0):

| Flag | Needs | Tag | What it does |
|------|-------|-----|---------------|
| `--lint [--all]` | freeze | check | phantom NOVAs (op=0), dead EXIT/NEXT refs, self-ref violations, bounds violations. Pattern-R phantoms suppressed by default; `--all` shows them too. Absorbed the old `graph` mode. |
| `--invariants` | fresh | check | eleven families of build invariant (FAILs only) — ASCII constants, `C_N` addresses, key NOVA wiring, critical-symbol presence, macro registration, self-hosting progress |
| `--health` | freeze | report | freeze health overview: counts, criticals, registers |
| `--broken` | freeze | check | broken/unset infrastructure luces |
| `--stubs` | freeze | report | lazily-created names nothing ever filled in |
| `--save-integrity` | freeze | check | SAVE/OWN/LOCK combo-block structural guarantee holds |
| `--nova-sizing` | freeze | check | every NOVA-target name got a full `NOVA_SIZE` lux, not a small data lux |
| `--op-identity` | fresh | check | compiled op matches each name's own source declaration (catches silent name-collision allocation loss) |
| `--deref-mismatch` | fresh | check | operand needs an address but got a dereferenced word (the `_REF`/Idem bug class) |
| `--graph-equivalence [file]` | fresh | check | self-hosted `SK_HTAB` graph agrees with Python's, by name — not just "did it finish running" |
| `--duplicate-labels` | text | check | any label defined more than once |
| `--unreachable-entry` | text | check | never-reached guard positioned before a real entry point |
| `--dropped` | text | report | every source line the loader silently parses to nothing (unrecognised command word ≈ prose, dropped without error — generalises the old label-order check) |
| `--comments` / `--lost` | text | check / report | `--comments`: real command lines swallowed by a broken comment block (definite bugs). `--lost`: same scan, listing form |
| `--spans FILE` | text | report | unfiltered comment-span + swallowed-line listing for one file, no threshold gating — the surviving use case of the old `sim.py --lexer`/`--prepass` file inspection, built on the same `_strip_comments` ground truth instead of a separate reimplementation |
| `--deps` | text | report | per-file symbol counts and cross-file dependencies |
| `--indent` | text | report | every line synthesised by the indent mechanism |
| `--parity FILE` | text | report | dump NOVA luces one target file contributes |
| `--macros` | fresh | report | macro registry vs actual line-starting usage |
| `--block-lint FILE` | text | report | indent/style outliers in CHAIN/SWITCH/FOR/NOVAE bodies |
| `--unwrapped FILE` | text | report | non-ASCII lines, flagged when inside a block body |
| `--lookup-sites` | text | report | list `BS_LOOKUP` sites for manual forward-ref-backfill review |
| `--strings` | fresh | report | string encoding layout for `SF_*` names |
| `--wiring SYM [SYM2...]` | fresh | report | dump full lux wiring for named symbols |
| `--htable NAME` | fresh | report | look up a name in the self-hosted runtime htable |
| `--htable-reentrancy NAME [--fi N] [--max-hits N] [--mode ...]` | trace | report | trace one name's interning for mid-call corruption |
| `--fallthrough-hijack` | fresh | report | candidates for fallthrough landing on an unrelated JZ/JEQ's own step (structural match, not confirmed) |
| `--autolink [args]` | trace | check | AutoNext adjacency + `BS_LAST_NOVA` capture invariants, verified against a real traced run (see below) |
| `--load-main [args]` | trace | report | run the self-hosted LOAD_MAIN and report stats |
| `--macro-tests [name]` | trace | check | macro equivalence tests, Python vs self-hosted (same as `test_macros.py`) |

### `--invariants`

Runs a battery of sanity checks against the frozen Aether. Reports FAIL
(definite bugs) vs WARN (suspicious, may be intentional). Run this
**after every significant change**, before chasing a bug by hand — a
wrong ASCII constant or broken NOVA wiring shows up here immediately
instead of after 20 minutes of manual tracing.

### `--lint`

The most-used check. Expected output:
`✓ Lint clean  (N Pattern-R phantoms/dead-refs suppressed as expected)`.
The `(N suppressed)` number grows only when new Pattern-R macros are
added — if it changes unexpectedly, something is wrong. `--all` shows
every suppressed entry too, so "expected" is inspectable rather than
taken on faith.

### `--autolink` — traced (not static) autolink invariant check

Unlike the text/fresh/freeze checks above (which inspect a finished
build or scan source), this one runs a real, *traced* `freeze()` and
watches every write to `RA_MC_PREV`/`BS_LAST_NOVA` as the self-hosted
`LOAD_MAIN` actually executes them:

1. **AutoNext adjacency** — every `RA_MC_PREV` transition from a
   nonzero `P` to a new `N` must leave `aether[P + SLOT_NEXT]` as
   either `0` (physically adjacent, fall-through) or `N` (explicit
   link). Anything else means `AUTOLINK`/`LINK_NEXT` linked the wrong
   lux, or skipped one it shouldn't have.
2. **`BS_LAST_NOVA` capture-on-`NOLINK`** — every write to
   `BS_LAST_NOVA` must be immediately followed by `RA_MC_PREV` being
   cleared to `0` — the structural signature of `NOLINK`'s own body.

Run after touching `AUTOLINK`, `LINK_NEXT`, `NOLINK`, or anything else
that writes `RA_MC_PREV`/`BS_LAST_NOVA` directly.

### `--graph-equivalence`

Answers what `--nova-sizing`/`--op-identity` can't: does the self-hosted
`LOAD_MAIN` build a graph that actually *agrees* with Python's real one,
not just "did it finish running" (the `Self-hosting: N/42` progress bar
only measures the latter). Works by replicating `HT_LOOKUP` in Python
(same djb2 hash, same linear probe, same full-name-from-pool
verification) to query `SK_HT_BASE` directly for every name Python
declared, then compares Op/El1/El2/Exit by *symbolic name* — the two
builds use different address ranges, so raw-address comparison can't
work. Needs a build where the self-hosted pass has actually run and
gotten somewhere; a run that halts very early reports almost everything
as "not found," which reflects incomplete progress, not new violations.

---

## trace.py — Universal execution tracer

The primary debugging tool. Runs freeze() (or uses reca.bin), instruments
the interpreter, and reports on the run. Not part of the iris hub — a
trace always owns its own patched run.

```bash
# Basic: run to halt, show crash context (last 30 PCs, registers)
python3 tools/trace.py

# From a specific lux
python3 tools/trace.py --from LOAD_MAIN_LOOP

# Cap execution
python3 tools/trace.py --steps 5000000

# Stop at symbol
python3 tools/trace.py --until BS_DONE

# Print every step (very verbose)
python3 tools/trace.py --verbose

# Watch a register change (with optional step window)
python3 tools/trace.py --watch RA_MC_PREV
python3 tools/trace.py --watch RA_MC_PREV --watch SK_FIDX

# Profile: top-N hottest PCs
python3 tools/trace.py --profile

# Stop on first unknown-op (dispatch failure) or fault vector
python3 tools/trace.py --fault

# Show call stack (Voca/Redi pairs) on every call
python3 tools/trace.py --stack

# Run compiler (Yaku), show EP_BUF state after
python3 tools/trace.py --compile

# LOAD_MAIN trace: show file+line+token for each dispatch
python3 tools/trace.py --loadmain
```

### Newer flags

```bash
# Print old→new every time SYM's address is written (Move or Write)
python3 tools/trace.py --write-watch TAB --write-watch LF

# Dump tokbuf+TLEN every time PC reaches SYM
python3 tools/trace.py --token-at BS_PARSE_INT --context 20

# Run PS_MAIN against FILE.re (uses cached reca.bin, no full freeze)
python3 tools/trace.py --psmain example.re

# Print SK_FIDX file-index transitions during LOAD_MAIN
python3 tools/trace.py --fidx

# History depth for --write-watch / --token-at (default 30)
python3 tools/trace.py --write-watch X --context 50

# Step-range filters (apply to --write-watch and --token-at)
python3 tools/trace.py --write-watch X --start-step 1000000 --stop-step 5000000
python3 tools/trace.py --token-at X --after-step 2000000

# Print SK_FIDX every N steps (progress during long runs)
python3 tools/trace.py --steps 50000000 --progress-every 5000000

# Detect repeating PC-state cycles (infinite loops)
python3 tools/trace.py --loop-detect

# Print lux wiring for named symbols after freeze
python3 tools/trace.py --dump LOAD_CMD_NEW --dump JEQ_N1

# Print call depth (Voca=push, Redi=pop) on every call
python3 tools/trace.py --stack-depth

# Halt entire trace at step N
python3 tools/trace.py --stop-step 3000000

# Find every write to SYM's aether address (who wrote it, when, with what
# instruction history). THE tool for "register has a wrong value, who set it?"
# instead of guessing with chains of write-watch/token-at scripts.
python3 tools/trace.py --find-writer LF
python3 tools/trace.py --find-writer LF --find-writer TAB --context 12

# Watch specific PCs being reached (does this line get hit, how many times)
python3 tools/trace.py --watch-pc LABEL_NAME_ONE LABEL_NAME_TWO
```

### --find-writer SYM — find who wrote a bad value

The fastest path from "register X has value V, who set it?" to an answer.
Wraps `make_patched_run(on_write_dest=...)` (see below) so you don't have to
hand-write a `patched_run` each time.

```bash
python3 tools/trace.py --find-writer LF
# prints every Move/Add/Sub/Mul/Write that targets LF's aether address:
#   fi=5 step=192028 op=Write value=10  history=[...8 PCs of context...]
# ... [find-writer] Summary: LF: 1 writes
```

Use this **first**, before reaching for write-watch/token-at chains or manual
`patched_run` scripts. If the answer isn't a single write (e.g. the value
comes from arithmetic on two registers), the printed instruction history
(`BS_RB_LOAD`, `BS_RB_INC`, ...) tells you exactly which subroutine to look
at next.

### trace_lib equivalent — make_patched_run()

For anything `--find-writer`/`--write-watch`/`--token-at` don't cover,
`trace.make_patched_run()` (in trace.py) is the reusable building block —
don't hand-roll a fresh `patched_run` per investigation. It takes callback
dicts keyed by symbol name and handles all the dispatch-table patching,
`fi` tracking, and PC history bookkeeping:

```python
from trace import make_patched_run

events = []
make_patched_run(
    on_write_dest={'RA_LOAD_BYTE': lambda ev: events.append(ev)},
    on_write_addr={2748929: lambda ev: events.append(ev)},  # by raw aether
                                                              # address -- for
                                                              # self-hosted-only
                                                              # addresses with
                                                              # no Python symbol
    on_voca_target={'SWITCH_START': lambda ev: print(ev)},
    on_equal_nxt={'SWITCH_EOFCK': lambda ev: print(ev)},
    context=30,             # PC history depth kept per event
    sample_every=5_000_000, # periodic (step, recent-pc-history) snapshots,
                            # returned as result['samples'] -- for seeing
                            # what a long run is doing deep in without
                            # already knowing which symbol to watch (this
                            # is how the RA_HT_SLOT infinite loop, BUGS.md
                            # forty-third round, was actually found: the
                            # same 4-instruction cycle, unchanged, 5
                            # million steps apart, repeatedly)
    suppress_stderr=True,   # hide freeze() noise
)
```

Each event dict has `fi`, `step`, `op`, `pc_name`, `a1`, `a2`, `value`,
`history` (list of preceding PC names). This is what `run_find_writer()`
and the CLI's `--find-writer` are built on — reuse it instead of
reimplementing the Voca/Equal/Move dispatch-table patching pattern.

### Typical investigation workflow

```bash
# 1. What happens and where does it halt?
python3 tools/trace.py --fidx --steps 20000000

# 2. Which file is it stuck on? (fidx transitions tell you)
# 3. What's being written wrong?
python3 tools/trace.py --write-watch TAB --write-watch LF

# 4. Who reaches the bad PC?
python3 tools/trace.py --token-at BS_PARSE_INT --context 30

# 5. Pin down the step
python3 tools/trace.py --write-watch X --start-step 5000000 --stop-step 6000000
```

---

## test_macros.py — Macro equivalence tests

Registered in the hub as `--macro-tests`; also runs standalone directly.

```bash
python3 tools/test_macros.py              # run all 7 macro tests
python3 tools/test_macros.py RVOCA        # run one test by macro name
python3 tools/test_macros.py RVOCA CLEAR  # run several

# Low-level BS lexer tests
python3 tools/test_macros.py --bs-read-chunk /tmp/chunk.bin   # feed raw bytes to BS_READ_TOKEN
python3 tools/test_macros.py --bs-token-value "Add"           # feed token to BS_TOKEN_VALUE
```

Each test loads two .re fragments and compares NOVA slot contents:
- **Python version**: uses the macro command (Python inline handler)
- **Reca version**: equivalent raw NOVA nodes

Pass = slots match exactly (word, op, e1, e2, exit).
Current tests: RVOCA, RREDI, CLEAR, NOP, JEQ, NOLINK, SETREF.

---

## check_comments.py — Comment-integrity checker

Registered in the hub as `--comments`/`--lost`; also runs standalone directly.
The single authoritative comment checker — ground truth is the loader's own
`_strip_comments`, the exact routine every real load uses (no reimplemented
scanner to drift out of sync).

```bash
python3 tools/check_comments.py            # definite bugs only (silent+OK when clean)
python3 tools/check_comments.py --advisory # also show isolated/long spans
python3 tools/check_comments.py --spans FILE.re  # every comment span in one file, unfiltered
```

Bug signal: a comment that swallows real code always eats a *run* of real
commands (2+ swallowed command lines in a file = definite bug). Prose that
merely starts with a command word ("For each...") is isolated — one line,
carries prose tokens — and is reported only as advisory, never as a hard bug.

Run after any large refactor that touches comment structure.

`--spans` is the plain, unfiltered listing for one file under suspicion —
where the old `sim.py --lexer`/`--prepass`/`--diff` simulators would have
been reached for. Not a revival of that tool: `sim.py` compared two
*separate* Python re-implementations of the lexer and the prepass scanner
against each other; `LOAD_PREPASS_FILE` was since simplified to call the
real `BS_READ_TOKEN` directly (wax.re), so that particular disagreement
can no longer occur by construction. `--spans` instead reports what the
loader's own real `_strip_comments` sees for one file — real ground
truth, not a simulation, and nothing new to keep in sync.

---

## check_graph_shape.py — Structural graph snapshot / diff

Standalone, not in the hub. Compares two builds by *walking* from every
named lux and recording the opcode sequence it passes through, rather than
listing named luces and their operands — the latter is blind to breaks
inside runs of anonymous luces (e.g. after CHAIN-block dissolution made
most body luces anonymous).

```bash
python3 tools/check_graph_shape.py save out.json           # snapshot current build
python3 tools/check_graph_shape.py diff before.json after.json  # compare two snapshots
```

Addresses are never compared directly (they shift on every edit for
reasons that mean nothing); opcodes are compared by name, so a rerouted
jump shows up as a changed sequence, not a changed number.

---

## check_overlap.py — Allocated-body overlap checker

Standalone, not in the hub. Names and addresses are unique, but the *cells*
a name occupies are not automatically checked against its neighbours — a
lux is not one cell (an instruction is seven; a lux with extra lumina is
seven plus two per lumen plus a terminator), and that width is computed in
more than one place. If two of those computations disagree, one name's
body silently reaches into its neighbour.

```bash
python3 tools/check_overlap.py
```

---

## long_run.py — resumable, checkpointed multi-billion-step runs

```bash
python3 tools/long_run.py --steps 400000000     # run (or continue) up to N more steps
python3 tools/long_run.py --status              # just print current progress
python3 tools/long_run.py --fresh --steps N     # discard any checkpoint, start over
```

Built during the session that found the `RA_HT_SLOT`-reset bug
(BUGS.md, forty-fifth round) — investigating it needed billions of
steps, far more than one tool call's practical wall-clock budget.
Restarting from step 0 every invocation wastes all prior progress;
this pauses (checkpoints the whole aether, including the call stack —
see the module docstring for why the watermark has to cover the
*entire* address space, not just the touched region) and resumes
across separate invocations instead. Uses the same fast raw-dispatch
loop as `fast_profile.py` (no per-instruction Python tracing overhead)
so it can actually reach multi-billion-step depths within reasonable
wall time — this is NOT built on `trace.py`'s traced/wrapped machinery.

Chain multiple calls from a shell loop to accumulate steps:
```bash
for i in 1 2 3; do python3 tools/long_run.py --steps 400000000; done
```

Writes `long_run_status.txt` (this_run_step, cumulative_step,
wall_seconds, sk_fidx, rpos, msg) periodically and at the end of each
invocation. `msg=FINISHED NATURALLY` means the interpreter reached
pc=0 — check `sk_fidx` to see whether that's genuine completion (42)
or an unexpected halt worth investigating separately.

Needs `reca.sym` present and matching the current source to resume
(addresses are deterministic across builds of unchanged source, but a
source change invalidates a checkpoint's saved PC/addresses — run a
plain `freeze()` and delete `long_run_ckpt.bin` before resuming after
any edit to a file that affects self-hosted LOAD_MAIN's own control
flow).

---

## fast_profile.py — hotspot profiler for long runs

```bash
python3 tools/fast_profile.py --steps 200000000              # profile a fresh run
python3 tools/fast_profile.py --from-checkpoint --steps 60000000  # profile a burst
                                                         # resumed from long_run.py's
                                                         # checkpoint instead
```

The tool that actually found the `RA_HT_SLOT`-reset bug: `trace.py
--profile` runs a fully-traced loop, far too slow to profile deep into
a long run. This adds only a single dict increment per step to the
same fast raw-dispatch loop `long_run.py` uses, trading "no register
values, no PC history, no conditional watches" for reaching orders of
magnitude further within the same wall-clock budget. Prints the top-N
(`--top`, default 40) hottest PCs by symbolic name with hit counts.

What the real signal looked like in practice: one label (or a tight
cluster of labels forming one loop body) at counts roughly 1000x every
other label in the run — not a handful of outliers, the *dominant*
cost center for the whole run. If nothing stands out anywhere near
that dramatically, the slowness likely isn't one hot loop; reach for
`long_run.py` plus `--graph-equivalence`/direct register tracing
(`trace.py`'s `make_patched_run`) instead.

Use `trace.py`'s `make_patched_run` (not this tool) for anything
needing more than "which PCs are hot" — specific register values, call
history, conditional capture on a particular event.

---

## gen_compiler.py — Compiler file generator

```bash
python3 gen_compiler.py              # generate files + freeze + reca_init.ll
python3 gen_compiler.py --check      # print first 20 lines of preamble, no writes
python3 gen_compiler.py --no-freeze  # generate files only, skip freeze
```

Generates three files into `generated/`:
- `generated/preamble.re` — PREAM_* Lux byte chains from `preamble_template.ll`
- `generated/compiler_sf.re` — XLEN-dependent SF_* string constants for Yaku
- `generated/layout.re` — K_XLEN, K_AETHER_SIZE Aether layout constants

Run when:
- `symphony.py` constants change (XLEN, AETHER_SIZE)
- `preamble_template.ll` is updated

loader.py calls `gen_compiler.generate_artifacts()` automatically during freeze
if the generated files are missing or older than `symphony.py`.

---

## repl.py — Runtime entry point

```bash
python3 repl.py                        # interactive: type symbol names, run them
python3 repl.py --load example.re      # load file via PS_MAIN, run MAIN
python3 repl.py --load f.re --entry E  # run specific entry point
python3 repl.py --load f.re --steps N  # override step limit (default 10M)
```

Boot paths:
- **FAST**: `reca.bin` exists → thaw raw bits, run immediately
- **COLD**: `reca.bin` missing → calls `loader.freeze()` first

In interactive mode, all symbols from `reca.sym` are available. Type a
symbol name to execute it. Ctrl+D to exit. Useful for testing individual
subroutines in isolation.

---

## Infrastructure (not run directly)

### loader.py
Bootstrap loader. Called via `from loader import freeze; freeze()`.
Reads all `.re` files, builds Aether, runs LOAD_MAIN, writes `reca.bin` + `reca.sym`.

Key exported functions:
- `freeze()` → Loader — full build
- `load_symbols()` → dict — read `reca.sym` into name→addr dict
- `load_or_freeze()` → (Interpreter, symbols) — load from bin or freeze if missing
- `fresh_loader()` → Loader — new Loader on top of frozen base (for incremental loading)
- `discover_newref_macro_names()` → set — all macro names from macros.re NEWREFs

### interpreter.py
Hot execution path. Three loop modes in `_run()`:
- **icache** (`use_cache=True`) — caches hot ops with generation counter (default)
- **no-cache** (`use_cache=False`) — for graph-building (LOAD_MAIN), graph changes mid-run
- **progress** — adds step counter and periodic print

### symphony.py
Aether I/O and layout constants.
- `Aether(size)` — create new Aether
- `aether.freeze(path, watermark)` — save to bin (sparse v3 format)
- `aether.thaw(path)` — load from bin
- Slot constants: `SLOT_WORD=0`, `SLOT_OP=1`, `SLOT_E1=2`, `SLOT_E2=3`, `SLOT_EXIT=4`, `SLOT_NEXT=5`, `NOVA_SIZE=7`

### iris.py / iris_common.py / iris_source.py / iris_graph.py / iris_trace.py
The hub and its theme modules. `iris.py` owns the command contract
(`Command`, `Context`, NEEDS-level build sharing) and dispatches only —
every actual check/report lives in a theme module (`iris_source.py`:
source-text checks; `iris_graph.py`: built-graph checks; `iris_trace.py`:
traced checks) or a standalone file that plugs in
(`check_comments.py`, `test_macros.py`, `check_htable_reentrancy.py`).
`iris_common.py` holds domain helpers genuinely shared across those
modules (SEAL-registration scanners, packed-string decoding, address
naming, file discovery) — deliberately not in `iris.py` itself (that
file owns orchestration, not domain logic) and deliberately not
duplicated per-module (two independent copies of the SEAL scan drifted
out of sync in the past — see BUGS.md Pattern X).

---

## Common workflows

### Verify a change works
```bash
python3 -c "from loader import freeze; freeze()" && python3 tools/iris.py --lint && python3 tools/iris.py --invariants
```

### Full check (freeze + every check, one shared build + macro tests)
```bash
python3 -c "from loader import freeze; freeze()"
python3 tools/iris.py --checks
```

### Debug: register/symbol has a wrong value, who wrote it?
```bash
python3 tools/trace.py --find-writer LF
```
This is the first thing to reach for — not write-watch chains, not manual
`patched_run` scripts. Only fall back to `--write-watch`/`--token-at`/
`make_patched_run()` if `--find-writer`'s single-write answer isn't enough
(e.g. value comes from arithmetic across two registers).

### Debug: where does LOAD_MAIN halt?
```bash
python3 tools/trace.py --fidx --steps 20000000
```

### Debug: what writes to a register?
```bash
python3 tools/trace.py --write-watch RA_MC_PREV --write-watch SK_TMP
```

### Debug: infinite loop
```bash
python3 tools/trace.py --loop-detect --steps 50000000
```

### Debug: wrong token parsing
```bash
python3 tools/trace.py --token-at BS_PARSE_INT --context 20
```

### Check for accidental comment-swallowing
```bash
python3 tools/iris.py --comments
```

### Inspect a specific lux
```bash
python3 tools/iris.py --wiring JEQ_N1 LOAD_CMD_NEW BS_READ_TOKEN
```

### Run PS_MAIN on a file (without full freeze)
```bash
python3 tools/trace.py --psmain example.re
```

### Regenerate compiler files after changing XLEN
```bash
# Edit symphony.py XLEN constant, then:
python3 gen_compiler.py
python3 -c "from loader import freeze; freeze()"
```

### Generic PC-hit tracing (`trace.py --watch-pc`)

For "does this line get reached, how many times, in roughly what
order relative to other lines" questions, use this instead of writing
a new one-off tracing script:
```bash
python3 tools/trace.py --watch-pc LABEL_NAME_ONE LABEL_NAME_TWO
```
Reuses `make_patched_run` (already correct — calls the real,
unmodified `_run`, only swaps individual dispatch-table handlers, so
it automatically takes whichever branch the real `freeze()` call
actually takes). Watches via both `on_voca_target` (real, `RVOCA`-
called entry points) and `on_equal_nxt` (`JEQ`/`JZ`-reached labels) —
covers most real labels one way or the other. For a plain `NOVA` reached
only via auto-chain, or for a *register's own value* at each hit (not
just "was this PC reached"), use `make_patched_run` directly with its
own `on_write_dest`/`on_voca_target`/`on_equal_nxt` callbacks instead —
`--watch-pc` is deliberately the simple, common case, not a full
replacement for hand-written tracing when you need one.

### Manual `_run` patching for direct execution tracing (`interpreter.Interpreter._run`)

This is the technique behind most root-cause finds that don't fit any
existing flag: monkey-patch `interpreter.Interpreter._run` with a
custom loop that watches specific registers/PCs while `freeze()` runs
for real. Powerful, but easy to get wrong in ways that silently produce
empty or misleading results rather than an obvious error — every
mistake below was made at least once during the session that first
wrote this section, several more than once.

**1. Get symbols from a *separate*, preliminary call — never from the
`sym` parameter `_run` itself receives.**
```python
import loader, interpreter
loader.freeze()                       # throwaway, just to populate symbols
syms1 = loader.load_symbols()         # THIS dict, not the `sym` argument below
addr_pt = syms1['SOME_LABEL']
```
The `sym` parameter your patched function receives is `None` for the
real, main execution call (confirmed directly this session — this is
what caused a `TypeError: 'NoneType' object is not subscriptable`,
which `freeze()`'s own error handling then silently swallowed, making
several traces in a row produce empty results with no visible error at
all). Only the values captured via `loader.load_symbols()` (or
`ldr.symbols` from a completed `freeze()`) are reliable.

**2. `_run` is called more than once per `freeze()` — always guard
for the early call(s) before the aether array is even allocated.**
```python
orig_run = interpreter.Interpreter._run
def patched_run(self, start, progress_every, sym, use_cache=True, max_steps=0):
    if self.aether.aether is None:
        return orig_run(self, start, progress_every, sym, use_cache, max_steps)
    aether = self.aether.aether
    ...  # your real logic
```
Without this guard, an early, short call can crash your own patch
(`aether` or `sym` being `None`) before the real, long-running call
ever gets a chance to execute — and since `freeze()` wraps
`execute_aether` in a try/except of its own, the exception is caught
and printed as `LOAD_MAIN error: ...` rather than propagating out to
your own script, so it's easy to miss entirely.

**3. Return immediately from inside the loop on every exit condition
— never rely on a loop finishing normally to "fill in" a shared dict.**
```python
result = {}
def patched_run(self, start, progress_every, sym, use_cache=True, max_steps=0):
    ...
    while pc:
        ...
        if pc == target_pc:
            result['reason'] = 'hit_target'; result['steps'] = steps
            return steps                      # <-- return HERE, not after the loop
        handler = dispatch.get(op_id)
        if handler is None:
            result['reason'] = 'crash'; result['steps'] = steps
            return steps
        pc = handler(e1, e2, ex, nxt, aether)
    result['reason'] = 'normal'; result['steps'] = steps   # loop exited via pc==0
    return steps
```
Wrap the whole loop body in try/except and log the exception too if
you're touching addresses that might be out of range (e.g. an
uninitialized or runaway register used as an address) — an unguarded
`IndexError` from `aether[...]` behaves exactly like point 2's own
`TypeError`: silently swallowed by `freeze()`, your own result dict
stays empty, and it looks like the trace "didn't capture anything"
rather than "crashed."

**4. A register's value at one specific PC can be misleading on its
own — trace it *alongside* a nearby, independent signal (like a step
counter) to catch a value being clobbered between two points that look
adjacent in the source but aren't adjacent in real execution.**
This is how `RA_HT_PROBE` being silently overwritten by an unrelated
called subroutine (`HT_NAME_CMP`, reusing it as scratch) was found:
noticing that two consecutive log points, only ~20 real steps apart,
showed a jump from `0` to `11216` — impossible via the loop's own `+1`
per iteration, and therefore proof of a direct overwrite somewhere in
between, not a step-counting bug in the trace itself.

**5. Grouping captured samples by a register's own value (e.g. "group
every probe attempt by which hash it belongs to") can silently split
one logical call's own data across multiple groups if that same
register gets reused mid-call for something unrelated.** Prefer
capturing an ungrouped, sequential list first (timestamp/step +
whatever registers matter), and only group/interpret afterward, once
you've confirmed the registers you're grouping by are actually stable
for the span you think they're stable for.

**6. `_run`'s own "unrecognized op" handling is not a hard stop — it
consults a fault vector first, and your patch needs to too, or you
will get false "crash"/"normal end" readings that don't match the
real interpreter's own behavior.**
```python
fv = self._fault_vector
...
handler = dispatch.get(op_id)
if handler is None:
    if fv and aether[fv]:
        pc = aether[fv]; fv = 0    # redirect, don't stop
        continue
    break   # only a REAL stop if no fault vector is registered
```
Confirmed directly this session: a simplified patch that treats
`handler is None` as an unconditional stop can produce a "clean,
42/42, normal program end" reading that the *real*, unpatched
interpreter never actually reaches — the official, unpatched
`freeze()` run stayed at a lower, stable count across five repeated
runs while the simplified patch consistently reported full completion.
Always include the fault-vector check (`self._fault_vector`,
`aether[fv]`) in any patched `_run`, not just the dispatch-table
lookup — otherwise your trace is testing different logic than the one
that actually runs in production.

**7. The real `freeze()` call path uses `_run`'s own "fast path with
step limit" branch (`max_steps` set, `progress_every` not set) — this
branch dispatches every op, including `Voca`/`Redi`, through the
ordinary `dispatch.get(op_id)` table lookup, no inline fast-path logic
at all. The one thing it has that a naive patch easily forgets: `if
steps > _max_steps: break`.**

Confirmed directly this session, the hard way: a "42/42, clean normal
end" reading from a patched trace that never implemented this step
limit did not match the real, unpatched `freeze()`'s own, stable "34/42"
result — the real call was hitting its own 100M-step budget and
stopping there, while the unlimited patched trace kept going past it.
(An earlier theory blamed `Voca`/`Redi`'s own inline call-stack logic
existing in a *different* branch of `_run` — true as a general fact
about the interpreter, and worth keeping in mind for other branches,
but not the actual explanation here; recorded as a caution against
settling on the first plausible-looking difference between two code
paths instead of confirming which branch is actually the one that
matters.) The real fix, once identified: `LOAD_MAIN` genuinely needs
more than 100,000,000 steps to reach a clean end — the budget itself
was too low, the same class of fix as increasing `AETHER_SIZE` earlier
this session when a real allocator turned out to need more room than
the previous ceiling allowed.

**When writing a patched `_run`, always mirror the *exact* branch the
real call actually takes** (check `max_steps`/`progress_every` against
what `execute_aether`/`freeze()` actually pass) rather than assuming
one, "most complete-looking" implementation covers every case — `_run`
has four genuinely different branches (fast-path-with-limit, cached,
uncached-no-progress, progress-path), and only one of them is what
production actually runs.
