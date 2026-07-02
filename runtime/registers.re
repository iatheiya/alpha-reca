// runtime/registers.re — Shared registers used across subroutines and the Exire register protocol (platform-agnostic)

This file is the single home for all globally-shared registers: syscall
protocol, runtime scratch, the automatic call stack, walk registers,
macro-generator scratch, _REF aliases, and loader I/O state. (Previously
split across registers.re and a separate regs.re — merged into one file
since the split added no real boundary, just two near-identical names.)

── REGISTER CLASSES ─────────────────────────────────────────

VOLATILE SCRATCH  (clobbered by any subroutine call)
RA_TMP  RA_TMP2  RA_TMP3  RA_TMP4  RA_FLAG
A subroutine may read and write these freely.
A caller must NOT rely on their values surviving a sub-call.

RETURN ADDRESS  (volatile; each subroutine uses RA_LINK as its return)
RA_LINK.word holds the return address (next Lux ID after the Voca Lux).
Initialised to 0: Redi with RA_LINK=0 halts execution cleanly.
Calling convention (native Voca/Redi):
caller: RVOCA name SUB  →  Voca El1=SUB (sets RA_LINK = next-pc)
subroutine: ends with RREDI name  →  Redi (jumps to word(RA_LINK))
The call stack (RA_SP) is automatic: Voca pushes the old RA_LINK onto
aether[RA_SP] (RA_SP -= RA_FRAME_SIZE) before overwriting it; Redi pops
aether[RA_SP] back into RA_LINK (RA_SP += RA_FRAME_SIZE) after jumping.
This makes nested/recursive calls correct at any depth with zero
per-function bookkeeping — no manual save/restore Lux needed.

remote landing: RCALL_AT name SUB landing  →  pushes old RA_LINK onto the
call stack (same mechanism as Voca above), then Move landing→RA_LINK;
Jump SUB. SUB's Redi goes to `landing` instead of next instruction, and
correctly pops the frame this call just pushed. (This is the INTENDED
design — see macros.re's RCALL_AT definition. Fixed to push explicitly
so it stays stack-compatible with RREDI-based callees.)

NOTE: RCALL_AT's `name` ITO (and all builder-class macros: LH, LR, LT,
WALK_ITO, EMIT, etc.) come out as op=0 at every call site. Root cause:
LOAD_MAIN does not yet run to completion (self-hosting 0%) — builder-class
macros are wired by LOAD_MAIN, not by Python Wave-B. They are therefore unwired
until LOAD_MAIN is unblocked (see ROADMAP B6 and BUGS.md Pattern N).
RA_LINK is platform-agnostic — the Reca canonical name for the return
address slot. It maps to x30/LR on AArch64, x1/ra on RISC-V, etc.,
but in Reca it is an explicit Lux, not a hidden register.

RA_RET2 is a GENERAL PURPOSE extra return slot — free for any caller.

GENERAL LOOP COUNTER
RA_I  — generic loop variable; also used by IRIS_DUMP to save
aspect_id across EMIT_INT_ENTRY calls (RA_I is safe there
because IRIS_DUMP's outer loop uses RA_TMP as its index).
Callers of IRIS_DUMP must not rely on RA_I surviving.

BYTE BUFFER
RA_BYTE  — the byte value passed to PUT_BYTE

WALK REGISTERS  (sequential traversal)
RA_TW_LUX    — current Lux in a byte-chain or instruction walk
RA_TW_LUMEN  — current Lumen pool address in a Lumen-list walk

MODULE-PRIVATE REGISTERS
Subroutine modules declare their own private registers with a module
prefix (e.g. RA_SR_* in symphony.re, RA_LM_* in alloc.re, RA_HT_* in
htable.re, RA_RING_* in ring.re). These are not global scratch —
callers of a module must not use that module's private registers,
and must assume them clobbered after any call into that module.

── SYSCALL REGISTERS ────────────────────────────────────────
SC_NR, SC_A0..SC_A3 hold syscall elements as plain word values.
"Set" via Move before Exire; Python reads aether[SC_X_id] directly.
Declared with NEW (no SETREF needed).

WHAT THIS IS:
  The abstract contract between Reca code and the Exire aspect.
  SC_NR holds the syscall number. SC_A0..SC_A3 hold elements.
  SC_A0 also receives the return value after Exire executes.

WHAT THIS IS NOT:
  This is NOT the OS calling convention (that lives in aria/syscall/linux_*.re).
  This is NOT the ABI for function calls (that lives in aria/abi/*.re).
  The names SC_NR/SC_A0..SC_A3 are canonical Reca names — they do not
  imply X0/X8 (ARM64) or a0/a7 (RISC-V). Those are lowering details.

DEPENDENCY: aspects.re, core/constants.re//

NEWSET RA_LINK 0

── Call stack ──────────────────────────────────────────────────────────────
//RA_SP = stack pointer (index into Aether stack region at top of Aether).
Grows downward: PUSH decrements SP by FRAME_SIZE before writing.
POP reads then increments SP by FRAME_SIZE.
Initial value set at freeze time by loader to K_STACK_TOP.word.
Stack region: [STACK_BOTTOM .. STACK_TOP] — separate from bump-alloc region.//
NEW RA_SP

── Volatile scratch ──────────────────────────────────────────────────────
NEW RA_TMP
NEW RA_TMP2
NEW RA_TMP3
NEW RA_TMP4
NEW RA_FLAG
SETREF RA_FLAG RA_FLAG  /self-ref flag for Equal→JumpIf

NEW RA_SAL_CUR  /SCAN_ALL_LUX cursor — dedicated, not clobbered by VS_TEST_SET or other subs

NEW RA_RET2

NEW RA_BYTE
NEW RA_I

── Walk registers: used for sequential traversal ──────────────────────────
NEW RA_TW_LUX     /current Lux in a byte-chain or instruction traversal
NEW RA_TW_LUMEN   /current Lumen address in a Lumen-list traversal

NEW SC_NR   /syscall number — set before Exire instruction
NEW SC_A0   /element 0 / return value
NEW SC_A1   /element 1
NEW SC_A2   /element 2
NEW SC_A3   /element 3

//Macro element registers — used during freeze only (load-time macro dispatch).
Python writes els here before execute_aether(MACRO_ADDR); Reca reads them.//
NEW RA_MA0    /macro arg 0 (first token after command, resolved to addr)
NEW RA_MA1    /macro arg 1
NEW RA_MA2    /macro arg 2
NEW RA_MA3    /macro arg 3
NEW RA_MA4    /macro arg 4
NEW RA_MA5    /macro arg 5
NEW RA_MA6    /macro arg 6
NEW RA_MA7    /macro arg 7
NEW RA_MA_RET /macro return value (addr of newly built lux, etc.)
NEWREF RA_LINK_REF RA_LINK  /word = addr(RA_LINK); use in macros via Write El2=RA_LINK_REF
NEW RA_JEQ_FLAG  /shared flag for JEQ/JZ macros — OK since Equal→JumpIf is always sequential
SETREF RA_JEQ_FLAG RA_JEQ_FLAG  /self-ref: Equal writes to this lux, JumpIf reads it
NEW RA_JEQ_FLAG_PTR  /immutable pointer: word = addr(RA_JEQ_FLAG). Written once by loader init.
NEWREF RA_C0_REF C_0   /word = addr(C_0); use in macros via Write El2=RA_C0_REF

── RCALL_AT push-mechanism _REF aliases ──────────────────────────────────
//Needed so RCALL_AT's generated code can address RA_SP/RA_RCA_TMP/RA_RCA_TMP2
RA_FRAME_SIZE as instruction operands (El1/El2/Exit), not just read their values.//
NEWREF RA_SP_REF         RA_SP          /word = addr(RA_SP)
NEWREF RA_RCA_TMP_REF    RA_RCA_TMP     /word = addr(RA_RCA_TMP)
NEWREF RA_RCA_TMP2_REF   RA_RCA_TMP2    /word = addr(RA_RCA_TMP2)
NEWREF RA_FRAME_SIZE_REF RA_FRAME_SIZE  /word = addr(RA_FRAME_SIZE)

── _REF symbols for macro Write El2= usage ──────────────────────────────────
/word = addr(target); macros use Write El2=X_REF to write addr into a slot

NEWREF RA_SR_LUX_REF    RA_SR_LUX
NEWREF RA_SR_REL_REF    RA_SR_REL
NEWREF RA_SR_OUT_REF    RA_SR_OUT
NEWREF RA_SR_LUMEN_REF  RA_SR_LUMEN
NEWREF RA_SR_OFFSET_REF RA_SR_OFFSET
NEWREF RA_TW_LUX_REF    RA_TW_LUX
NEWREF RA_TMP2_REF      RA_TMP2
NEWREF RA_BYTE_REF      RA_BYTE

── Loader I/O registers (used by saku.re and macro programs) ──────────────
/These are global so SWITCH/FOR/SAVE programs can access file state.
NEW RA_LOAD_FD       /current file descriptor being loaded
NEW RA_LOAD_RPOS     /read position in load buffer
NEW RA_LOAD_RLEN     /bytes available in load buffer
NEW RA_LOAD_BYTE     /last byte read from load buffer
NEW RA_LOAD_INDENT   /current indentation level (0=none, 1=one, ...)
NEW RA_LOAD_TLEN     /token length in bytes
NEW RA_LOAD_HASH     /token hash (djb2)
NEW RA_LOAD_RESULT   /result of LOAD_INTERN_TOKEN: interned lux addr

── BS_READ_BYTE redirect mode ──────────────────────────────────────────────
//When RA_REDIRECT_BASE != 0, BS_READ_BYTE reads from this buffer instead of
the live file stream (RA_LOAD_RPOS/RA_LOAD_RLEN are left completely
untouched), advancing RA_REDIRECT_POS independently. Used by
LOAD_DISPATCH_BUILT_LINE so that already-constructed text (from
CHAIN/FOR/SAVE body processing) is what gets tokenized, instead of
whatever the live file stream's position happens to be at -- see
BUGS.md for the corruption this caused before redirect mode existed.//
NEW RA_REDIRECT_BASE /0 = off (normal live-stream reads); else = buffer addr
NEW RA_REDIRECT_POS  /read position within the redirect buffer
NEW RA_REDIRECT_LEN  /valid byte count in the redirect buffer
