# ARCHITECTURE.md — Reca System Architecture

## Pipeline

```
.re sources
    ↓  loader.py — Python scaffold (allocation + Wave-B wiring + LOAD_MAIN launch)
    ↓  LOAD_MAIN — Reca loader (reads .re, builds the graph independently)
reca.bin + reca.sym
    ↓  interpreter.py
LLVM IR  (via --self-compile)
    ↓  llc + clang
native binary
```

Python is the scaffold layer. LOAD_MAIN is the authoritative loader.
Wave-B in Python duplicates LOAD_MAIN and is gradually removed as it stabilises.

---

## File Architecture

| File | Role | After self-hosting |
|------|------|--------------------|
| `aspects.re` | 24 Aspects | Unchanged |
| `constants.re` | SLOT_*, ITO_SIZE, C_N | Unchanged |
| `registers.re` | RA_MA*, RA_LOAD_*, SC_* | Unchanged |
| `bootstrap.re` | Primitives: htable, tokeniser, file I/O, bump alloc | **Permanent** — always needed |
| `macros.re` | Reca programs for commands (JEQ, JZ, CLEAR, RVOCA…) | **Permanent** |
| `saku.re` | LOAD_MAIN: .re file loader | **Permanent** |
| `alloc.re` | ALLOC_LUX, ADD_LUMEN, REMOVE_LUMEN, K_CURSOR | Unchanged |
| `htable.re` | HT_LOOKUP, HT_INSERT | Unchanged |
| `symphony.re` | SR_WALK_ONE, SR_GLL/GLR/GLE/GLX | Unchanged |
| `parser.re` | Runtime .re parser (for self-compile) | Unchanged |
| `yaku.re` | LLVM IR emitter | Unchanged |
| `loader.py` | Python scaffold → gradually shrinks | Only: aspects + launch |

### bootstrap.re — why not rename it

Despite the name, bootstrap.re contains **permanent primitives** used after self-hosting:
- `BS_READ_TOKEN`, `BS_INTERN`, `BS_LOOKUP` — lexer/interner (always needed by LOAD_MAIN)
- `BS_READ_BYTE`, `BS_SKIP_TO_EOL` — file I/O
- `ALLOC_LUCES` — bump allocator (used by all code)
- `BS_HTAB_000` — symbol table

The file iteration logic (`BS_MAIN`, `BS_LOAD_ALL_NEW`, `BS_WIRE_ALL`) is an
alternative loading path, currently unused (LOAD_MAIN is used instead).

---

## Loader

**Python (loader.py) — permanent:**
- `update_relations()`: registers SLOT_* and 24 Aspects into Python dispatch
- `freeze()`: writes bin/sym to disk
- Wave-A: address allocation for all names (needed before LOAD_MAIN runs)

**Python (diag.py):**
- `_setup_load_main()`: initialises htable, file list, K_CURSOR — used to run LOAD_MAIN in diagnostics

**Python (loader.py) — scaffold, being removed:**
- Wave-B: wires ITO/RVOCA/RREDI/LINK — duplicates LOAD_MAIN, kept as a safety net for now
- `_wave_newref_line`: wires NEWREF/SETREF — LOAD_MAIN handles this itself

**saku.re (LOAD_MAIN) — authoritative loader:**
- Wave 1: prepass — counts LINK lumens per source
- Wave 2: load — builds the graph, allocates ITO luces of the right size
- Commands: ITO, NEW, NEWREF, NEWSET, SETREF, LINK, BLOCK, RVOCA, RREDI, NOLINK
- Backfill for forward references
- Macros: JEQ, JZ, CLEAR, NOP, FOR, SWITCH, SAVE via `{CMD}_MACRO` lookup
