# REFERENCE.md — Reca Macro and Register Reference

---

## Part I — Load-time Macros

Load-time macros are Reca programs in `macros.re` that the loader executes during Wave-B
instead of a Python inline handler. Every `.re` command except the primitives (`NEW`, `SET`, `ITO`…)
is dispatched to the corresponding macro entry point.

**How it works:** the loader writes resolved token addresses into `RA_MA0..RA_MA7`,
then calls the macro via `Voca`. The macro builds ITO luces directly in Aether.

---

### Instruction synthesis

#### `RVOCA name sub`
```
→ ITO name Voca El1=sub Exit=RA_LINK
```
Build a Voca lux. `name` = label, `sub` = called subroutine.

#### `RREDI name`
```
→ ITO name Redi El1=RA_LINK
```
Build a Redi lux (return). Resets `_last_ito` = 0 (chain terminator).

#### `CLEAR name target`
```
→ ITO name Move El1=C_0 Exit=target
```
Zero out `target`.

#### `NOP name`
```
→ ITO name Move El1=C_0 Exit=C_0
```
No-op lux. Used as placeholder or `_K` lux in JEQ.

---

### Conditional branches

#### `JEQ name a b dest`
```
→ ITO name   Equal   El1=a   El2=b   Exit=RA_JEQ_FLAG
  ITO name_J JumpIf  El1=RA_JEQ_FLAG Exit=dest
  ITO name_K Move    El1=C_0         Exit=C_0
```
Three luces. Jumps to `dest` if `a == b`. Auto-suffix: MA4=`_J`, MA5=`_K`.

#### `JZ name cond dest`
```
→ ITO name   Equal   El1=cond El2=C_0  Exit=RA_JEQ_FLAG
  ITO name_J JumpIf  El1=RA_JEQ_FLAG   Exit=dest
  ITO name_K Move    El1=C_0           Exit=C_0
```
Jumps to `dest` if `cond == 0`. Auto-suffix: MA3=`_J`, MA4=`_K`.

---

### Functions

`FUNC`/`ENDFUNC` and the `CS_PUSH`/`CS_POP`/`CS_SAVE_REG`/`CS_REST_REG`
subroutines they generated have been **removed** (zero callers project-wide).
RA_LINK is saved/restored automatically by the call stack on every
`Voca`/`Redi` — see `aria/regs.re`. A normal function is just:
```
NEWREF name body_start
NOLINK
ITO body_start ...   /first real instruction, becomes the entry point/
...
RREDI name_ret       /→ ITO name_ret Redi El1=RA_LINK/
```
No special entry/exit macro is needed; nested and recursive calls work
correctly at any depth via `RA_SP`.

---

### Call stack

`RCALL`/`RRET` (an inline call stack using a fixed 1024-entry array,
`BS_CS_BUF_000`/`BS_CS_SP`) have been **removed** — zero callers project-wide.
The automatic call stack (`RA_SP`, pushed/popped by every `Voca`/`Redi`, see
`aria/regs.re`) replaced this entirely.

#### `RCALL_AT name sub landing`
```
→ ITO name   Move landing → RA_LINK
  ITO name_J Jump Exit=sub
```
Call with explicit landing (resets `_last_ito` = 0).

**KNOWN BUG (see BUGS.md "LOAD_CU_BUILDER dispatch path is dead code"):**
`name` currently comes out as `op=0` (unwired/"phantom") at every call site —
the macro body above is never actually executed by the loader. Treat
RCALL_AT-based code paths as non-functional until this is fixed.

---

### Graph (lumens)

#### `LINK_OP name src rel exit`
```
→ ITO name   Move src  → RA_LM_SRC
  ITO name_R Move rel  → RA_LM_REL
  ITO name_T Move exit → RA_LM_EXIT
  ITO name_J Voca ADD_LUMEN Exit=RA_LINK
```
Adds lumen `src --rel--> exit`. 4 luces. Auto-suffix: MA4=`_R`, MA5=`_T`, MA6=`_J`.

---

### Graph traversal (Walk)

#### `WALK_ONE name lux rel`
```
→ ITO name_LUX Move lux → RA_SR_LUX
  ITO name_REL Move rel → RA_SR_REL
  ITO name_OFF Move C_1 → RA_SR_OFFSET
  ITO name     Voca SR_WALK_ONE Exit=RA_LINK
```
Finds first lumen with `rel` at `lux`. Result in `RA_SR_OUT`.
Auto-suffix: MA3=`_LUX`, MA4=`_REL`, MA5=`_OFF`.

#### `LX name src exit_lux`
```
→ ITO name   Move src → RA_SR_LUMEN
  ITO name_J Voca SR_GLX Exit=RA_LINK
  ITO name_W Move RA_SR_LUMEN → exit_lux
```
Advance to next lumen. Auto-suffix: MA3=`_J`, MA4=`_W`.

#### `LH name src exit_lux`
```
→ ITO name_OFF Move C_1 → RA_SR_OFFSET
  ITO name     Move src → RA_SR_LUX
  ITO name_J   Voca SR_GLL Exit=RA_LINK
  ITO name_W   Move RA_SR_LUMEN → exit_lux
```
Read head (first lumen). Auto-suffix: MA3=`_J`, MA4=`_W`, MA5=`_OFF`.

#### `LR name src exit_lux`
```
→ ITO name   Move src → RA_SR_LUMEN
  ITO name_J Voca SR_GLR Exit=RA_LINK
  ITO name_W Move RA_SR_REL → exit_lux
```
Read relation field of a lumen.

#### `LT name src exit_lux`
Same as LR but via SR_GLE (reads exit field).

---

### Output

**NOTE:** `EMIT`, `EMITI`, and `PUTBYTE` are builder-class macros routed through
`LOAD_CU_BUILDER` — which is currently dead code (see BUGS.md "LOAD_CU_BUILDER
dispatch path is dead code"). Their generated primary luces (`name`) come out as
`op=0` at every call site. Additionally, `PB_FLUSH`/`PUT_BYTE` have an off-by-one
that produces wrong output bytes even when called directly (see BUGS.md
"PB_FLUSH reads from RA_OB_BASE but PUT_BYTE writes starting at RA_OB_BASE+1").
Treat output macros as non-functional until both bugs are fixed.

#### `EMIT name arg`
```
→ ITO name   Move arg → RA_TW_LUX
  ITO name_R Voca EMIT_STR_ENTRY Exit=RA_LINK
  ITO name_K Move C_0 → C_0
```
Print packed string. Auto-suffix: MA2=`_R`, MA3=`_K`.

#### `EMITI name arg`
```
→ ITO name   Move arg → RA_TMP2
  ITO name_R Voca EMIT_INT_ENTRY Exit=RA_LINK
  ITO name_K Move C_0 → C_0
```
Print integer.

#### `PUTBYTE name arg`
```
→ ITO name   Move arg → RA_BYTE
  ITO name_R Voca PUT_BYTE Exit=RA_LINK
  ITO name_K Move C_0 → C_0
```
Print one byte.

---

### Packed string builder

#### `NEXO name "template"`
Build a packed string from a string literal. Processes `{X}` as placeholders.
Writes address of first packed lux into `aether[name]`.

#### `YAKU_NEXO name`
Like NEXO, but the template is read as indented body lines (multi-line template).

#### `YAKU_NEXO_TERM name`
YAKU_NEXO + adds lumen `name --Terminates--> Yaku`.

#### `YAKU_NEXO_CMP name`
YAKU_NEXO + adds lumen `name --HasCmpResult--> Yaku`.

#### `YAKU_NEXO_ARITH name`
YAKU_NEXO + adds lumen `name --HasArithResult--> Yaku`.

#### `YAKU_NEXO_ALIAS name alias`
YAKU_NEXO_TERM for `name` + adds lumen `alias --ForType--> name`.

---

### Iteration and templates

#### `SWITCH RA_REG`
```
  value_1  dest_1
  value_2  dest_2
```
Reads indented lines, generates JEQ luces for each value.
Line syntax: `VAL DEST` or `V1 V2 > DEST` (multiple values → same dest).

#### `FOR elem1 elem2 ...`
```
  {X}_action_one
  {X}_action_two
```
Reads indented body as template, expands for each element. `{X}` → element, `{N}` → index.

#### `SAVE reg1 reg2 ...`
```
  ...body...
```
Emits save-ITO before body and restore-ITO after.

---

### Primitive commands (Wave-A/B Python, not macros)

| Command | Action |
|---------|--------|
| `NEW name` | Allocate a Lux (word=0). |
| `NEWREF name [ref]` | Allocate a Lux; self-ref if ref omitted, else cross-ref. |
| `NEWSET name value` | NEW + SET. Value is integer or string. |
| `SETREF name ref` | Set word(name) = addr(ref). |
| `SET name integer` | Set word(name) = integer. |
| `LINK src rel exit` | Add lumen `src --rel--> exit`. |
| `ITO name op e1 e2 exit` | Allocate an ITO lux. |
| `BLOCK name count` | Allocate count raw luces. |
| `NOLINK` | Reset auto-link (`_last_ito` = 0). |

---

### Internal subroutines (not called from `.re` directly)

**AUTOLINK** — if `RA_MC_PREV != 0`, writes `RA_MC_LUX` into `RA_MC_PREV.next` (slot 5),
then sets `RA_MC_PREV = RA_MC_LUX`.

**AUTOLINK_RESET** — like AUTOLINK, but sets `RA_MC_PREV = 0` after (chain terminator).

**WIRE_AUTOLINK** — writes op/e1/e2/exit from `RA_MC_OP/E1/E2/DEST` into the lux at `RA_MC_LUX`, then calls AUTOLINK.

**WIRE_AUTOLINK_RESET** — like WIRE_AUTOLINK, but calls AUTOLINK_RESET instead.

**WRITE_ITO_SLOTS** — writes all 5 ITO slots (word, op, e1, e2, exit) from `RA_MC_LUX/OP/E1/E2/DEST`
into aether, then calls WIRE_AUTOLINK. Used by JEQ, JZ, LX, LH, WALK_ONE, LINK_OP to avoid
repeating the 10-line slot-writing pattern. Uses `RA_MC_WIS_RL` as its own RA_LINK scratch
so it is safe to call from any macro that already uses `RA_MC_TMP_RL`.

---

### Auto-suffix convention

The loader automatically creates suffixed names for secondary luces:

| Suffix | Typical use |
|--------|-------------|
| `_J` | JumpIf lux in JEQ/JZ; Voca lux in LINK_OP |
| `_K` | NOP lux in JEQ/JZ (fall-through) |
| `_R` | Voca lux in EMIT/EMITI/PUTBYTE |
| `_W` | Move-result in LX/LH/LR |
| `_OFF` | Offset lux in LH/WALK_ONE |
| `_LUX`, `_REL`, `_OFF` | Setup luces in WALK_ONE |

---

## Part II — Registers

Registers in Reca are ordinary luces with an `RA_` prefix. They are declared as `NEW` (word=0)
or `NEWREF` (word=own_addr or cross-ref). None have hardware meaning —
they are shared luces that subroutines read and write by convention.

Registers are **not preserved** across calls unless the caller saves them explicitly.
All `RA_` registers below are global to the entire Aether.

---

### Core registers (`regs.re`)

| Register | Description |
|----------|-------------|
| `RA_LINK` | Return address. Voca pushes the old value onto the call stack (RA_SP), then writes `nxt_pc` here; Redi reads and jumps, then pops the saved value back. NEWSET to 0. |
| `RA_SP` | Automatic call-stack pointer, grows downward. Pushed/popped by every Voca/Redi. Initialised to `K_STACK_TOP`. |
| `RA_TMP` | Scratch 1 — general purpose. |
| `RA_TMP2` | Scratch 2. |
| `RA_TMP3` | Scratch 3. |
| `RA_TMP4` | Scratch 4. |
| `RA_FLAG` | Boolean result for JumpIf (usually from Equal/Less). |
| `RA_I` | Walk index; sequential traversal. Also used by IRIS_DUMP — callers must not rely on it surviving. |
| `RA_BYTE` | Current byte (PUT_BYTE, BS_READ_BYTE). |
| `RA_RET2` | Second return address (when two levels of return are needed). |
| `RA_TW_LUX` | Current Lux during byte-chain or instruction traversal. |
| `RA_TW_BYTE` | Byte read from word of current Lux. |
| `RA_TW_LUMEN` | Current lumen address during lumen-list traversal. |
| `RA_SAL_CUR` | Cursor for SCAN_ALL_LUX — not overwritten by nested calls. |

---

### Macro arguments and result (`registers.re`)

The loader sets these before calling each load-time macro.

| Register | Description |
|----------|-------------|
| `RA_MA0` | Argument 0 — address of first token after the command. |
| `RA_MA1` | Argument 1. |
| `RA_MA2` | Argument 2. |
| `RA_MA3` | Argument 3. |
| `RA_MA4` | Argument 4 (typically auto-suffix `_J`). |
| `RA_MA5` | Argument 5 (auto-suffix `_K`). |
| `RA_MA6` | Argument 6. |
| `RA_MA7` | Argument 7. |
| `RA_MA_RET` | Return value from load-time subroutines (address of new lux, 0 = error). |

**Pointer registers** (NEWREF — their word = target address, not self-ref):

| Register | Points to |
|----------|-----------|
| `RA_LINK_REF` | `RA_LINK` — used in macros as `Write El2=RA_LINK_REF` |
| `RA_C0_REF` | `C_0` |
| `RA_SR_LUX_REF` | `RA_SR_LUX` |
| `RA_SR_REL_REF` | `RA_SR_REL` |
| `RA_SR_OUT_REF` | `RA_SR_OUT` |
| `RA_SR_LUMEN_REF` | `RA_SR_LUMEN` |
| `RA_SR_OFFSET_REF` | `RA_SR_OFFSET` |
| `RA_TW_LUX_REF` | `RA_TW_LUX` |
| `RA_TMP2_REF` | `RA_TMP2` |
| `RA_BYTE_REF` | `RA_BYTE` |
| `RA_LM_SRC_REF` | `RA_LM_SRC` |
| `RA_LM_REL_REF` | `RA_LM_REL` |
| `RA_LM_EXIT_REF` | `RA_LM_EXIT` |

**Special:**

| Register | Description |
|----------|-------------|
| `RA_JEQ_FLAG` | Shared flag lux for JEQ/JZ — Equal writes 0 or 1; JumpIf reads. |
| `RA_JEQ_FLAG_PTR` | Immutable pointer to `RA_JEQ_FLAG`. Written once at init. |

---

### Loader / tokenizer (`bootstrap.re`, `registers.re`)

| Register | Description |
|----------|-------------|
| `RA_LOAD_FD` | File descriptor of the currently loading file. |
| `RA_LOAD_RPOS` | Read position in the buffer. |
| `RA_LOAD_RLEN` | Bytes available in the buffer. |
| `RA_LOAD_BYTE` | Last byte read (0 = EOF). |
| `RA_LOAD_TLEN` | Length of the last token in bytes. |
| `RA_LOAD_HASH` | djb2 hash of the current token. |
| `RA_LOAD_INDENT` | Current indent level (0 = none, 1 = one level). |
| `RA_BS_RESULT` | Result of BS_INTERN / BS_LOOKUP (lux address or 0). |
| `RA_BS_EL0..3` | Resolved elements 0–3. |
| `RA_BS_ELC` | Count of recognized elements. |
| `RA_BS_FLAG` | Boolean result inside bootstrap. |
| `RA_BS_TMP` / `RA_BS_TMP2` / `RA_BS_TMP3` | Scratch for bootstrap. |
| `RA_BS_FIDX` | Index into file list. |
| `RA_BS_FPATH` | Packed-string address of current file path. |
| `RA_BS_PACK_WORD` | Accumulates bytes when packing a string (up to 8 per lux). |
| `RA_BS_PACK_SHIFT` | Bit-shift during packing (0, 8, 16, …, 56). |
| `RA_BS_PACK_SIDX` | Source index in tokbuf (0..tlen-1). |
| `RA_BS_PACK_DST` | Destination pointer in Aether during packing. |
| `RA_BS_POS_SLOT` | Next positional slot (1..7) for ITO arguments. |

---

### Allocator and lumens (`alloc.re`)

| Register | Description |
|----------|-------------|
| `RA_ALLOC_COUNT` | Number of luces to allocate. |
| `RA_ALLOC_RESULT` | Address of first allocated lux. |
| `RA_LM_SRC` | IN for ADD_LUMEN / REMOVE_LUMEN: source lux address. |
| `RA_LM_REL` | IN: relation lux address (0 = untyped). |
| `RA_LM_EXIT` | IN: target lux address. |
| `RA_FLUX_BOTTOM` | Lower bound of flux zone. Written at freeze. |
| `RA_FLUX_TOP` | Upper bound of flux zone. |
| `RA_ALLOC_TMP` / `RA_ALLOC_TMP2` / `RA_ALLOC_TMP3` | Scratch for allocator. |
| `RA_AN_NLUMENS` | Requested lumen count for ALLOC_LUX_N. |
| `RA_AL_POS` | Current position when scanning lumens (rel lux). |
| `RA_RL_POS` | Position when removing a lumen. |

---

### Call stack (`callstack.re`)

The call stack is automatic — every `Voca`/`Redi` pushes/pops `RA_LINK` on
`RA_SP`. No subroutines or per-function bookkeeping needed (CS_PUSH, CS_POP,
CS_SAVE_REG, CS_REST_REG, and the FUNC/ENDFUNC macros that used them have all
been removed — zero callers). `callstack.re` now only declares the supporting
registers:

| Register | Description |
|----------|-------------|
| `RA_FRAME_SIZE` | Frame size in luces (= 8). Written at freeze. Read by the interpreter on every Voca/Redi. |
| `RA_STACK_GUARD` | Lower stack bound (STACK_BOTTOM). Written at freeze. Reserved for future overflow detection (not yet checked). |
| `RA_CS_TMP` | General scratch register (also used by bootstrap.re). |

---

### Symphony / graph traversal (`symphony.re`)

| Register | Description |
|----------|-------------|
| `RA_SR_LUX` | IN: lux address to traverse. |
| `RA_SR_POS` | IN/OUT: current position (rel slot of a lumen pair). |
| `RA_SR_OFFSET` | IN: offset to first lumen (1 for Data, ITO_SIZE for ITO). |
| `RA_SR_REL` | IN (SR_WALK_ONE): relation to find / OUT (SR_GLR): relation found. |
| `RA_SR_OUT` | OUT: exit lux address, or 0 if not found. |
| `RA_SR_LUMEN` | Current lumen during WALK. |
| `RA_SR_WO_REL` | Scratch for WALK_ONE (saved relation). |
| `RA_SR_CNT` | Counter during traversal. |

---

### Macro builder (`macros.re`)

Used inside load-time macro programs. Do not access directly from user code.

| Register | Description |
|----------|-------------|
| `RA_MC_LUX` | Address of the lux currently being built. |
| `RA_MC_PREV` | Address of previous lux (for AUTOLINK). Synced with `loader._last_ito`. |
| `RA_MC_SLOT` | `lux_addr + offset` when writing slots. |
| `RA_MC_TMP_RL` | Saved RA_LINK before calling AUTOLINK. |
| `RA_MC_WIS_RL` | Saved RA_LINK inside WRITE_ITO_SLOTS (separate from RA_MC_TMP_RL). |
| `RA_MC_FLAG` | Address of flag lux (for JEQ/JZ). |
| `RA_MC_J` / `RA_MC_K` | Scratch: second / third lux (`_J`, `_K` suffixes). |
| `RA_MC_OP` / `RA_MC_E1` / `RA_MC_E2` / `RA_MC_DEST` | Scratch for WIRE_AUTOLINK. |
| `RA_SW_REG` / `RA_SW_IDX` / `RA_SW_DEST` / `RA_SW_AROW` | SWITCH macro internals. |
| `RA_FOR_IDX` / `RA_FOR_ELEM` / `RA_FOR_BODY` | FOR macro internals. |
| `RA_FOR_IRIS_BUF` / `RA_FOR_IRIS_PTR` / `RA_FOR_IRIS_LEN` / `RA_FOR_IRIS_IDX_BASE` | FOR name buffer. |
| `RA_SV_REGS` / `RA_SV_COUNT` | SAVE macro internals. |

---

### Hash table (`htable.re`)

| Register | Description |
|----------|-------------|
| `RA_HT_BASE` | Start address of hash table. |
| `RA_HT_HASH` | Hash for lookup/insert. |
| `RA_HT_SIZE` | Table size (power of two). |
| `RA_HT_MASK` | `size - 1` for fast modulo. |
| `RA_HT_RESULT` | OUT: address of found lux or 0. |
| `RA_HT_LID` | IN: lux ID for insert. |
| `RA_HT_SLOT` | Scratch: current slot. |
| `RA_HT_WORD` | Scratch: word read from slot. |
| `RA_HT_STORED_H` | Scratch: stored hash in slot. |
| `RA_HT_PROBE` | Probe counter. |

---

### Output (`output.re`)

| Register | Description |
|----------|-------------|
| `RA_OB_BASE` | Start of output buffer. |
| `RA_OB_POS` | Current write position (byte level). |
| `RA_OB_SHIFT` | Bit-shift within current Lux (0, 8, …, 56). |
| `RA_OB_ADDR` | Address of current Lux in buffer. |
| `RA_OB_FD` | File descriptor for output (NEWSET = 1 = stdout). |
| `RA_DS_BASE` | Decimal string buffer (start). |
| `RA_DS_POS` | Position in decimal string buffer. |
| `RA_PR_WORD` / `RA_PR_SHIFT` | Scratch for packed-string print. |

---

### Yaku compiler (`yaku.re`)

| Register | Description |
|----------|-------------|
| `RA_ENTRY_LUX` | Current Entry→Yaku lux of the function being compiled. |
| `RA_INSTR` | Current instruction being compiled. |
| `RA_OP_ID` | Primitive ID of the current instruction. |
| `RA_RULE` | `Yaku_X` lux for the current primitive. |
| `RA_NEXT_ID` | Current K_CURSOR value (next free address). |
| `RA_SSA_CTR` | Monotonic SSA register counter (reset to 0 at function start). |
| `RA_SSA_V1` / `RA_SSA_V2` | Pointers to SSA slots for El1 and El2 of current instruction. |
| `RA_SSA_OUT` | SSA index of result. |
| `RA_SSA_RESULT` | Pointer to result slot. |
| `RA_SSA_FR` / `RA_SSA_FR2` | Pointers to fresh SSA slots. |
| `RA_SSA_CMP` | Saved SSA result of last HasCmpResult operator. |
| `RA_SSA_ARITH_IDX` / `RA_SSA_ARITH_TGT` | Fusion: index/target of last HasArithResult. |
| `RA_A1` / `RA_A2` / `RA_EXIT_N` / `RA_NXT_N` | Lux IDs for El1/El2/Exit/Next of current instruction. |
| `RA_RT_BASE` | Base of rule-lookup table (op_id → Yaku_X). |
| `RA_EP_BUF_BASE` / `RA_EP_BUF_CNT` / `RA_EP_IDX` | Entry-point buffer. |
| `RA_PREAM_BASE` | Start address of preamble in output buffer. |
| `RA_VS_WORDS` | Word count in visited bitset. |
| `RA_SSA_BASE` / `RA_SSA_GEN` / `RA_SSA_LID` / `RA_SSA_HIT` | SSA direct-map array. |

---

### Specialized arias

**BFS** (`bfs.re`): `RA_BQ_BASE`, `RA_BQ_HEAD`, `RA_BQ_TAIL` (queue); `RA_VS_BASE`, `RA_VS_ID` (visited set); `RA_FW_ENTRY/CUR/REL/RESULT` (frontier walk).

**Flux** (`flux.re`): `RA_FX_SRC`, `RA_FX_TYPE`, `RA_FX_POS`, `RA_FX_CODE`, `RA_FX_VAL`, `RA_FX_SLOT`, `RA_FX_TYPE_POS`, `RA_FX_RESULT`, `RA_FX_FIND`, `RA_FX_COUNT`.

**Chain** (`tether.re`): `RA_CHAIN_HEAD`, `RA_CHAIN_OFF`, `RA_CHAIN_TAIL`, `RA_CHAIN_CUR`, `RA_CHAIN_NXT`.

**Dimensions** (`runtime/dimensions.re (removed — forward declaration only)`): `RA_VIEW_LUX`, `RA_VIEW_DIM`, `RA_VIEW_OUT`, `RA_VIEW_TMP`.

**Fmt** (`fmt.re`): `RA_FMT_STR`, `RA_FMT_ARGS_HEAD`, `RA_FMT_ARG_CUR`, `RA_FMT_BYTE`.

**Select** (`select.re`): `RA_SEL_COND`, `RA_SEL_A`, `RA_SEL_B`, `RA_SEL_OUT`, `RA_SEL_RET`.

**Sort** (`sort.re`): `RA_SORT_BASE`, `RA_SORT_LEN`, `RA_SORT_I`, `RA_SORT_J`, `RA_SORT_KEY`, `RA_SORT_TMP`, `RA_SORT_TMP2`, `RA_SORT_ADDR`.

**Ring** (`ring.re`): `RA_RING_HEAD`, `RA_RING_TAIL`, `RA_RING_COUNT`, `RA_RING_VAL`, `RA_RING_TMP`, `RA_RING_TMP2`.

**Scheduler** (`scheduler.re`): `RA_SCHED_LUX`, `RA_SCHED_IDX`, `RA_SCHED_NEXT`, `RA_SCHED_TID`, `RA_SCHED_STACK`.

**Parser** (`parser.re`): `RA_JTL_*` — jump table for LINK commands; `RA_JT_*` — main parser dispatch table.

**Iris / trace** (`iris.re`): `RA_IRIS_BUF`, `RA_IRIS_SIZE`.

**Accord / scan** (`accord.re`): `RA_SCAN_BODY`, `RA_SCAN_STOP`, `RA_SCAN_LUX`.

**Strip comments** (`comments.re`): `RA_STRIP_IN`, `RA_STRIP_OUT`.

---

### Key K_ constants

`K_CURSOR`, `K_WATERMARK`, `K_TRACE_POS`, `K_STACK_TOP` are `NEW` declarations in
`alloc.re`, filled in by `loader.py` at freeze time. `K_AETHER_SIZE` and `K_XLEN`
are preamble constants generated by `gen_compiler.py` for the compiler/LLVM layer.
Do not edit any of these manually — they are (re)written automatically.

| Constant | Description |
|----------|-------------|
| `K_CURSOR` | Next free address in Aether (bump pointer). |
| `K_STACK_TOP` | Initial value of RA_SP. |
| `K_TRACE_POS` | Address of trace buffer, or 0 (tracing disabled). |
| `K_WATERMARK` | High-water mark: boundary between code-space and runtime-space. |
| `K_AETHER_SIZE` | Total Aether array size in luces. |
| `K_XLEN` | Target word width in bits. |
| `K_AETHER_SIZE` | Total Aether array size in luces. |
| `K_XLEN` | Target word width in bits. |
