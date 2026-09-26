//
============================================================
aria/ring.re — Dynamic circular queue backed by a Lux-linked ring
All subroutines use native Voca/Redi via RA_LINK.
The call stack (RA_SP) is automatic — no per-function bookkeeping needed.

DEPENDENCY: aspects.re, core/constants.re, runtime/registers.re,
aria/lumen.re, runtime/alloc.re
//

SET RING_NEXT_REL

NEW RA_RING_HEAD
NEW RA_RING_TAIL
NEW RA_RING_COUNT
NEW RA_RING_VAL
NEW RA_RING_TMP
NEW RA_RING_TMP2

── RING_INIT ─────────────────────────────────────────────────
/Non-leaf (calls ALLOC_TO, LINK_OP).
NOLINK
ALLOC_TO RING_INIT Exit=RA_RING_HEAD Count=C_2
NOVA RI_TAIL Op=Move El1=RA_RING_HEAD Exit=RA_RING_TAIL
CLEAR RI_CNT El1=RA_RING_COUNT
LINK_OP RI_LINK RA_RING_HEAD RING_NEXT_REL RA_RING_HEAD
RREDI RI_RET_r

── RING_PUSH ─────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
WALK_ONE RING_PUSH RA_RING_TAIL RING_NEXT_REL
/RA_SR_OUT = RING_NEXT(TAIL)
JEQ RP_AFTER_WALK El1=RA_SR_OUT El2=RA_RING_HEAD Exit=RING_PUSH_GROW
NOVA RP_WRITE Op=Write El1=RA_RING_TAIL El2=RA_RING_VAL
NOVA Op=Move El1=RA_SR_OUT Exit=RA_RING_TAIL
NOVA Op=Add El1=RA_RING_COUNT El2=C_1 Exit=RA_RING_COUNT

RREDI RP_RET_r

── RING_PUSH_GROW ────────────────────────────────────────────
NOLINK
NOVA RING_PUSH_GROW Op=Move El1=RA_SR_OUT Exit=RA_RING_TMP
ALLOC_TO RP_GR_NEW Exit=RA_RING_TMP2 Count=C_2
UNLINK_OP RP_GR_UNL RA_RING_TAIL RING_NEXT_REL RA_RING_TMP
LINK_OP RP_GR_LK1 RA_RING_TAIL RING_NEXT_REL RA_RING_TMP2
LINK_OP RP_GR_LK2 RA_RING_TMP2 RING_NEXT_REL RA_RING_TMP
NOVA RP_GR_WRITE Op=Write El1=RA_RING_TAIL El2=RA_RING_VAL
NOVA Op=Move El1=RA_RING_TMP2 Exit=RA_RING_TAIL
NOVA Op=Add El1=RA_RING_COUNT El2=C_1 Exit=RA_RING_COUNT

RREDI RP_GR_RET_r

── RING_POP ──────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
JEQ RING_POP El1=RA_RING_HEAD El2=RA_RING_TAIL Exit=RP_EMPTY
NOVA RP_READ Op=Read El1=RA_RING_HEAD Exit=RA_RING_VAL
WALK_ONE RP_ADV RA_RING_HEAD RING_NEXT_REL
NOVA RP_ADV4 Op=Move El1=RA_SR_OUT Exit=RA_RING_HEAD
NOVA RP_CNTD Op=Sub El1=RA_RING_COUNT El2=C_1 Exit=RA_RING_COUNT
RREDI RP_RET2_r

CLEAR RP_EMPTY El1=RA_RING_VAL
RREDI RP_EMRET_r

── RING_FULL ─────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
WALK_ONE RING_FULL RA_RING_TAIL RING_NEXT_REL
NOVA RFL_AFTER Op=Equal El1=RA_SR_OUT El2=RA_RING_HEAD Exit=RA_FLAG
RREDI RFL_RET_r

── RING_EMPTY ────────────────────────────────────────────────
/Leaf.
NOLINK
NOVA RING_EMPTY Op=Equal El1=RA_RING_HEAD El2=RA_RING_TAIL Exit=RA_FLAG
RREDI REM_RET
