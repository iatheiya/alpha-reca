# LANGUAGE.md — Reca Syntax and Semantics

## The Model

Everything in Reca is a **Lux** in a flat **Aether** (address space).
Each Lux holds exactly one integer word (XLEN bits — 64 on ARM64).
Lux are connected by directed **Lumen**: `src --relation--> exit`.

No types. No stacks. No scopes. No separate heap. No GC.

---

## The .re Bootstrap Format

`.re` files are the bootstrap text format — the only syntax Python understands.
After self-hosting, syntax is a Reca Aria (`parser.re`).

### Commands

**`NEW Name`** — create a Lux (word = 0). Idempotent.

**`NEWREF Name`** — create a Lux with `word = own ID` (self-referential). Required
for identity luces (Aspects, relations) so that `Equal(lumen_rel, El1)` works correctly.

**`NEWSET Name value_or_string`** — matryoshka of `NEW Name` + `SET Name value`. Convenient
single-line form for declaring and initialising a Lux in one step.

**`SET Name integer`** — set a Lux's word to a literal integer.

**`SET Name "string"`** — build a null-terminated byte chain: 8 bytes packed per lux
(little-endian u64), one raw 2-lux slot per chunk, NUL-terminated. `Name.word` = address
of the first chunk. Escape sequences: `\n` newline, `\t` tab, `\\` backslash.

Note: strings are "dead cargo" — passed to syscalls (SYS_WRITE, SYS_OPENAT) or scanned
sequentially. The 8-bytes-per-lux packing is intentional: it minimises Aether pressure
and avoids cache misses from single-byte luces. Fat pointers are not needed because Reca
code never addresses the middle of a string by absolute lux index — it always reads
sequentially from the start via the linked chunk chain.

**`SETREF Target Source`** — set `Target.word = Source's Lux ID`.

**`LINK src rel exit`** — add a directed Lumen: `src --rel--> exit`.

**`BLOCK Name count`** — allocate `count` consecutive Lux. `Name` = first ID.
Used for buffers and arrays.

**`YAKU_NEXO Aspect body`** — create `Yaku_Aspect` + `ForType` lumen + RO-graph body.
Body is the rest of the line (no quotes). Multi-line form with indentation:

    YAKU_NEXO Add
      {result} = add i{XLEN} {v1}, {v2}
      store i{XLEN} {result}, ptr {ptr_tgt}

Indented lines are synthesised as `NEXO Aspect body` automatically by the loader.

**`YAKU_NEXO_TERM Aspect body`** — same as YAKU_NEXO + `Terminates` lumen to Yaku.

**`YAKU_NEXO_ALIAS Aspect Alias body`** — same as YAKU_NEXO_TERM + second `ForType` lumen to Alias.

**`NEXO Aspect body`** — append one RO-graph body line to an existing `Yaku_Aspect`.

**`ITO Name Op [El1=X] [El2=Y] [Exit=Z]`** — create an instruction Lux.
Auto-links sequential ITO luces via `Next` (slot 5) unless preceded by `NOLINK`.

**`NOLINK`** — break the auto-Next chain. The following ITO gets no Next from the previous.

---

## Execution Model

```
pc = entry_lux
while pc != 0:
    op  = aether[pc + SLOT_OP]      // which Aspect
    e1  = aether[pc + SLOT_E1]      // first operand address
    e2  = aether[pc + SLOT_E2]      // second operand address
    exit= aether[pc + SLOT_EXIT]    // result / jump / link target
    execute(op, e1, e2, exit)
    raw = aether[pc + SLOT_NEXT]
    // Three paths (step / warp / flux):
    if raw == 0:            pc = pc + ITO_SIZE   // step: sequential fall-through
    elif raw < FLUX_BOTTOM: pc = raw             // warp: explicit address jump
    else:                   pc = exec_flux(raw)  // flux: structured parallel branch
```

ITO lux layout (7 base slots + optional extra LINK lumens):
```
slot 0: word  = self-ref (aether[addr] = addr)
slot 1: op    = Aspect Lux address
slot 2: e1    = first operand
slot 3: e2    = second operand
slot 4: exit  = result destination / jump target / RA_LINK addr for Voca
slot 5: next  = 0 → step (pc + ITO_SIZE); non-zero → warp or flux target
slot 6: pad   = 0  (terminates lumen scan before extra lumens)
slot 7+:       extra LINK lumens: (rel, exit) pairs, 0-terminated
```

Data lux layout:
```
slot 0: word  = value
slot 1+:       lumen pairs (rel_addr, tgt_addr), 0-terminated
```

Distinction: ITO has `op != 0` (slot 1); data lux has `op == 0`.

---

## The 24 Aspects

The 24 entries of `aspects.re` are the maximum universal set — operations natively
available on the vast majority of target CPUs. Each maps to one CPU instruction or
a fixed OS escape. IDs are stable for the current Aria.

### Memory (1–2)

| Name | Semantics |
|---|---|
| `Read` | `exit.word ← aether[e1.word]` |
| `Write` | `aether[e1.word] ← e2.word` |

### Arithmetic (3–9)

| Name | LLVM | Semantics |
|---|---|---|
| `Add` | `add i{X}` | addition mod 2^X |
| `Sub` | `sub i{X}` | subtraction |
| `Mul` | `mul i{X}` | multiplication (lower X bits) |
| `Div` | `sdiv i{X}` | **signed** division |
| `Rem` | `srem i{X}` | **signed** remainder |
| `UDiv` | `udiv i{X}` | **unsigned** division |
| `URem` | `urem i{X}` | **unsigned** remainder |

### Bitwise (10–15)

| Name | LLVM | Semantics |
|---|---|---|
| `And` | `and i{X}` | bitwise AND |
| `Or` | `or i{X}` | bitwise OR |
| `Xor` | `xor i{X}` | bitwise XOR |
| `Left` | `shl i{X}` | shift left |
| `Right` | `lshr i{X}` | **logical** right shift (zero-fills MSB) |
| `ARight` | `ashr i{X}` | **arithmetic** right shift (sign-extends MSB) |

### Comparison (16–18)

| Name | LLVM | Semantics |
|---|---|---|
| `Equal` | `icmp eq` | equality → 0 or 1 |
| `Less` | `icmp slt` | **signed** less-than → 0 or 1 |
| `ULess` | `icmp ult` | **unsigned** less-than → 0 or 1 |

### Control flow (19–20)

| Name | Semantics |
|---|---|
| `JumpIf` | `if e1.word != 0: pc ← exit.word` |
| `JumpReg` | `pc ← e1.word` (indirect jump) |

### System (21–22)

| Name | Semantics |
|---|---|
| `End` | terminates execution |
| `Exire` | OS/host call via SC_NR/SC_A0..SC_A3 (see `registers.re`) |

### Procedure (23–24)

| Name | Semantics |
|---|---|
| `Voca` | pushes old `RA_LINK` onto the automatic call stack (`RA_SP`), then `aether[exit] ← next_instr_id; pc ← e1.word` |
| `Redi` | `pc ← aether[e1.word]`, then pops `RA_LINK` from the automatic call stack (`RA_SP`) |

`Voca` + `Redi` are the calling convention: Voca saves the return address into
`RA_LINK` (pushing the old value first), Redi jumps to it (popping afterward).
The call stack (`RA_SP`) makes this correct at any nesting depth automatically —
no per-function save/restore needed. See `aria/regs.re` for the full
description.

### Relations (`relations.re`) — interpreter conventions, not hardware

`Op`, `Next`, `El1`, `El2`, `Exit`, `Entry`, `Yaku` — used by the
loader and interpreter to wire and traverse instruction Lux. Stable IDs at load
time, but not Aspects: a different Aria may interpret Lumen structure differently.

### Derived ops (`derived.re`) — expressible from Aspects

`Not`, `Greater`, `Jump`, `Move`, `Load`, `Store` — each reducible to 1–3 Aspect
calls. Exist for expressiveness; compiler has dedicated `Yaku_*` templates for them.

---

## Same Value, Different Contexts

In Reca, two Lux from different semantic worlds can have the same numeric word —
this is a feature, not a conflict. The Aether is a flat array of u64; the *meaning*
of a value is always decided by the Aria reading it.

Example: `C_48` (a shift amount of 48 bits) and `ASCII_0` (the character `'0'`,
ASCII code 48) are both `word = 48`. They live in different conceptual worlds —
arithmetic constants vs. character codes — and should be used by their *semantic*
name in each context. The numeric coincidence is irrelevant; what matters is intent.

This principle applies generally: different Aria can assign different meanings to
the same u64 value. Reca does not enforce a single interpretation. An Aria that
reads a lux as a colour, a timestamp, and an instruction address simultaneously is
working correctly as long as each reader knows which convention applies.

---

## Memory Layout

```
aether[0]          = 0  (NULL sentinel — zero is absence)
aether[1]          = K_CURSOR    current bump position
aether[2]          = K_WATERMARK high-water mark
aether[3]          = K_TRACE_POS trace write pointer (0 = off)
aether[4..N]       = Lux allocated by loader and runtime
```

Allocation is a bump pointer (`K_CURSOR`). Each `NEW`/`BLOCK`/`ITO` advances it.
No free list. Deactivation = losing connectivity (topology-based GC).

---

## Entry Points

A Lux is a compiler entry point when it has:
```
LINK MyFunc Entry Yaku
```
The compiler (P0_COLLECT) finds all such Lux and emits one LLVM function per entry.
`PS_MAIN` (parser) and `P0_NID` (compiler itself) are the two current entry points.

---

## Connectivity and Deactivation

A Lux exists as long as something links to it. Deactivation is implicit: remove
the last incoming link and the Lux becomes invisible — GC by topology, not by sweep.
Any explicit memory strategy (tracing GC, reference counting, arenas) is built on top.
