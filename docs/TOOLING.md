# TOOLING.md — Reca Developer Tools

All tools are Python scripts in the project root. Run with `python3`.
Infrastructure files (loader.py, interpreter.py, symphony.py) are not run directly.

---

## Quick reference

| Task | Command |
|------|---------|
| Build / self-host | `python3 -c "from loader import freeze; freeze()"` |
| Static lint | `python3 diag.py --lint` |
| Invariant check (run after every change) | `python3 diag.py --invariants` |
| Find who wrote a bad value | `python3 trace.py --find-writer SYM` |
| Run all macro tests | `python3 test_macros.py` |
| Trace a halt | `python3 trace.py --steps 5000000` |
| Watch what writes to X | `python3 trace.py --write-watch X` |
| Watch file-index transitions | `python3 trace.py --fidx` |
| Run PS_MAIN on a file | `python3 trace.py --psmain example.re` |
| Check comment spans | `python3 check_comments.py` |
| Check ITO sizing | `python3 check_ito_sizing.py` |
| Regenerate compiler files | `python3 gen_compiler.py` |
| Compare lexer vs Python | `python3 sim.py --diff myfile.re` |
| Run a program | `python3 repl.py --load example.re` |

---

## trace.py — Universal execution tracer

The primary debugging tool. Runs freeze() (or uses reca.bin), instruments
the interpreter, and reports on the run.

```bash
# Basic: run to halt, show crash context (last 30 PCs, registers)
python3 trace.py

# From a specific lux
python3 trace.py --from LOAD_MAIN_LOOP

# Cap execution
python3 trace.py --steps 5000000

# Stop at symbol
python3 trace.py --until BS_DONE

# Print every step (very verbose)
python3 trace.py --verbose

# Watch a register change (with optional step window)
python3 trace.py --watch RA_MC_PREV
python3 trace.py --watch RA_MC_PREV --watch SK_FIDX

# Profile: top-N hottest PCs
python3 trace.py --profile

# Stop on first unknown-op (dispatch failure) or fault vector
python3 trace.py --fault

# Show call stack (Voca/Redi pairs) on every call
python3 trace.py --stack

# Run compiler (Yaku), show EP_BUF state after
python3 trace.py --compile

# LOAD_MAIN trace: show file+line+token for each dispatch
python3 trace.py --loadmain
```

### Newer flags

```bash
# Print old→new every time SYM's address is written (Move or Write)
python3 trace.py --write-watch TAB --write-watch LF

# Dump tokbuf+TLEN every time PC reaches SYM
python3 trace.py --token-at BS_PARSE_INT --context 20

# Run PS_MAIN against FILE.re (uses cached reca.bin, no full freeze)
python3 trace.py --psmain example.re

# Print SK_FIDX file-index transitions during LOAD_MAIN
python3 trace.py --fidx

# History depth for --write-watch / --token-at (default 30)
python3 trace.py --write-watch X --context 50

# Step-range filters (apply to --write-watch and --token-at)
python3 trace.py --write-watch X --start-step 1000000 --stop-step 5000000
python3 trace.py --token-at X --after-step 2000000

# Print SK_FIDX every N steps (progress during long runs)
python3 trace.py --steps 50000000 --progress-every 5000000

# Detect repeating PC-state cycles (infinite loops)
python3 trace.py --loop-detect

# Print lux wiring for named symbols after freeze
python3 trace.py --dump LOAD_CMD_NEW --dump JEQ_N1

# Print call depth (Voca=push, Redi=pop) on every call
python3 trace.py --stack-depth

# Halt entire trace at step N
python3 trace.py --stop-step 3000000

# Find every write to SYM's aether address (who wrote it, when, with what
# instruction history). THE tool for "register has a wrong value, who set it?"
# instead of guessing with chains of write-watch/token-at scripts.
python3 trace.py --find-writer LF
python3 trace.py --find-writer LF --find-writer TAB --context 12
```

### --find-writer SYM — find who wrote a bad value

The fastest path from "register X has value V, who set it?" to an answer.
Wraps `make_patched_run(on_write_dest=...)` (see below) so you don't have to
hand-write a `patched_run` each time.

```bash
python3 trace.py --find-writer LF
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
    on_voca_target={'SWITCH_START': lambda ev: print(ev)},
    on_equal_nxt={'SWITCH_EOFCK': lambda ev: print(ev)},
    context=30,            # PC history depth kept per event
    suppress_stderr=True,  # hide freeze() noise
)
```

Each event dict has `fi`, `step`, `op`, `pc_name`, `a1`, `a2`, `value`,
`history` (list of preceding PC names). This is what `run_find_writer()`
and the CLI's `--find-writer` are built on — reuse it instead of
reimplementing the Voca/Equal/Move dispatch-table patching pattern.

### Typical investigation workflow

```bash
# 1. What happens and where does it halt?
python3 trace.py --fidx --steps 20000000

# 2. Which file is it stuck on? (fidx transitions tell you)
# 3. What's being written wrong?
python3 trace.py --write-watch TAB --write-watch LF

# 4. Who reaches the bad PC?
python3 trace.py --token-at BS_PARSE_INT --context 30

# 5. Pin down the step
python3 trace.py --write-watch X --start-step 5000000 --stop-step 6000000
```

---

## diag.py — Static and runtime diagnostics

```bash
python3 diag.py                    # full health check + graph + macros
python3 diag.py --lint             # static lint (phantom ITOs, dead refs, self-ref violations)
python3 diag.py --health           # freeze health only
python3 diag.py --graph            # Aether graph integrity
python3 diag.py --macros           # registered vs used macro check
python3 diag.py --strings          # string encoding layout
python3 diag.py --parity file.re   # macro parity: Python loader vs Reca runtime
python3 diag.py --lost             # symbols swallowed by comment blocks
python3 diag.py --deps             # SCC load order and dependency tree
python3 diag.py --indent           # what indent-synthesis produced
python3 diag.py --load-main        # run LOAD_MAIN, show per-file pass/fail
python3 diag.py --trace [...]      # delegate to trace.py (remaining args forwarded)
python3 diag.py --invariants       # comprehensive post-freeze invariant check (run after EVERY change)

# Low-level lux inspection
python3 diag.py --wiring LOAD_CMD_NEW JEQ_N1   # print lux slots for named symbols
python3 diag.py --broken                        # check primitives self-ref + ITO op=0
python3 diag.py --htable BS_READ_TOKEN          # look up name in runtime hash table
```

### --invariants — postcondition checker (check_state equivalent)

Runs a battery of sanity checks against the frozen Aether: ASCII constant
values, `C_N` constant addresses/formula gaps, key ITO wiring
(`NEWSET_START`, `LOAD_CNS_WR`, ...), critical-symbol presence, macro
registration, and self-hosting file-processing progress (how many of the
42 `.re` files fully dispatch). Reports FAIL (definite bugs) vs WARN
(suspicious, may be intentional — e.g. a register holding a non-zero value
after freeze, which is often fine).

```bash
python3 diag.py --invariants
```

Run this **after every significant change**, before chasing a bug by hand —
a wrong ASCII constant or broken ITO wiring shows up here immediately
instead of after 20 minutes of manual tracing.

### --lint output

The most-used flag. Reports:
- **Phantom ITOs**: luces with op=0 (symbol declared but never wired — usually a typo in Exit= or macro name)
- **Dead refs**: EXIT or NEXT pointing to a lux with op=0
- **Self-ref violations**: ITO lux where `word != addr`

Expected output: `✓ Lint clean  (N Pattern-R phantoms/dead-refs suppressed as expected)`

The `(N suppressed)` number grows only when new Pattern-R macros are added.
If the count changes unexpectedly, something is wrong.

---

## test_macros.py — Macro equivalence tests

```bash
python3 test_macros.py              # run all 7 macro tests
python3 test_macros.py RVOCA        # run one test by macro name
python3 test_macros.py RVOCA CLEAR  # run several

# Low-level BS lexer tests
python3 test_macros.py --bs-read-chunk /tmp/chunk.bin   # feed raw bytes to BS_READ_TOKEN
python3 test_macros.py --bs-token-value "Add"           # feed token to BS_TOKEN_VALUE
```

Each test loads two .re fragments and compares ITO slot contents:
- **Python version**: uses the macro command (Python inline handler)
- **Reca version**: equivalent raw ITO nodes

Pass = slots match exactly (word, op, e1, e2, exit).
Current tests: RVOCA, RREDI, CLEAR, NOP, JEQ, NOLINK, SETREF.

---

## check_comments.py — Block comment span checker

```bash
python3 check_comments.py                  # scan all .re files, threshold=8 lines
python3 check_comments.py --threshold 4   # flag spans >= 4 lines
python3 check_comments.py --spans-only    # just list spans, no classification
```

Finds suspicious `//...//` block comment spans that may accidentally swallow
real code (missing closing `//`). Classifies each span: does it contain lines
that look like real commands (ITO, NEW, RVOCA, JEQ, etc.)?

Run after any large refactor that touches comment structure.

---

## check_ito_sizing.py — ITO allocation invariant checker

```bash
python3 check_ito_sizing.py
```

For every name that appears as the target of `ITO`, `RVOCA`, `RREDI`, `JEQ`,
`JZ`, `CLEAR`, `WAVE`, etc. — verifies that its address is in `_ito_addrs_set`
(allocated as full 7-lux ITO, not a small data lux).

Violations = under-allocation bug class (same root cause as the 33/42 blocker
fixed in the BUGS.md Pattern-R entry). Run after adding new naming commands.

---

## sim.py — Lexer/prepass simulators

```bash
python3 sim.py --prepass myfile.re   # simulate LOAD_PREPASS_FILE byte scanner
python3 sim.py --lexer myfile.re     # simulate BS_READ_TOKEN comment logic
python3 sim.py --diff myfile.re      # run both, show where they disagree
```

Faithful Python re-implementations of Reca's own lexer (BS_READ_TOKEN
comment/block handling) and prepass scanner. Use when suspecting a
Python-vs-Reca parsing disagreement on a specific file.

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
- Slot constants: `SLOT_WORD=0`, `SLOT_OP=1`, `SLOT_E1=2`, `SLOT_E2=3`, `SLOT_EXIT=4`, `SLOT_NEXT=5`, `ITO_SIZE=7`

---

## Common workflows

### Verify a change works
```bash
python3 -c "from loader import freeze; freeze()" && python3 diag.py --lint && python3 diag.py --invariants
```

### Full check (freeze + lint + invariants + tests)
```bash
python3 -c "from loader import freeze; freeze()"
python3 diag.py --lint
python3 diag.py --invariants
python3 test_macros.py
```

### Debug: register/symbol has a wrong value, who wrote it?
```bash
python3 trace.py --find-writer LF
```
This is the first thing to reach for — not write-watch chains, not manual
`patched_run` scripts. Only fall back to `--write-watch`/`--token-at`/
`make_patched_run()` if `--find-writer`'s single-write answer isn't enough
(e.g. value comes from arithmetic across two registers).

### Debug: where does LOAD_MAIN halt?
```bash
python3 trace.py --fidx --steps 20000000
```

### Debug: what writes to a register?
```bash
python3 trace.py --write-watch RA_MC_PREV --write-watch SK_TMP
```

### Debug: infinite loop
```bash
python3 trace.py --loop-detect --steps 50000000
```

### Debug: wrong token parsing
```bash
python3 trace.py --token-at BS_PARSE_INT --context 20
python3 sim.py --diff suspicious_file.re
```

### Check for accidental comment-swallowing
```bash
python3 check_comments.py
```

### Inspect a specific lux
```bash
python3 diag.py --wiring JEQ_N1 LOAD_CMD_NEW BS_READ_TOKEN
```

### Run PS_MAIN on a file (without full freeze)
```bash
python3 trace.py --psmain example.re
```

### Regenerate compiler files after changing XLEN
```bash
# Edit symphony.py XLEN constant, then:
python3 gen_compiler.py
python3 -c "from loader import freeze; freeze()"
```
