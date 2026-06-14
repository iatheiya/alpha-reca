# CANON.md — The Canon of Reca

What is **fundamental** — the parts that cannot change without redefining Reca.
Everything else is an aria.

---

## Principle 1 — Zero Is Absence

```
aether[i] == 0  →  nothing here
aether[i] != 0  →  something is here; meaning is decided by an Aria
```

Python, LLVM, and the runtime know only this. They never assume that a lux holds
a cursor, a watermark, a return address, or a trace position. Those are Aria
conventions in `.re` files, resolved through symbols.

---

## Principle 2 — The 24 Aspects Are Hardware ABI

The 24 entries of `aspects.re` are the portable image of the silicon: ALU
operations, loads/stores, branches, syscalls. Each maps to one or a small fixed
set of CPU instructions with no smaller decomposition in the Reca model.

The 24 are stable. Adding a 25th or removing one redefines Reca, not an aria.

Relations (`Op`, `Next`, `El1`, `El2`, `Exit`, `Entry`, `Yaku`) are
Aria conventions — how one Aria (the bootstrap interpreter) chooses to interpret
Lumen structure. A different Aria may interpret it differently.

Full table: **LANGUAGE.md § The 24 Aspects**.

---

## Principle 3 — Lux and Lumen

```
Lux  = u64                — a value. Any u64: an address, an op id, a number.
Lumen = addr              — a pointer. An address in Aether pointing to another Lux.
```

**Lux** is the atom of information. It is just a u64 — no structure imposed.
What a Lux means (an op id, a relation, a return address, a constant) is decided
by the reader, not by the canon.

**Lumen** is a connection — an address that points to a Lux. A Lumen is not a
pair of luces; it is simply an addr. The Aether layout of how Lumens are stored
alongside a Lux (inline, chained, elsewhere) is an Aria decision, not canon.

The minimum that can form a graph: a Lux that carries a value, and a Lumen that
connects it to another Lux. Everything else — lux layout, lumen chains,
termination conventions — is an Aria convention.

---

## Principle 4 — SETREF Invariant (Aspects only)

For every Aspect Lux X: `aether[X.addr] == X.addr`. Each Aspect self-identifies.

This enables dispatch: the interpreter reads the lux value at a Lumen and
recognises which of the 24 operations to execute — no separate mapping needed.

This invariant **must hold** for the 24 Aspects. Other Lux may use their word for anything.

---

## Principle 5 — Existence Through Connectivity

A Lux exists as long as something links to it. No allocator saying "this lux is
owned". Deactivation is implicit: remove the last incoming link and the lux becomes
invisible. This is GC by topology, not by sweep.

Tracing GC, reference counting, and arena clearing are all Arias — optional, not canon.

---

## Principle 6 — One Aether, Many Strategies

The Aether is a flat u64 array. One address space. Within it, an Aria may carve
arenas, choose addresses by hash, seek zeroed luces, or maintain a bump cursor.
These are strategies, not structures. The canon picks only the invariant
(`aether[0] = 0`) and lets Arias do the rest.

---

## Principle 7 — The Aria Is the Graph

No modules, namespaces, or imports. Loading an Aria adds its Lumen to the Aether.
After saku there is no record of where a Lumen came from — the graph is one graph.

The bootstrap loader (`loader.py`) exists only to bring the Aether into being from
text. After self-hosting it is replaced by `parser.re`.

---

## Principle 8 — Exit Is "Where To", Not "What For"

`Exit` is the single concept for "where does this go next". It applies at every
level: ITO slot 4 (control flow), lumen pair field (graph lumen target). The
distinction between "destination", "target", and "exit" is a reader convention,
not a canon distinction. `Target` and `Dest` as separate relations do not exist
in canon — they are aliases for `Exit` that have been retired.

---

## Principle 9 — Lumen Structure Is an Aria Decision

The canon defines: a lumen is an address connecting one Lux to another.
The current `(rel, exit)` pair layout in `symphony.re` is an Aria convention —
optimal for the current implementation but not fixed by canon.

**Decided (not canon):** `rel` remains a required field in the `(rel, exit)` pair
layout. Removing `rel` would force positional semantics which is equivalent
complexity with less clarity. A lumen without `rel` is indistinguishable from
a plain `exit` at the ITO level. The current structure is kept.

**Open (future Aria):** a variadic element macro that extends ITO with N inputs
and M exits, expressed as a higher-level graph pattern. This is not a change to
the ITO layout — ITO stays 7 slots. The macro compiles to a chain of ITOs.

---

## Principle 10 — Parallelism Is an Aria, Not Native

Reca's graph structure naturally expresses parallelism: N exits from one Lux
means N independent continuations. The native interpreter executes sequentially.

**Decided:** dataflow execution (token-passing, firing rules, parallel scheduling)
is implemented as an Aria, not baked into the native interpreter. The native
interpreter remains: execute one lux, follow exit. Overhead appears only where
parallelism is chosen, not universally.

A dataflow Aria would sit above the native interpreter and schedule Lux execution
based on token availability. The native ITO protocol is unchanged underneath.

---

## What Is Not Canon

The following are Aria conventions of the current implementation — they can change:

- Relation IDs (`Op`, `Next`, `El1`, `El2`, `Exit`) — in `relations.re`
- The compiler marker (`Yaku`) — in `relations.re`, used by `yaku.re`
- The 7-slot ITO layout and slot offsets (SLOT_*)
- The lumen pair layout `[rel, exit]` and 0-termination
- Auto-Next behaviour of sequential ITO chains
- Byte-chain string encoding
- Aether size (`K_AETHER_SIZE`)
