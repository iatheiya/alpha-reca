# PLAN: Universal lux mutability — `next` at position 1, no `bound`, no sentinel

Status: APPROVED IN PRINCIPLE. Implementation deferred — apply after
repository sync (push current working tree first, then implement in a
dedicated session).

Supersedes: PLAN_BOUND_NEXT.md (rejected — see that file for why `bound`
was dropped).

---

## Core insight

The prepass (`Wave 0` in `saku.re`) already counts exact lumen counts per
name via `LOAD_LCOUNT_GET` / `SK_ITO_LCNT`. Both allocators use:

```
Data lux size = 1 + 2*lcount + (lcount>0 ? 1 : 0)
ITO  lux size = 7 + 2*lcount + (lcount>0 ? 1 : 0)
```

The `+1` at the end is the `OVERFLOW_REL` sentinel slot. Since the prepass
guarantees exact allocation, overflow never happens — confirmed empirically:
`OVERFLOW_REL` is never written anywhere in the frozen aether (0 occurrences
at runtime). The sentinel mechanism is completely dead code. We can remove
the `+1` and use that slot budget for `next` instead.

`next` is needed for **all** lux for mutability — the ability to add lumens
after initial allocation. This is not about whether a lux has lumens now,
but about preserving the option to add them later. Without `next`, you
cannot extend a lux at runtime without reallocating and patching all
references to it.

---

## New layout

### ITO lux (ITO_SIZE: 7 → 6)

```
slot 0: word   (self-referential addr — ITO invariant)
slot 1: next   (0 = no extension, else addr of continuation lux)  ← MOVED from slot 5
slot 2: op     (operation primitive addr)                          ← unchanged
slot 3: e1     (first operand addr)                                ← unchanged
slot 4: e2     (second operand addr)                               ← unchanged
slot 5: exit   (result register addr)                              ← was slot 4 before? NO —
```

Wait — current ITO slots per symphony.re:
```
slot 0: word
slot 1: op
slot 2: e1
slot 3: e2
slot 4: exit
slot 5: next   (currently here — MOVE to slot 1)
slot 6: pad    (the "lishny slot" — REMOVE)
```

New ITO layout:
```
slot 0: word
slot 1: next   ← moved here from slot 5
slot 2: op     ← shifted (was slot 1)
slot 3: e1     ← shifted (was slot 2)
slot 4: e2     ← shifted (was slot 3)
slot 5: exit   ← shifted (was slot 4)
```

ITO_SIZE = 6 (was 7). Removing `pad` (slot 6) and moving `next` to slot 1.

Lumen pairs (if any) follow at slots 6, 7, 8, 9, ... (was 7, 8, 9, 10, ...)

### Data lux

```
slot 0: word   (the actual value / lux id)
slot 1: next   ← NEW (was: no next slot at all)
slot 2+: lumen pairs [rel, exit, rel, exit, ...]
```

Data lux size = 2 + 2*lcount (was: 1 + 2*lcount + (lcount>0 ? 1 : 0))

For lcount=0: size 2 (was 1) → +1 slot per no-lumen Data lux
For lcount=1: size 4 (was 4) → identical
For lcount=N: size 2+2N (was 2+2N) → identical

---

## Memory impact

From empirical measurement of the current frozen binary (~55K live lux):

- ITO lux: save 1 slot each (removing pad, net effect of next relocation)
  - ~24K ITO lux (estimate) → save ~24K slots
- Data lux with lumen (~54% of data lux, ~3.5K): no change
- Data lux without lumen (~46% of data lux, ~3.1K): +1 slot each → +3.1K slots

Net: meaningful savings. The no-lumen Data overhead (+3.1K) is dominated
by ITO savings (~24K). Total aether footprint shrinks.

---

## What changes in code

### symphony.re / symphony.py — slot constants

```
SLOT_NEXT = 1   (was 5)
SLOT_OP   = 2   (was 1)
SLOT_E1   = 3   (was 2)
SLOT_E2   = 4   (was 3)
SLOT_EXIT = 5   (was 4)
ITO_SIZE  = 6   (was 7)
```

This is the single most impactful change — symphony.py exports these
constants and they are imported everywhere. Changing them here cascades
correctly to all Python consumers.

symphony.re defines the same constants as Reca NEWSETs — update to match.

### saku.re — allocators

Remove the `+1` sentinel from both allocator formulas:

```
/Old:  1 + 2*lcount + (lcount>0 ? 1 : 0)
/New:  2 + 2*lcount
LOAD_CMD_NEW:  RA_ALLOC_COUNT = 2 + 2*lcount

/Old:  ITO_SIZE + 2*lcount + (lcount>0 ? 1 : 0)
/New:  ITO_SIZE + 2*lcount   (ITO_SIZE is now 6)
LOAD_CMD_ITO:  RA_ALLOC_COUNT = ITO_SIZE + 2*lcount
```

The `JZ LOAD_CI_TCKZ / LOAD_CN_TCKZ` branches (the `lcount>0 ? +1 : 0`
sentinel logic) are removed entirely.

### alloc.re — remove OVERFLOW_REL entirely

- Remove `NEW OVERFLOW_REL`
- Remove `ALLOC_LUX_N` (allocator that writes the sentinel — now dead)
- Remove the sentinel-write in `ANN_OVWR` path
- Remove `OVERFLOW_REL` checks in `ADD_LUMEN` (`AL_OVCK`/`AL_OVERFLOW`)
- Remove `OVERFLOW_REL` checks in `REMOVE_LUMEN` (`RL_OVCK`/`RL_SHOVCK`)

### symphony.re — SR_* lumen walkers

Remove `OVERFLOW_REL` checks from:
- `SR_FIRST_LM` / `SR_NEXT_LM` (SR_FL_OVCK, SR_NL_OVCK)
- `SR_GLX` (SR_GLX_OVCK)
- `SR_COUNT_LM` (SC_OVCK)

These walkers now simply scan consecutive pairs until rel==0. Clean.

### accord.re — SCAN_LUMEN_OF

The "was LX" inline advance can remain as-is (it never used OVERFLOW_REL
handling anyway — see comment added this session). The NOTE about
OVERFLOW_REL in that comment can be removed once this plan is implemented.

### loader.py — Python-side lux building

All Python code that builds ITO lux (Wave-B wiring, NEWREF restoration,
etc.) uses the symphony.py slot constants — these update automatically when
symphony.py is updated. Double-check any hardcoded `+ 5` (old SLOT_NEXT)
or `+ 1` (old SLOT_OP) patterns.

### interpreter.py

Uses symphony.py constants via `_slot()` helper — updates automatically.
Check `_run` loop for any hardcoded slot numbers.

### diag.py / trace.py / check_ito_sizing.py

Use symphony.py constants — updates automatically. `check_ito_sizing.py`
specifically validates ITO_SIZE: update its expected value to 6.

---

## What does NOT change

- `LR`, `LT`, `LH` macros: these access lumen pairs at `ITO_SIZE + offset`
  — ITO_SIZE changes from 7 to 6, so the offset changes too. But the
  *formula* (`ITO_SIZE + 2*N`) stays the same, so if these macros use
  `ITO_SIZE` symbolically (not hardcoded 7), they update automatically.
  Check each one.
- `WALK_ONE` / `WALK_ITO`: same as above — use `ITO_SIZE`/`C_7` for
  offset. `C_7` is currently used as a proxy for ITO_SIZE in WALK_ITO
  (`El1=C_7_REF` in macros.re:241). After this change, ITO_SIZE=6, so
  C_7 is wrong. WALK_ITO needs `C_6` or a new `ITO_SIZE_REF`. This is
  one place that needs careful auditing.
- `LINK_OP` / `UNLINK_OP`: call `ADD_LUMEN` / `REMOVE_LUMEN` which are
  updated above — these call sites don't change.
- All `.re` user-facing code (`fmt.re`, `io.re`, etc.): they don't touch
  slot offsets directly.

---

## Implementation order (when the time comes)

1. Update `symphony.py` constants (SLOT_NEXT, SLOT_OP, SLOT_E1, SLOT_E2,
   SLOT_EXIT, ITO_SIZE). This is the single source of truth.
2. Update `symphony.re` NEWSETs to match.
3. Update `saku.re` allocator formulas (remove sentinel branch).
4. Update `alloc.re` (remove OVERFLOW_REL, ALLOC_LUX_N, overflow paths).
5. Update `symphony.re` lumen walkers (remove OVERFLOW_REL checks).
6. Audit `WALK_ITO` / `C_7_REF` usage — fix to use ITO_SIZE (now 6).
7. Run `python3 -c "from loader import freeze; freeze()"` — expect
   many issues first time, fix systematically.
8. Run full diagnostic suite: `diag.py --lint`, `--invariants`,
   `--block-lint`, `--unwrapped`, `test_macros.py`, `check_ito_sizing.py`.
9. Self-hosting should stay at 40/42 or improve (the open IndexError
   in ALLOC_LUCES may also resolve, since the sentinel slot removal
   changes allocation addresses — worth checking first before deep-diving
   that bug again).

---

## Open questions (resolve before implementing)

1. **`WALK_ITO` and `C_7_REF`**: currently hardcodes `C_7` as ITO_SIZE.
   After this change, ITO_SIZE=6. Need `C_6_REF` or symbolic `ITO_SIZE_REF`.
   The `C_6` constant already exists in `constants.re` — just need the REF.

2. **`preamble_template.ll` and `gen_compiler.py`**: generated LLVM IR uses
   ITO layout. Check if slot offsets are hardcoded in the codegen.

3. **The open `IndexError` in `ALLOC_LUCES`**: may or may not be related to
   the sentinel `+1` slot. Worth re-testing after this change before
   resuming the deep trace from the previous session.
