# TOOLING.md — Reca Tools

All tools are Python scripts in the project root. Run with `python3`.

---

## loader.py — Main build

```bash
python3 loader.py
```

Reads all `.re` files, builds the graph in Aether, runs LOAD_MAIN (self-hosted loader),
saves `reca.bin` and `reca.sym`.

**What happens inside:**
1. Wave-A: Python allocates addresses for all names (`NEW`, `NEWREF`, `NEWSET`, `ITO`, `BLOCK`).
2. Wave-B0: Python wires `NEWREF`/`SETREF` (macro entry points).
3. Wave-B: Python wires `ITO`, `RVOCA`, `RREDI`, `LINK`, `JEQ`, `JZ`, `CLEAR`, `NOP`…
4. LOAD_MAIN: Reca itself re-reads all `.re` files and builds the graph authoritatively.
5. Freeze: writes bin + sym.

**Output files:**
- `reca.bin` — binary snapshot of Aether (sparse format).
- `reca.sym` — symbol table: `name\taddress` per line.

---

## diag.py — Diagnostics

```bash
python3 diag.py                 # quick health check
python3 diag.py --load-main     # test LOAD_MAIN (how many files passed)
python3 diag.py --lint          # static lint
python3 diag.py --dump SYMBOL   # show lux contents by name
python3 diag.py --steps N       # run + print progress every N steps
```

### Health check (no flags)
Checks: 24 Aspects present, ITO_SIZE consistent, aether[0]=0, critical symbols defined.

### `--load-main`
Runs LOAD_MAIN (self-hosted loader). Shows how many `.re` files successfully
completed Wave-B. Target: all of them.

### `--lint`
Static analysis of the loaded graph:
- **Phantom ITOs**: luces with op=0 (symbol declared but ITO never wired — typo in Exit= or LINK).
- **Dead references**: EXIT/NEXT point to a lux with op=0.
- **Self-ref violations**: ITO lux where word != addr (self-ref expected).

```python
# Quick manual phantom check:
phantom = [addr for addr in ldr._ito_addrs if aether[addr+1] == 0]
```

### `--dump SYMBOL`
Prints all slots of a lux and its lumens.

---

## test_macros.py — Macro tests

```bash
python3 test_macros.py              # all tests
python3 test_macros.py RVOCA        # specific macro
python3 test_macros.py JEQ CLEAR    # several
```

For each macro, loads two fragments:
- **py-version**: uses the macro command (Python inline handler).
- **re-version**: equivalent raw ITOs.

Compares lux slots (word, op, e1, e2, exit). Pass = byte-exact match.

**Current status**: RVOCA, RREDI, CLEAR, NOP, JEQ, NOLINK, SETREF — all PASS.

---

## repl.py — REPL

```bash
python3 repl.py                    # interactive: type symbol names to run them
python3 repl.py --run example.re   # load file, run MAIN, exit
```

### Interactive mode (no flags)

```
Reca REPL — 288388 symbols loaded. Type a symbol name to run it, Ctrl+D to exit.
> ALLOC_LUCES
Done. (8 steps)
> BS_READ_TOKEN
Done. (14 steps)
> 
```

All symbols from `reca.bin` are available. Useful for testing individual subroutines.
If a name is not found, the REPL suggests close matches.

### `--run filepath`

```bash
python3 repl.py --run example.re            # run MAIN from file
python3 repl.py --run example.re --entry DOUBLE   # run a specific entry point
python3 repl.py --run example.re --steps 1000000  # override step limit (default 10M)
```

Loads the `.re` file via the Python loader, executes the entry point, prints
`Done. (N steps)` on exit. If the step limit is reached, prints `Stopped: step
limit reached`.

This is the primary way to run user `.re` files.

---

## trace.py — Tracing

```bash
python3 trace.py SYMBOL_NAME
python3 trace.py SYMBOL_NAME --steps 10000
python3 trace.py SYMBOL_NAME --no-dedup
```

Runs a Reca program with tracing of every executed lux.
Outputs run-length encoded list: `/count.id` (count=1 → `/id`).

Useful for debugging infinite loops and unexpected execution paths.
If a program does not terminate — use `--steps N` to limit first.

---

## diag_loadmain.py — LOAD_MAIN detailed diagnostics

```bash
python3 diag_loadmain.py
python3 diag_loadmain.py --file saku.re
python3 diag_loadmain.py --htable BS_HTAB_000
```

Runs LOAD_MAIN with a detailed log: which files loaded, where it stalled.
Use when `diag.py --load-main` shows an incomplete counter.

---

## gen_compiler.py — LLVM preamble and layout generator

```bash
python3 gen_compiler.py
```

Generates three files needed by the Yaku compiler:
- `preamble.re` — LLVM IR preamble packed as Reca byte chains
- `compiler_sf.re` — XLEN-dependent string constants for Yaku
- `aria/layout.re` — Aether layout constants for the compiler

Run when `symphony.py` constants change (XLEN, AETHER_SIZE) or when
`preamble_template.ll` is updated. Output files are not committed to the repo.

---

## symphony.py — Aether I/O

Not run directly. Used by loader.py and diag.py.

Key functions:
- `Aether(size)` — create a new Aether.
- `freeze(path, watermark)` — save to bin.
- `thaw(path)` — load from bin.
- `read_header(path)` — read metadata without loading.

**Sparse v3 format**: stores only non-zero segments. Typical bin size — 400–500 KB
for ~600K Aether luces.

---

## interpreter.py — Interpreter

Not run directly. The hot execution path.

Three loop modes in `_run()`:
- **icache** (`use_cache=True`): caches hot-ops with a generation counter.
- **no-cache** (`use_cache=False`): for graph-building (LOAD_MAIN) — the graph changes mid-run.
- **progress**: adds a step counter and periodic print (debug only).

`_run_traced()`: separate loop that writes pc into the trace buffer on every step.

---

## Common Workflows

### Apply a change to .re and verify:
```bash
python3 loader.py && python3 diag.py
```

### Find phantom labels after changes:
```bash
python3 loader.py && python3 diag.py --lint 2>&1 | grep PHANTOM
```

### Test a specific macro:
```bash
python3 test_macros.py JEQ
```

### Debug a hang:
```bash
python3 trace.py MY_ENTRY --steps 50000 2>&1 | head -100
```

### Check self-hosting progress:
```bash
python3 diag.py --load-main
# Target: all files completing LOAD_MAIN pass2
```
