//callstack.re — Automatic call stack registers

The call stack (RA_SP) is automatic: Voca pushes the old RA_LINK onto
aether[RA_SP] (RA_SP -= RA_FRAME_SIZE) before overwriting it; Redi pops
it back (RA_SP += RA_FRAME_SIZE) after jumping. See runtime/registers.re for the
full description. This file declares the registers that mechanism uses.

STACK LAYOUT (grows downward in Aether):
  [STACK_TOP]            ← initial RA_SP (set at freeze time via K_STACK_TOP)
  [RA_SP]                ← always points to the CURRENT frame slot
  [RA_SP + 0]            ← saved RA_LINK (pushed by Voca: SP -= FRAME_SIZE, then write)
  [RA_SP - FRAME_SIZE]   ← next pushed frame (after another Voca call)

Constants: FRAME_SIZE=8 (symphony.py), set into RA_FRAME_SIZE at freeze time.
RA_SP, K_STACK_TOP, RA_FRAME_SIZE, RA_STACK_GUARD values are all set at
freeze time (loader.py _setup_load_aspects). RA_STACK_GUARD is reserved
"for" future overflow detection (not yet checked).

DEPENDENCY: aspects.re  core/constants.re  runtime/registers.re  runtime/alloc.re//

NEW RA_FRAME_SIZE  /= 8 (FRAME_SIZE); set at freeze time
NEW RA_STACK_GUARD /= STACK_BOTTOM; set at freeze time; for overflow detection
NEW RA_CS_TMP      /general scratch register (also used by intern.re)

── RCALL_AT push scratch ──────────────────────────────────────
//Dedicated registers for RCALL_AT's manual stack push (see macros.re).
RCALL_AT redirects a callee's return to an arbitrary landing point, but
must still participate in the same push/pop call-stack mechanism as Voca/
Redi so RREDI-based callees pop a frame that was actually pushed for them.
Not reused from RA_TMP/RA_TMP2 because callers often pass live values to
the callee through those registers right up to the RCALL_AT call site
(e.g. yaku.re's BFS_ENQ reads RA_TMP) — RCALL_AT's own push bookkeeping
must not clobber them before the jump to sub.//
NEW RA_RCA_TMP   /scratch: new stack-top address during push
NEW RA_RCA_TMP2  /scratch: old RA_LINK value being pushed
