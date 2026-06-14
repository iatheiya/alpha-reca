# Reca

A language where all is Aether.

Only Aether is defined. The rest is yours to define — with freedom of interpretation.

---

## The Model

**Aether** — a flat array of **Lux** (u64). Lux are connected by directed **Lumen** (`src --rel--> exit`).

Execution is traversal of this graph, with no distinction between code and data — everything is u64, always.

## Example

```reca
NEW X
SET X 7

NOLINK
ITO DOUBLE   Add El1=X El2=X Exit=RA_TMP
RREDI DOUBLE_r

NOLINK
ITO MAIN     Move El1=C_0 Exit=RA_TMP
RVOCA MAIN_CALL DOUBLE
ITO MAIN_END End
```

`X` is a Data Lux whose value is 7. `DOUBLE` is an ITO Lux that adds
`X` to itself, writing into `RA_TMP`, then returns via `Redi`. `MAIN`
calls `DOUBLE` via `Voca` — which saves the return address and jumps.
After execution, `RA_TMP` holds 14.

## How it runs

There is no AST, no JIT, and no type system at the language level.

The graph is built directly, without intermediate representation.
Reca does not impose types, but nothing prevents building them.

Execution is a tight loop over the Aether. The interpreter never
leaves its own compiled binary; the Aether stays RW throughout.

## Why Reca

Most languages choose a trade-off for you, once, at the language level — speed
or mutability, density or safety. Reca doesn't make that choice. The graph
carries no inherent restriction; what an optimiser can remove depends on how
the code actually uses the graph.

This works because the execution model is designed for the hardware: linear
by default, with mutability built in as a single redirect — an architectural
property, not an added cost. Starting from mutability would mean adding
restrictions later to regain speed.

The same absence of imposed structure makes Reca AI-native: there is no syntax
standing between intent and meaning. Density and predictability come from the
absence of what isn't needed.

## Quick Start

```bash
python3 loader.py             # build reca.bin
python3 repl.py --run example.re  # run file
```

Full reference: `docs/TOOLING.md`

## Documentation

| File | Contents |
|------|----------|
| `docs/LANGUAGE.md` | Syntax, 24 Aspects, execution model |
| `docs/ARCHITECTURE.md` | System architecture, pipeline, key files |
| `docs/CONVENTIONS.md` | Naming, calling convention, code style |
| `docs/REFERENCE.md` | All macros and registers |
| `docs/TOOLING.md` | How to use loader.py, diag.py, repl.py, trace.py |
| `docs/GLOSSARY.md` | Definitions: Lux, Lumen, Aether, Aria, Aspect and more |
| `docs/ROADMAP.md` | Open tasks and current status |

## Status

Self-hosting was reached, then lost during architecture changes.
The current goal is restoring it on the new foundation.

The bootstrap layer is written in Python. It is being replaced by Reca.
