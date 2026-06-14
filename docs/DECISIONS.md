# DECISIONS.md — Architectural Decisions

This file records decisions that **look like problems** but are intentional,
and explains why. The goal: avoid repeating the same reasoning twice.

---

## Architecture

### SLOT_NEXT: hybrid semantics (implicit fall-through + explicit link)

```
aether[pc + SLOT_NEXT] == 0  →  implicit fall-through: next_pc = pc + ITO_SIZE
aether[pc + SLOT_NEXT] != 0  →  explicit graph link:   next_pc = aether[pc + SLOT_NEXT]
```

Loader leaves SLOT_NEXT=0 for physically adjacent ITOs (already in memory order).
Runtime can patch SLOT_NEXT to insert a new lux without memory shifts.
This gives both cache-friendly linear execution and runtime graph mutability.

Trade-off: ~14% memory overhead (one zero slot per ITO). Acceptable — no better alternative.
Removing SLOT_NEXT = killing runtime mutability.
Two ITO types (6-slot + 7-slot) = branch predictor chaos.

---

### Inline lumens vs separate luces

Lumens sit inline in the same lux (after pad). In 95–99% of cases there are zero
extra lumens — the branch predictor learns the pattern, the pipeline flies.
A rare mispredict (10–15 cycles) << constant cache misses from jumping to separate luces
(200–300 cycles). Inline wins.

---

### Call stack: CS_PUSH / CS_POP

Separate region in Aether (STACK_BOTTOM..STACK_TOP).
All non-leaf functions go through CS_PUSH/CS_POP.
The `_OUTER` pattern is fully removed (561 replacements).

---

### Flux: no predefined types

No prebuilt slot combinations. A type lux describes structure dynamically.
Fixing specific types contradicts the idea of flux — the author decides the structure.

---

### Flux as operand

The interpreter does not interfere. A flux in el1/el2 → the reader gets
the word as a value. Semantics are defined by the author through usage context.

---

### Flux zone fragmentation

Current: bump allocator (simple, fast).
Future: free-list when fragmentation becomes a real problem.
Does not block the current implementation.

---

### Exit — single direction concept

`Target` and `Dest` as separate relations are retired. `Exit` covers both.
Unifying them removes a redundant distinction that added no information.

---

### Parallelism is an Aria, not native

Baking dataflow scheduling into the native interpreter would add overhead to every
instruction, killing the fast path even where parallelism is not needed.

---

### Variadic element macro — open future Aria

Extending ITO with N inputs and M exits is a higher-level graph pattern.
The macro compiles to a chain of ITOs. ITO stays 7 slots.

---

## Macros

### EMIT / EMITI / PUTBYTE — three separate macros

**Looks like:** duplication (three similar bodies).
**Why not:** Semantically different operations — string, integer, byte. Names carry
semantics and are used at call-sites (yaku.re). Implementations share `RCN_IMPL`
(common body); public names are preserved.

---

### WALK_ONE vs WALK_ITO — different lux naming mechanisms

**Looks like:** clones with one difference (offset C_1 vs C_7).
**Why not:** WALK_ONE generates **named** luces (MA3/MA4/MA5 — auto-suffix, entered
into LSYM). WALK_ITO — **anonymous** (`__LT_ALLOC_ITO`). Intentional distinction:
named luces are needed when sub-luces must be referenced externally.
Structurally different, not duplication.

---

### JEQ/JZ/LX/LH/LINK_OP/UNLINK_OP — inline autolink with RA_MA_RET

**Looks like:** inconsistency (others use `Voca AUTOLINK`).
**Why not:** `RA_MA_RET` here links luces **within one macro** (between the second
and third lux). `AUTOLINK` and `RA_MC_PREV` link **with the external chain**
(previous ITO in the .re file). Fundamentally different roles.
`WIRE_AUTOLINK` is not applicable for intra-macro links.

---

### RRET/RCALL — named luces linked via explicit Next, not RA_MC_PREV

**OBSOLETE — RRET/RCALL have been removed (zero callers; superseded by the
automatic call stack, see aria/regs.re). Kept for historical reference only.**

**Looks like:** does not use WIRE_AUTOLINK.
**Why not:** Luces MA1..MA5 are linked via `Add MA_X+C_5→slot; Write slot←MA_Y`.
This is an explicit link through the Next slot, not through `RA_MC_PREV`.
WIRE_AUTOLINK updates `RA_MC_PREV` — not applicable here. Inline wire stays.

---

### Two-pass scheme in saku.re

**Looks like:** could be single-pass (backfill exists).
**Why not:** Pass 1 counts the number of LINK lumens per source.
This is needed for compact in-place memory allocation — no overallocation.
Backfill resolves forward references but does not give lumen block size upfront.
Two passes is an architectural decision, not a mistake.

---

### parser.re — own 3-digit number parser instead of BS_PARSE_INT

**Looks like:** duplicated parsing logic.
**Why not:** parser.re parses a specific format — 3-digit numeric suffixes in names
(NNN in OB_NNN, DS_NNN). This is a specialised parser for a fixed context,
not a general number parser. BS_PARSE_INT is universal. Different tasks.

---

### parser.re — two-level JT/JTL dispatch instead of hash table

**Looks like:** inconsistency with saku.re/bootstrap.re.
**Why not:** parser.re is a compiler, optimised for speed. JT on first byte +
JTL on second byte = O(1) dispatch without hash computation. saku.re is a
runtime loader with lower speed requirements. Different contexts, different
approaches are justified.

---

### Three string packing approaches (bootstrap / loader / parser)

**Looks like:** inconsistency.
**Why not:** Each level solves its own task — bootstrap: speed without escape,
loader: full string parser with special characters, parser: templating with
`{name}` placeholders. These are layers, not duplication.

---

### Stream parsing (bootstrap) vs line buffering (parser)

**Looks like:** inconsistency in I/O model.
**Why not:** parser.re buffers lines for backtracking and contextual analysis
(LLVM IR rules). bootstrap.re reads a stream for maximum speed.
Different requirements, different solutions.

---

### RA_BS_RESULT save in BS_INTERN_NAMED

**Looks like:** hack (save → call → restore).
**Why not:** BS_PACK_TOKBUF uses ALLOC_LUCES which writes to RA_ALLOC_RESULT,
but BS_INTERN_NAMED already wrote the address to RA_BS_RESULT before the call.
Saving via RA_BS_EL0 is the correct caller-save register pattern around a call.
Not a hack — correct calling convention.

---

### ETH_* shared restore pattern in yaku.re

**Looks like:** many handlers manually save RA_TW_LUX into RA_ETH_TW_SAVED.
**Why not:** This is intentional shared-endpoint architecture. All handlers jump
to a common `ETH_TW_RESTORE` which restores and proceeds to `ET_ADVANCE`.
SAVE macro generates save/restore around each block separately — here one common
restore. This is more efficient than N separate SAVEs.

---

### LOAD_MAS_0..7 in saku.re — MA-slot reading not via FOR

**Looks like:** 7 identical RVOCA+JZ+ITO patterns for MA1..MA7.
**Why not:** Each iteration ends with `RVOCA LOAD_MAS_{N+1}_RD` — calling the
next reader by name. FOR does not support `{N+1}` indexing in a template.
Applying FOR is impossible without restructuring.

---

### NOITO and CHAIN — macros for grouping luces

**NOITO** — list of independent luces (each with its own NOLINK):
```
NOITO
    NAME1 Move El1=foo Exit=bar
        RVOCA X Y
    NAME2 Move El1=baz Exit=qux
```
Expands to: NOLINK + ITO NAME1 + RVOCA X Y + NOLINK + ITO NAME2.

**CHAIN** — one NOLINK, chain of linked luces. Named or anonymous:
```
CHAIN LOAD_FL
    _DONE Move El1=SYS_CLOSE Exit=SC_NR
    Move El1=RA_LOAD_FD Exit=SC_A0
        RVOCA X CS_POP
    _RET Redi El1=RA_LINK
```
Three unified modes for NOITO/CHAIN/SWITCH: anonymous, explicit anonymous (`_`), named.

---

### Wave-B: unknown name warnings for FOR templates

Wave-B emits `Unknown name: 'X{N}'` for names from FOR/CHAIN bodies containing `{N}`.
This is expected: FOR templates expand in Pass-2 (Reca runtime), not Wave-B (Python).
Wave-B sees the literal `{N}` as a name and cannot find it — correct behaviour.
Fixing it would require Wave-B to understand FOR/CHAIN syntax. Not worth the complexity.

---

### CHAIN cannot apply when a block does not start with ITO

Pattern: `RVOCA X CS_PUSH` → `ITO A ...` → `ITO B ...`
Applying CHAIN to ITO A/B adds an extra NOLINK between CS_PUSH and A.
CHAIN only fits blocks where the first line is an entry point.
Blocks starting with RVOCA (call as part of the chain) — do not apply CHAIN.

---

### DJB2 hashing — three inline copies in hot loops

**Looks like:** duplication (3–4 places: BS_RT, BS_TV, LOAD_DL_CO, parser.re).
**Why not:** DJB2 is in the **hot loop** of the tokenizer. Extracting to a
subroutine adds Voca overhead on every iteration — unacceptable.
Each location also differs slightly (different registers for byte, different masks).
Stays inline.

---

### Division by zero returns 0 instead of fault

**Looks like:** silent bug.
**Why:** `_fault_vector` is not yet implemented as a full exception system.
Division by zero → 0 is predictable behaviour.
When fault_vector is implemented — add a jump to it on div/0.

---

## Python Bootstrap

### FUNC hardcoded in Python _expand_indent

**Looks like:** duplication — Python hardcodes `Sub RA_SP + Write RA_SP RA_LINK`
instead of using the FUNC macro from macros.re.
**Why not:** This is the bootstrap layer. macros.re is loaded through the same
Python loader. Removing FUNC from Python would break it before macros.re loads.
Duplication is unavoidable at the bootstrap level. When self-hosting is stable —
the Python layer is removed entirely.

---

### Syscall number caching in interpreter.py

**Looks like:** violation of "aether as single source of truth".
**Why not:** SYS_WRITE and other syscall numbers are Linux ABI constants.
They do not change at runtime. Caching at `update_relations` is a correct
performance trade-off. If syscall virtualisation is ever needed — add a hook
in `update_relations` when aether[SYS_*] changes.

---

## Stable Invariants

These are not canon but should not change without deliberate discussion:

- ITO layout: 7 base luces (word, op, e1, e2, exit, next, pad)
- `BS_HT_MASK = 0x3FFFF` (262144 slots)
- BLOCK base addresses (`_000`) in htable — LOAD_MAIN resolves them correctly
- 24 Aspects in `aspects.re` — adding or removing one redefines Reca

---

### Mobile execution model

Reca's execution model is structurally legal on all mobile platforms.

The interpreter lives in a signed, static binary — RX, never touched at runtime.
The Aether is plain RW memory — from the OS's perspective, just an array of numbers.
No mprotect, no anonymous executable mappings, no JIT spray surface.

When Yaku generates ARM64 natively (M1), the result is a standard signed binary.
No LLVM at runtime, no dynamic code generation, no entitlements required.
The OS sees an interpreter reading numbers. That's all it ever needs to see.
