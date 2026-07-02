# HISTORY.md — Historical Decisions

Records decisions that were made, why they were made, and why some were reversed.
Does not duplicate current documentation — only what is behind us or fixed as "do not do".

---

## Renames

### compiler.re → saku.re → yaku.re
The file was first renamed from `compiler.re` (too conventional) to `saku.re`
(the Saku concept — compilation rules). Then renamed again to `yaku.re` because
Yaku is the actual system name used throughout the codebase (`Yaku_X` rules,
`YAKU_NEXO` macros, etc.). Current filename: `yaku.re`.

### loader.re → saku.re
The self-hosted loader was named `loader.re` (conventional). Renamed to `saku.re`
following the same logic as yaku.re: Saku is the name of the Aria (the loader),
not a replacement for the words load/loader/loaded. Analogy: Yaku is the compiler
Aria, not a replacement for compile/compiler/compiled.

### exhale.re → shioreru.re
The exit report file was named `exhale.re` (inhale/exhale metaphor, now retired).
Renamed to `shioreru.re` — the conceptual counterpart to Saku: what blooms must
also wilt. Inhale/exhale/bloom/withering terminology retired in favour of
the cleaner Saku/Shioreru pair.

### Phase/Pass/Sweep → Wave
"Wave" describes a pass through the graph more naturally than "phase" or "pass".

### Target / Dest → Exit
`Target` (where we write the result) and `Dest` (jump destination) are the same concept:
"where to\". `Exit` expresses this without splitting. `yaku.re` itself already noted
`unified: DEST_N == TGT` — acknowledging they were identical.

### tgt → exit in lumen
The lumen structure was described as `(rel, tgt)`. Symbols: `RA_LM_TGT`, `SR_LM_TGT`, `SR_GLT`.
Now: `(rel, exit)`. Symbols: `RA_LM_EXIT`, `SR_LM_EXIT`, `SR_GLE`.
A lumen exit is "where the connection leads" — the same concept as exit in ITO.
One term for one concept.

### OPUS_* → ARIA_* (in aria.re)
Symbols renamed to match the concept. File was renamed to `aria.re` but symbols stayed `OPUS_*` — now corrected.

### ITO — etymology
Previously called `INSTR` (instruction). Renamed because "step" expresses the
essence more precisely: ITO is one step of execution.

---

## Removed Concepts

### Rel-slots in ITO (Op_rel, El1_rel, El2_rel)
Dead data — nobody read them. Removed. ITO layout became cleaner.

### CreateLux / Link / Unlink / LumenHead / LumenRel / LumenExit / LumenNext
Removed as primitives. Replaced by `ALLOC_LUX`, `ADD_LUMEN`, `REMOVE_LUMEN`,
`SR_GLL`, `SR_GLR`, `SR_GLE`, `SR_GLX` via macros.

### Trace / ExecutedBy
Removed as concepts. Trace is now a pure observer: the interpreter writes
`word(pc)` into the trace buffer when `K_TRACE_POS != 0`. No special primitive.

### CompareNe
Removed — the JNE/JNZ macros that generated it are gone.

### SCC / Tarjan (Python)
Python previously used Tarjan's algorithm to determine file processing order
(strongly connected components). Replaced by a two-phase pass (Wave-A allocation →
Wave-B wiring), which eliminates the need for file ordering. Forward references
are resolved in Wave-B.

---

*The sections "Will Not Be Implemented" and "Fixed" were moved to `DECISIONS.md` where they belong — decisions about what not to do, and records of completed architectural cleanups.*
