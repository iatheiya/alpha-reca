# GLOSSARY.md — Reca Terminology

---

## The Model

**Aether** *(Greek: αἰθήρ — the upper sky, the pure air of the heavens)*
The flat array of machine words that is Reca's entire address space.
Every value, every instruction, every connection lives in the Aether.
There is no separate heap, stack, or code segment — only Aether.

**Lux** *(Latin: light — pl. luces)*
One slot in Aether. One machine word. The atom of Reca.
A Lux has no intrinsic type or meaning — what it represents is decided by whoever reads it.
An address, an integer, an opcode, a character — all are luces.

**Lumen** *(Latin: a beam of light, a flow — pl. lumens)*
A directed connection from one Lux to another.
In the current Aria: stored as a `(rel, exit)` pair inline after a Lux's word.
The lumen concept is canon; its physical layout is not.

---

## Programs and Structure

**Aria** *(Italian/English: a self-contained piece for one voice)*
Any program, library, or convention built on top of the Aether.
An Aria interprets luces — it assigns meaning, defines structure, imposes behaviour.
The Canon Aria (the `.re` files in this repository) is one possible Aria.
Another Aria could read the same Aether and see something entirely different.

**Symphony** *(from the graph traversal library `symphony.re`)*
The graph — the full Aether viewed as a connected structure.
Consonant with Aria: many Arias together form a Symphony.

**Accord** *(English: agreement, convention)*
A convention that an Aria can adopt. If an Aria links itself to an `ACCORD_RULE`,
it signals that it follows that convention. At Shioreru (exit), ACCORD_CHECK verifies
adopted accords and reports the count.

---

## Instructions

**ITO** *(Latin: ito — "go", "step"; imperative of ire)*
An instruction Lux. One step of execution.
Stored as: `[word, op, e1, e2, exit, next, pad, lumens...]`.
The `op` field holds the address of an Aspect Lux, which determines what the ITO does.

**Aspect**
One of the 24 irreducible operations — the portable image of the hardware.
Each Aspect maps directly to a CPU instruction or OS call.
They are the only things in Reca that have fixed, built-in meaning.

**Voca** *(Latin: voca — "call"; imperative of vocare)*
The call Aspect. Pushes the old `RA_LINK` onto the automatic call stack
(`RA_SP`), saves the next instruction address into `RA_LINK`, then jumps to `e1`.

**Redi** *(Latin: redi — "return"; imperative of redire)*
The return Aspect. Jumps to the address stored in `aether[e1]`, then pops
`RA_LINK` from the automatic call stack (`RA_SP`).

**Exire** *(Latin: exire — "to exit", "to go out")*
The system call Aspect. Passes control to the OS or host via the SC_* slot convention.

---

## Compilation

**Yaku** *(Japanese: 訳 — translation, rendering)*
The compiler Aria. `yaku.re` maps each Aspect to an LLVM IR template.
A Lux with `LINK X Entry Yaku` marks an entry point for compilation.
The name reflects the idea of one form being rendered into another.
Not to be confused with `gen_compiler.py`, an unrelated Python codegen
script that pre-generates XLEN-dependent constants and layout files —
it has no connection to the Yaku Aria.

---

## Lifecycle

**Saku** *(Japanese: 咲く — to bloom, to flower; as in sakura)*
The loader — reads source files, allocates luces, wires connections.
`saku.re` implements the self-hosted loader. The words load/loader/loaded
are not replaced; Saku is the name of the Aria, not the concept.
After saku, the graph is one graph: no record remains of which file a Lumen came from.

**Shioreru** *(Japanese: 萎れる — to wither, to wilt)*
The teardown Aria — emits the execution summary (trace status, Accord count) at exit.
`shioreru.re` is the conceptual counterpart to Saku: what blooms must also wilt.
The words exit/report/teardown are not replaced; Shioreru is the name of the Aria.

---

## Observation

**Iris** *(Greek: Ἶρις — the iris of the eye; regulates how much light passes through)*
The execution trace observer. `iris.re` is a pure observer — it never modifies
the instruction graph. When `K_TRACE_POS` is nonzero, the interpreter writes
`word(pc)` into the trace buffer on every step.

**Phantom ITO**
A Lux that was declared but never had its `op` slot wired.
`op=0` → the interpreter halts on it. A common source of bugs.

---

## Execution paths

The three ways `next_pc` is resolved after each Lux:

**Line**
`SLOT_NEXT == 0` → `next_pc = pc + ITO_SIZE`.
Sequential execution: no read, no branch, no overhead. The loader leaves
`SLOT_NEXT` at zero for consecutive ITOs. The CPU prefetcher sees a linear
stream and loads ahead automatically. The dominant path in hot code.

**Warp**
`0 < SLOT_NEXT < FLUX_BOTTOM` → `next_pc = aether[SLOT_NEXT]`.
Explicit jump to a non-adjacent Lux. Used by `Jump`, `JumpIf` (true branch),
`Voca`, `Redi`. One extra memory read, then execution continues linearly from
the new address.

**Flux**
`SLOT_NEXT >= FLUX_BOTTOM` → `_exec_flux(SLOT_NEXT)`.
Dispatches through a structured flux Lux that can carry multiple `Next` targets.
Foundation for future parallel fan-out via `Exire(clone)`. Rare in current code.
