//callstack.re — Automatic call stack registers

The call stack (RA_SP) is automatic: Voca pushes the old RA_LINK onto
aether[RA_SP] (RA_SP -= RA_FRAME_SIZE) before overwriting it; Redi pops
it back (RA_SP += RA_FRAME_SIZE) after jumping. See runtime/regs.re for the
full description. This file declares the registers that mechanism uses.

STACK LAYOUT (grows downward in Aether):
  [STACK_TOP]            ← initial RA_SP (set at freeze time via K_STACK_TOP)
  [RA_SP]                ← always points to the CURRENT frame slot
  [RA_SP + 0]            ← saved RA_LINK (pushed by Voca: SP -= FRAME_SIZE, then write)
  [RA_SP - FRAME_SIZE]   ← next pushed frame (after another Voca call)

Constants: FRAME_SIZE=8 (symphony.py), set into RA_FRAME_SIZE at freeze time.
RA_SP, K_STACK_TOP, RA_FRAME_SIZE, RA_STACK_GUARD values are all set at
freeze time (loader.py _setup_load_aspects). RA_STACK_GUARD is reserved
for future overflow detection (not yet checked).

DEPENDENCY: aspects.re  core/constants.re  runtime/regs.re  runtime/alloc.re//

NEW RA_FRAME_SIZE  /= 8 (FRAME_SIZE); set at freeze time
NEW RA_STACK_GUARD /= STACK_BOTTOM; set at freeze time; for overflow detection
NEW RA_CS_TMP      /general scratch register (also used by bootstrap.re)
