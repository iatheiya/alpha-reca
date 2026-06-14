//runtime/regs.re — Shared registers used across subroutines

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

remote landing: RCALL_AT name SUB landing  →  Move landing→RA_LINK; Jump SUB.
SUB's Redi goes to `landing` instead of next instruction. (This is the
INTENDED design — see macros.re's RCALL_AT definition.)

NOTE: RCALL_AT's `name` ITO (and all builder-class macros: LH, LR, LT,
WALK_ITO, EMIT, etc.) come out as op=0 at every call site. Root cause:
LOAD_MAIN does not yet run to completion (self-hosting 0%) — builder-class
macros are wired by LOAD_MAIN, not by Python Wave-B. They are therefore unwired
until LOAD_MAIN is unblocked (see ROADMAP B6 and BUGS.md Pattern N).
The 143 remaining op=0 issues are all of this class. Manually invoking
the macro entry points with correct MA0..MA7 works correctly (verified).
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
RA_TW_BYTE   — byte value read from RA_TW_LUX.word (low 8 bits)
RA_TW_LUMEN  — current Lumen pool address in a Lumen-list walk

MODULE-PRIVATE REGISTERS
Subroutine modules declare their own private registers with a module
prefix (e.g. RA_SR_* in symphony.re, RA_LM_* in alloc.re, RA_HT_* in
htable.re, RA_RING_* in ring.re). These are not global scratch —
callers of a module must not use that module's private registers,
and must assume them clobbered after any call into that module.

── SYSCALL REGISTERS ────────────────────────────────────────
SC_NR, SC_A0..SC_A3 (from runtime/registers.re) hold syscall
elements as plain word values. Set via Move before Exire; Python
reads aether[SC_X_id] directly. Declared with NEW (no SETREF needed).

DEPENDENCY: aspects.re, core/constants.re, runtime/registers.re//

NEWSET RA_LINK 0

/── Call stack ────────────────────────────────────────────────────────────────
/RA_SP = stack pointer (index into Aether stack region at top of Aether).
/Grows downward: PUSH decrements SP by FRAME_SIZE before writing.
/POP reads then increments SP by FRAME_SIZE.
/Initial value set at freeze time by loader to K_STACK_TOP.word.
/Stack region: [STACK_BOTTOM .. STACK_TOP] — separate from bump-alloc region.
NEW RA_SP

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

/Walk registers: used for sequential traversal
NEW RA_TW_LUX     /current Lux in a byte-chain or instruction traversal
NEW RA_TW_BYTE    /byte value read from a Lux word (low 8 bits)
NEW RA_TW_LUMEN   /current Lumen address in a Lumen-list traversal
