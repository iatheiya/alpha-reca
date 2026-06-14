# BUGS.md — Bug Patterns and Debug Lessons

Records concrete bugs found during self-hosting debugging and the patterns behind them.
The main value is not the bugs themselves — it is the patterns they reveal.

---

## Context

**Goal:** LOAD_MAIN pass2 = all files loaded by Reca itself.
**Result:** most files pass; LOAD_MAIN stalls while processing saku.re.
**Why:** RA_LOAD_BYTE = LF before reading MA1 in LOAD_MA_READARG — root cause not
established before end of session.

---

## Pattern 1: Phantom Labels (phantom ITO)

### What happened
Several names in `.re` files were used as LINK/EXIT targets but had no corresponding
ITO definition. The lux stayed with op=0 → terminate on execution.

### Specific instances
- `LOAD_CU_SKIP` → correct: `LOAD_CU_SKPL`
- `LOAD_CB_SKL` → correct: `LOAD_CB_SKLS`
- `LOAD_RB_LOOP` → correct: `LOAD_RB_RL`
- Malformed: `LOAD_RB_LC_JMP2LOAD_RB_` → correct: `LOAD_RB_LC_JMP2RL`
- 1000+ comment lines without `/` prefix → read as commands

### Lesson
**A name in EXIT/LINK must exactly match the ITO definition.** A one-character typo
creates a phantom ITO that is invisible when reading the code but kills execution.
Static linter needed: for every EXIT addr, check that `aether[addr+1] != 0`.

### Diagnostics
```python
# Quick way to find phantom labels:
zero_ops = [addr for addr in ito_addrs if aether[addr+1] == 0]
```

---

## Pattern 2: Name Conflicts Between Files

### What happened
Identical names in different `.re` files → one lux overwrites the other on intern.

### Specific instances
- `AC_WRCUR` in `alloc.re:67` conflicted with `AC_WRCUR` in `arena.re:50`
  → arena.re renamed to `ACL_WRCUR`
- `SC_LOOP`, `SC_INC`, `SC_DONE`, `SC_RET` in `comments.re` conflicted
  with identical names in other files → renamed to `CMT_LOOP/CMT_INC/CMT_DONE/CMT_RET`

### Lesson
**Local labels must be prefixed with the file or module name.**
`LOAD_*` for saku.re, `CMT_*` for comments.re, `ACL_*` for arena.re, etc.
Global names (accessible from other files via LINK) — no prefix.

---

## Pattern 3: JZ Builder Writes to a System Lux Address

### What happened
JZ macro builder allocated an ITO (via ALLOC_LUCES) and received address 328.
By coincidence: `BS_FILE_COUNT` lives at address 328 → its word = 45 (file count)
was overwritten by the ITO value.

### Lesson
**System luces (BS_FILE_COUNT, K_CURSOR, etc.) live in the range ~80–400.**
The bump allocator can reach them under certain conditions. Protection needed:
- Guard in ALLOC_LUCES: if `new_bump < SYSTEM_RESERVED_TOP` → halt
- Or: system luces should live above the initial bump

### Fix
In `loader.py`: after LOAD_MAIN, restore `a[bs_file_count] = len(all_files)`.

---

## Pattern 4: SETREF Dispatch Bug (SET vs SETREF)

### What happened
In `saku.re`, LOAD_DISPATCH_LINE mapped 'S' + second_byte='E' → LOAD_CSS_SETCK.
LOAD_CSS_SETCK checked tlen==3 for SET, but if tlen!=3 — fell into LOAD_CMD_UNKNOWN.
For SETREF (tlen=6) this gave the correct path but the path was hidden.

Added an explicit NOLINK path: `LOAD_CSS_SEUCMD` for tlen != 3, which correctly
delegates to LOAD_CMD_UNKNOWN.

### Lesson
**Every branch in dispatch must explicitly handle all cases.**
The missing fallthrough path for "non-SET SE-tokens" created undefined behaviour.

---

## Pattern 5: K_CURSOR / K_WATERMARK Corruption

### What happened
LOAD_MAIN uses K_CURSOR to track the current bump allocator position.
Under certain conditions something overwrote K_CURSOR.word, setting aether[K_CURSOR]
to an incorrect value.

Symptom: after LOAD_MAIN, K_CURSOR.word ≠ expected → next ALLOC_LUCES returns
an address in already-occupied space.

### Temporary fix
In `loader.py` after LOAD_MAIN:
```python
a[k_cursor] = k_cursor + 1
a[k_cursor + 1] = loader._bump  # restore bump position
```

### Lesson
Root cause not found. This is a workaround, not a cure.

---

## Pattern 6: Write Protection as a Hack on Top of a Hack

### What happened
LOAD_MAIN wrote a RVOCA addr (764) into NEWREF.word, then 3000 (RA_LINK addr).
This broke dispatch: NEWREF.word = data lux → Voca → op=0 → terminate.

Attempted fix: add runtime write-protection in `loader.py`, intercepting all
`_write` operations and blocking "suspicious" writes to command luces.

### Why it did not work
Protection was either too aggressive (blocked legitimate writes) or too weak
(missed other corruption paths). Every tightening of protection opened a new
corruption vector.

**This is the "symptom → hack → new symptom" pattern.**

### Lesson
**Write protection in Python is diagnostics, not a fix.**
If LOAD_MAIN writes garbage into system luces — the bug is in LOAD_MAIN itself.
Find the source of the bad write in the Reca code and fix it there.

**Correct approach:** find which line in which file triggers a write via
LOAD_CMD_SET_IMPL with name=NEWREF, and understand why that line is processed
as SET instead of NEWREF.

---

## Pattern 7: Tracking Error — Wrong Phase Trigger

### What happened
During diagnostics, `oc[0] == 71` was used to detect when `saku.re` opened.
In reality, saku.re opens at `oc[0] == 72` (linux_generic.re = 71).
All tokens analysed as "from saku.re" were from linux_generic.re.

### Lesson
**Diagnostics must capture the file name via openat, not assume a counter.**
Always: on openat → decode path from Aether → `current_file[0] = fname`.

---

## Pattern 8: LOAD_MA_READARG and RA_LOAD_BYTE (open)

### What is known
LOAD_MA_READARG checks `RA_LOAD_BYTE == LF (10)` before reading MA1.
If LF — returns 0 without reading. Meaning: if the previous read left
RA_LOAD_BYTE = 10, MA1 will not be read.

**Normal:** after BS_READ_TOKEN for the MA0 token, RA_LOAD_BYTE = delimiter (space=32, not LF).
**In practice:** RA_LOAD_BYTE = LF before the MA1 check.

### Hypotheses (unverified)
1. Some intermediate Voca calls BS_READ_BYTE and reads LF
2. BS_READ_TOKEN for MA0 reads up to LF (not space) — tokeniser bug
3. RA_LOAD_BYTE is overwritten by something between MA0 and MA1 reads

### Next step
Add a tracker that prints on every LOAD_MA_PRELF trigger:
- Call stack (last 5 Voca/Redi)
- Current RA_LOAD_BYTE
- Current file position (via LOAD_RPOS or equivalent)
- Last read token (tokbuf + tlen)

---

## Root Cause Chain (hypothesis)

1. LOAD_MAIN processes saku.re
2. LOAD_MA_READARG for some NEWREF command returns MA1=0 (even though the ref argument exists)
3. NEWREF handler works only with MA0 (name) — ref is skipped
4. LOAD_DISPATCH_LINE reads the ref argument as the next command
5. That "command" (e.g. "LOAD_CMD_NEWREF") triggers LOAD_CMD_NEWREF handler
6. That handler reads the next line as its arguments — one line shift
7. Cascade: every NEWREF line is now processed incorrectly
8. Eventually NEWREF.word = garbage → Voca → op=0 → terminate

**Presumed root cause:** RA_LOAD_BYTE = LF at MA1 check in LOAD_MA_READARG.
**Source:** not established.

---

## What Was Fixed

✅ Phantom labels (~5 specific instances)
✅ 1000+ broken comment lines
✅ Name conflicts: AC_WRCUR, SC_LOOP/SC_INC/SC_DONE/SC_RET
✅ SETREF dispatch (LOAD_CSS_SETCKJ fallthrough)
✅ BS_FILE_COUNT corruption (JZ builder hit address 328)
✅ K_CURSOR/K_WATERMARK double-deref setup in loader.py
✅ Post-LOAD_MAIN restoration in loader.py (command handlers, system luces)

## What Remains Open

❌ LOAD_MAIN pass2 incomplete — stalls at saku.re
❌ Root cause of RA_LOAD_BYTE = LF at MA1 check
❌ 93 phantom ITOs in files after saku.re (likely disappear once pass2 completes for all files)

---

## Recommendations

### 1. Clean before debugging
Before deep debugging — clean the architecture:
- Remove duplication (identical patterns in different places)
- Verify all comment lines start with `/`
- Add static lint: all EXIT/LINK addresses → `aether[addr+1] != 0`
Clean architecture makes bugs more visible.

### 2. Diagnose correctly
Rule: find the source of the problem via tracing first, then fix.
Do not add a fix until the exact cause is known.

For LOAD_MA_READARG:
```python
# On LOAD_MA_PRELF trigger:
# capture: RA_LOAD_BYTE, last_5_vocas, tokbuf_content, file_byte_offset
```

### 3. Python write-protection — remove or minimise
Current protection in loader.py is too complex and interacts badly.
If needed — only for K_CURSOR, K_WATERMARK, BS_FILE_COUNT (those that should
never change during LOAD_MAIN pass2). Everything else — remove.

### 4. Treat LOAD_MAIN as Reca code
LOAD_MAIN is a Reca program. Its bugs must be found in `.re` code, not in Python.
Every time the urge arises to add a Python hack — ask: "What in the Reca code makes this wrong?"

---

## Pattern N: FOR macro expansion bug (fixed) → root cause of 27 op=0 issues

**Status: FIXED.** Root cause found and fixed in `loader.py` (`_expand_indent`).

### What was broken

`FOR elem0 elem1 ... / body with {N} {X}` was not registered in `_INDENT_LEADERS`
and fell through to the generic `mode='prepend'` handler. That handler prepends
`ctx['name']` (= `parts[1]` of the leader line = the FIRST element of the FOR list)
to every body line:

```
FOR RA_MA0 RA_MA1 RA_MA2 ...
    CLEAR LOAD_MAS_CLR{N} {X}
```

became (via prepend with name='RA_MA0'):

```
CLEAR RA_MA0 LOAD_MAS_CLR{N} {X}
```

Python's `CLEAR name target` handler took `parts[1]='RA_MA0'` as the ITO **name**
and called `_alloc_ito('RA_MA0')`, allocating a fresh ITO at address 601114 and
overwriting `symbols['RA_MA0']`.

`macros.re` is compiled first in Wave-B (macro_first ordering). By that point
`RA_MA0` was still the correct data-lux at address 842. So all `_SPN` macros
(RCALL_AT_SPN, LH_N0_W0, LR_SPN, LT_SPN, WALK_ITO_SPN, RVOCA_SPN, CLEAR_SPN, ...)
baked in `e1=842`. Then saku.re's FOR/CLEAR ran and corrupted `symbols['RA_MA0']`
to 601114. `LOAD_MAS_0_SET: Move El1=LD_MA_RESULT Exit=RA_MA0` (saku.re) wrote
the resolved token address to 601114 — but the `_SPN` macros read from 842
(`aether[842]=0`), so `RA_MC_LUX` always became 0, and all Write operations
targeted offsets from address 0 instead of the actual target lux.

The same bad expansion affected bootstrap.re's `FOR RA_MA1..RA_MA7` blocks, and
parser.re / yaku.re FOR-based initialisation sequences.

### Why only 27 of the 170 issues were fixed

The other 143 remaining `op=0` issues are builder-class macros (`LH`, `LR`, `LT`,
`WALK_ITO`, `RCALL_AT`, `EMIT`, `EMITI`, `PUTBYTE`, etc.) whose Python Wave-B
handler does nothing (they're Reca macros, not in `_BOOTSTRAP_CMDS`). They are
supposed to be wired by `LOAD_MAIN` — but `LOAD_MAIN` stalls at step 1 (see
ROADMAP B6 and Pattern 8). The FOR fix is a prerequisite: now that `RA_MA0`
resolves correctly, the builder-macro dispatch chain is internally consistent and
will work correctly once `LOAD_MAIN` is unblocked.

Verified: manually invoking `LH_N0_W0` (LH's entry point) with MA0=LIM_BODY,
MA1=RA_I, MA2=RA_TMP now correctly wires `LIM_BODY.op=Move, e1=RA_I`. The
mechanism works end-to-end; only LOAD_MAIN activation is missing.

### The fix

`loader.py`, `_expand_indent`: added `mode='for'` handling. When `FOR` is
encountered as a leader line, a `'for'` context is pushed with `elements=parts[1:]`.
For each body line, N copies are emitted with `{N}` → index and `{X}` → element:

```python
if mode == 'for':
    elements = ctx['elements']
    for idx, elem in enumerate(elements):
        expanded = stripped.replace('{N}', str(idx)).replace('{X}', elem)
        result.append((lineno, expanded))
    continue
```

The FOR blocks in `saku.re`, `bootstrap.re`, `parser.re`, `yaku.re` are unchanged
(they were always syntactically correct — just never correctly expanded).

---

## Pattern N+1: PB_FLUSH reads from RA_OB_BASE but PUT_BYTE writes starting at RA_OB_BASE+1 (off-by-one, output is always wrong/zero for first lux)

**Status: NOT FIXED. Found while verifying README.md's example (see git history /
session that fixed README's broken EMIT example). Small, isolated, well-understood.**

`PUT_BYTE`'s first call (RA_OB_SHIFT==0 initially) takes the `PB_ZERO_LUX` path:
`ITO PB_ZERO_LUX Add El1=RA_OB_ADDR El2=C_1 Exit=RA_OB_ADDR` — this ADVANCES
RA_OB_ADDR by 1 BEFORE writing the first byte. So byte 0 ends up at
`RA_OB_BASE+1`, not `RA_OB_BASE`.

`PB_FLUSH` uses `RA_OB_BASE` (unchanged, always points at the original base lux)
as the source address for `SYS_WRITE_PACKED`, with `RA_OB_POS` as the byte count.
Since the actual bytes are at `RA_OB_BASE+1` (and `RA_OB_BASE` itself is never
written, staying 0), `PB_FLUSH` always writes N zero bytes instead of the real
content.

Verified via repl (make_fresh_loader + a small NEWSET/RVOCA EMIT_STR_ENTRY/RVOCA
FLUSH fragment): after EMIT_STR_ENTRY of "Hello, world!\n", RA_OB_POS=8,
aether[RA_OB_BASE]=0, aether[RA_OB_BASE+1]="Hello, w" (packed). FLUSH outputs
8 zero bytes. Same for EMIT_INT_ENTRY(5): outputs 1 zero byte instead of "5".

Likely fix: either (a) PB_ZERO_LUX shouldn't advance RA_OB_ADDR on the very FIRST
byte (only on subsequent lux-boundary crossings), or (b) PB_FLUSH should read from
RA_OB_BASE+1 / track a separate "first real lux" pointer, or (c) RA_OB_ADDR should
be initialized to RA_OB_BASE-1 at freeze time so the first advance lands it on
RA_OB_BASE. Needs a decision on which register's semantics are "correct" per the
header comments in output.re, then a single-line fix + verification that
EMIT_STR/EMIT_INT + FLUSH produce correct bytes for both single-lux (<=8 bytes)
and multi-lux (>8 bytes) strings.

---

## Pattern O: LIMITER_ADOPTED scans all Lux (O(N) at exit) — FIXED

**Status: FIXED.** `K_LIMITER_ADOPTED` flag set at freeze time by loader.py.
`LIMITER_ADOPTED` now reads the flag in O(1) instead of calling `SCAN_ALL_LUX`.

`LIMITER_ADOPTED` (limiter.re) calls `SCAN_ALL_LUX` to determine whether
any Lux has `ACCORD_USE → LIMITER_ACCORD`. This is O(N) over all allocated
Lux. Called from `EXHALE_REPORT` (shioreru.re) once at exit.

For large programs (millions of Lux) this causes a perceptible pause at exit.

**Proper fix:** add a `K_LIMITER_ADOPTED` flag lux (or a small registry).
Set it to 1 when `LINK X ACCORD_USE LIMITER_ACCORD` is first executed.
`LIMITER_ADOPTED` then becomes O(1): just read the flag.

Requires: a hook in accord.re or saku.re when ACCORD_USE lumens are added.

---

## Pattern P: LINK command — different lumen storage model in Python vs Reca

**Status: NOT FIXED. Blocks self-hosting correctness.**

Python `_add_lumen` writes `(rel, exit)` lumen pairs directly into the source
lux at slots `1 + count*2` and `1 + count*2 + 1` (embedded model).

`LOAD_CMD_LINK_IMPL` in `saku.re` allocates a separate 2-luce block for each
`(rel, exit)` pair, then adds it to the source lux (standalone model).

These produce different memory layouts for the same LINK command.
When LOAD_MAIN runs, it will build lumens differently than Python loader,
which will break any code that reads lumens by offset (e.g. `SR_FIRST_LM`,
`SR_NEXT_LM`, `ADD_LUMEN`, `REMOVE_LUMEN`).

**Root cause:** Python `_alloc_data` pre-allocates lumen slots based on
`_lumen_prepass` count. Reca allocates each lumen pair on demand.

**Proper fix:** either align Python to allocate lumen pairs separately,
or align Reca to write lumens embedded in the source lux (pre-allocated).
The latter matches the current Python model and avoids fragmentation.

---

## Pattern Q: Yaku Voca/Redi templates missing call-stack operations

**Status: NOT FIXED. Affects compiled output correctness.**

The Yaku LLVM IR templates for `Voca` and `Redi` do not emit the automatic
call-stack push/pop that the interpreter performs via `RA_SP`.

`Voca` template: emits `store RA_LINK=NXT_N; br` — missing `RA_SP -= FRAME_SIZE;
aether[RA_SP] = old_RA_LINK` (push).

`Redi` template: emits `load RA_LINK; br` — missing `RA_SP += FRAME_SIZE;
RA_LINK = aether[RA_SP]` (pop).

**Impact:** compiled functions with nested calls will corrupt the call stack,
causing incorrect returns for any call depth > 1.

**Fix needed:** add `RA_SP_ID` and `RA_FRAME_SIZE_ID` escape bytes to the
template substitution system, then update Voca/Redi templates to emit
the stack push/pop using inline GEP reads/writes.

See: `compile/yaku.re` LATENT ISSUE comment lines 98-105.
